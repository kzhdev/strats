#include "quoting.hpp"

#include <cassert>
#include <cmath>
#include <limits>

namespace tachyon::trade::mm {

// ---------------------------------------------------------------------------
// Pricing block (legacy :436-446)
// ---------------------------------------------------------------------------

void quote_engine::refresh_market(contract_state& s, uint32_t c, uint64_t now_cycles) const noexcept {
    const level& bid = s.side_book(side::bid).top();
    const level& ask = s.side_book(side::ask).top();

    // Legacy updates the established level only when both sides show size (:437-440); the
    // value is the average of the two top sizes.
    if (bid.qty > 0 && ask.qty > 0) {
        update_established_level(s, c, static_cast<double>(bid.qty + ask.qty) / 2.0, now_cycles);
    }

    s.mid_point = mid_point(s, c);

    // Legacy :444-446: width = (spread / 2) * percentage, truncated toward zero.
    const double spread = static_cast<double>(ask.price - bid.price);
    s.width = static_cast<price_t>(spread / 2.0 * s.width_pct);
}

price_t quote_engine::mid_point(const contract_state& s, uint32_t c) const noexcept {
    const level& bid = s.side_book(side::bid).top();
    const level& ask = s.side_book(side::ask).top();

    // Legacy weights the mid by the top sizes (clipped to the established level) when the
    // spread is exactly one tick, and takes the plain mid otherwise (:913-918).
    if (bid.price + m_.tick_size[c] == ask.price) {
        const qty_t ask_size = std::min(ask.qty, s.established_level);
        const qty_t bid_size = std::min(bid.qty, s.established_level);
        const qty_t total = bid_size + ask_size;
        if (total != 0) [[likely]] {
            return (ask.price * bid_size + bid.price * ask_size) / total;
        }
        // Legacy divides by zero here (both clipped sizes are 0 only when the established
        // level is 0, which its own clamp makes impossible). Defensive fall-through instead of
        // a SIGFPE; documented in PARITY_NOTES §4 as unreachable in practice.
    }
    return (bid.price + ask.price) / 2;
}

void quote_engine::update_established_level(contract_state& s, uint32_t c, double tob_qty,
                                            uint64_t now_cycles) const noexcept {
    const uint64_t period = m_.est_level_ema_period[c];

    // Legacy (:924-926) guards on `_lastEmaTime && _estLevelEmaPeriod > 0`: the first
    // observation only seeds the time and the last quantity, it does not move the EMA.
    if (s.last_ema_time != 0 && period > 0) {
        const uint64_t elapsed = now_cycles - s.last_ema_time;
        const double alpha = static_cast<double>(elapsed) / static_cast<double>(period);
        const double mu = std::exp(-alpha);
        const double nu = (1.0 - mu) / alpha;
        const double last_ema = s.ema_est_level;
        const double new_ema =
            mu * last_ema + (nu - mu) * s.last_tob_qty + (1.0 - nu) * tob_qty;
        if (std::isfinite(new_ema)) {
            s.ema_est_level = new_ema;
            // Legacy: max<qty_t>(_emaEstLevel * _estLevelPctEma, 1), i.e. truncate then clamp.
            const qty_t scaled = static_cast<qty_t>(s.ema_est_level * m_.est_level_pct_ema[c]);
            s.established_level = scaled > 1 ? scaled : 1;
        }
    }

    s.last_tob_qty = tob_qty;
    s.last_ema_time = now_cycles;
}

// ---------------------------------------------------------------------------
// Size helpers (legacy :105-141, :356-361, :741-753, :1748-1790)
// ---------------------------------------------------------------------------

qty_t quote_engine::max_quote_size_based_on_market(uint32_t c, side sd, const level& book,
                                                   double pct, price_t price) const noexcept {
    const bool at_or_through = sd == side::bid ? price >= book.price : price <= book.price;
    if (!at_or_through) return std::numeric_limits<qty_t>::max();
    return static_cast<qty_t>(static_cast<double>(book.qty) * pct);
}

qty_t quote_engine::internal_quote_size(const contract_state& s, uint32_t c, side sd,
                                        qty_t max_size) const noexcept {
    const qty_t max_pos = m_.max_pos_internal[c];
    if (sd == side::bid) {
        if (s.internal_pos < 0) return std::min(max_pos, max_size);
        if (max_pos <= s.internal_pos) return 0;
        return std::min(max_pos - s.internal_pos, max_size);
    }
    if (s.internal_pos > 0) return std::min(max_pos, max_size);
    if (max_pos <= -s.internal_pos) return 0;
    return std::min(max_pos + s.internal_pos, max_size);
}

qty_t quote_engine::external_quote_size(const contract_state& s, uint32_t c, side sd,
                                        qty_t quote_size, qty_t max_size) const noexcept {
    const qty_t max_pos = m_.max_pos_external[c];
    if (sd == side::bid) {
        if (s.external_pos < 0) return std::min(quote_size, std::min(max_pos, max_size));
        if (max_pos <= s.external_pos) return 0;
        return std::min(quote_size, std::min(max_pos - s.external_pos, max_size));
    }
    if (s.external_pos > 0) return std::min(quote_size, std::min(max_pos, max_size));
    if (max_pos <= -s.external_pos) return 0;
    return std::min(quote_size, std::min(max_pos + s.external_pos, max_size));
}

qty_t quote_engine::candidate_external_size(const contract_state& s, uint32_t c, side sd,
                                            qty_t size, qty_t max_size) const noexcept {
    qty_t candidate = m_.valid_qty(c, external_quote_size(s, c, sd, size, max_size));
    const quote_state& q = s.quote_of(sd);
    // Legacy :747-751: a decrease is always allowed; an increase smaller than one quote size
    // is suppressed while the candidate is above min(quoteSize, market cap).
    if (candidate > q.last_external_size_sent) {
        if ((candidate - q.last_external_size_sent) < m_.quote_size[c] &&
            candidate > std::min(m_.quote_size[c], max_size)) {
            candidate = q.last_external_size_sent;
        }
    }
    return candidate;
}

qty_t quote_engine::spray_quantity(const contract_state& s, uint32_t c, side sd,
                                   qty_t current_qty, const level& book,
                                   price_t external_price) const noexcept {
    // Legacy getSprayQty (:866-872): one more quote size on top of what is already at the
    // level, capped by the market and by the incremental-size rule, and never negative.
    qty_t size = current_qty + m_.quote_size[c];
    const qty_t max_by_market =
        max_quote_size_based_on_market(c, sd, book, m_.pct_tob_max_quote[c], external_price);
    size = candidate_external_size(s, c, sd, size, max_by_market);
    const qty_t spray = size - current_qty;
    return spray > 0 ? spray : 0;
}

bool quote_engine::enough_size(side sd, const level& book, qty_t threshold,
                               price_t price) noexcept {
    // Legacy :135 / :140, keeping its bitwise operators (no short-circuit).
    if (sd == side::bid) {
        return ((price >= book.price) & (book.qty >= threshold)) | (price < book.price);
    }
    return ((price <= book.price) & (book.qty >= threshold)) | (price > book.price);
}

bool quote_engine::in_payup(uint32_t c, qty_t our_size, const level& book) const noexcept {
    // One transcription, shared with the hedger (book.hpp).
    return ::tachyon::trade::mm::in_payup(m_.payup_qty[c], m_.payup_orders[c],
                                          m_.payup_multiple[c], our_size, book);
}

price_t quote_engine::worsen_price(side sd, price_t price, price_t amount) noexcept {
    // Legacy calculateWorsenPrice (:76-87).
    return sd == side::bid ? price - amount : price + amount;
}

price_t quote_engine::payup_internal_price(side sd, price_t ordinary, price_t worsened) noexcept {
    // Legacy calculatePayUpInternalPrice (:89-100).
    return sd == side::bid ? std::min(ordinary, worsened) : std::max(ordinary, worsened);
}

// ---------------------------------------------------------------------------
// Quote construction (legacy :603-628, :661-739, :756-864)
// ---------------------------------------------------------------------------

namespace {

// The S-curve's exponential. MM_FAST_EXP selects a polynomial 2^k * exp(r) approximation
// (~1e-7 relative) instead of the libm call; the gate measures both (PLAN.md §0 D6).
#if defined(MM_FAST_EXP) && MM_FAST_EXP
inline double queue_exp(double x) noexcept {
    constexpr double kInvLn2 = 1.4426950408889634;
    constexpr double kLn2 = 0.6931471805599453;
    const double k = static_cast<double>(static_cast<int>(x * kInvLn2 + (x >= 0 ? 0.5 : -0.5)));
    const double r = x - k * kLn2;  // |r| <= ~0.35
    const double p = 1.0 + r * (1.0 + r * (0.5 + r * (1.0 / 6.0 +
                              r * (1.0 / 24.0 + r * (1.0 / 120.0)))));
    return std::ldexp(p, static_cast<int>(k));
}
#else
inline double queue_exp(double x) noexcept { return std::exp(x); }
#endif

}  // namespace

quote_engine::scaled_price quote_engine::price_at_queue_position(const contract_state& s,
                                                                 uint32_t c, side sd,
                                                                 price_t price,
                                                                 qty_t queue_position,
                                                                 double inv_established) const noexcept {
    // Legacy :783-787, transcribed. pctThroughQueue is 1 when nothing is in front of us and 0
    // at the front of the queue, clamped to [0, 1].
    //
    // MM_HOIST_EST_DIV replaces the per-order `qp / establishedLevel` division with a multiply
    // by a reciprocal computed once per price_of() call; identical when the reciprocal is exact,
    // at most one ulp otherwise (PARITY_NOTES §4).
#if defined(MM_HOIST_EST_DIV) && MM_HOIST_EST_DIV
    const double pct_through_queue = std::max(
        std::min(1.0 - static_cast<double>(queue_position) * inv_established, 1.0), 0.0);
#else
    const double pct_through_queue =
        std::max(std::min(1.0 - static_cast<double>(queue_position) /
                                    static_cast<double>(s.established_level), 1.0), 0.0);
#endif
    // Logistic map from queue position to a [-1, 1] price-offset scale.
    const double offset_scale =
        2.0 / (1.0 + queue_exp(-static_cast<double>(m_.queue_importance[c]) *
                               (pct_through_queue - 0.5))) - 1.0;
    // Legacy assigns the double product to an int64, which truncates toward zero.
    const price_t offset = static_cast<price_t>(offset_scale * static_cast<double>(s.width));
    const price_t internal_price = apply_price_offset(price, offset, sd);
    return {internal_price, m_.round(c, internal_price, sd)};
}

quote_engine::price_and_size quote_engine::price_of(const contract_state& s, const order_queue& q,
                                                    uint32_t c, side sd, price_t price,
                                                    bool quote_externally) const noexcept {
    // Legacy :757-760: with external quoting off there is nothing to evaluate — the price is
    // passed through and the contract's quote size is used as-is.
    if (!quote_externally) {
        return {price, m_.round(c, price, sd), m_.quote_size[c]};
    }

    price_t target_price = m_.round(c, price, sd);
    const book& bk = s.side_book(sd);
    // Legacy :762-766: never quote better than the touch; snap to it and take the commission
    // off the internal price instead.
    if (is_better_price(target_price, bk.top().price, sd)) {
        target_price = bk.top().price;
        price = apply_price_offset(target_price, m_.commission[c], sd);
    }

    // Legacy :768-773: the two documented bypasses.
    if (s.established_level == 0 || m_.queue_importance[c] == 0) {
        return {price, target_price, m_.quote_size[c]};
    }

    // Legacy :794-795: the queue depth at the target level, less the implied quantity our
    // orders cannot be behind.
    const int32_t target_level = bk.level_index(target_price);
    const qty_t target_size =
        target_level == -1 ? 0 : bk.tracked_qty(static_cast<uint32_t>(target_level));

    const double inv_established = 1.0 / static_cast<double>(s.established_level);

    // Legacy :796-808: nothing resting at the target price yet, so assume we join at the back.
    if (q.first(sd, target_price) == nullptr) {
        const scaled_price at_back =
            price_at_queue_position(s, c, sd, price, target_size, inv_established);
        if (is_better_price(target_price, at_back.external_price, sd)) {
            // Even the back of the queue prices worse than the target: stay there with no size
            // rather than quote through the level we wanted.
            return {price, target_price, 0};
        }
        if (at_back.external_price != target_price) {
            // The evaluated price is better than the target: stay on the target level.
            return {price, target_price, m_.quote_size[c]};
        }
        return {at_back.internal_price, at_back.external_price, m_.quote_size[c]};
    }

    // Legacy :810-832: price the resting orders from their queue positions, in insertion order.
    // An order that would price worse than the target stops the walk — everything behind it is
    // unwanted too.
    price_t cost = 0;
    qty_t qty = 0;
    bool order_unwanted = false;
    for (const order_queue::slot* slot = q.first(sd, target_price); slot != nullptr;
         slot = q.next(*slot)) {
        // :815-819: a queue position the exchange has not sent yet is assumed to be at the back.
        const qty_t queue_position = slot->qp == order_queue::NOT_SET ? target_size : slot->qp;
        const scaled_price sp =
            price_at_queue_position(s, c, sd, price, queue_position, inv_established);
        if (is_better_or_equal(sp.external_price, target_price, sd)) {
            cost += sp.internal_price * slot->qty;
            qty += slot->qty;
        } else {
            order_unwanted = true;
            break;
        }
    }

    // Legacy :834-851: if the last of our orders has enough room behind it, spray one more at
    // the back of the level.
    const order_queue::slot* last = q.last(sd, target_price);
    if (!order_unwanted && last != nullptr && last->qp >= 0) {
        const qty_t qty_behind = target_size - last->qp;
        if (qty_behind >= m_.quote_gap[c]) {
            const scaled_price at_back =
                price_at_queue_position(s, c, sd, price, target_size, inv_established);
            if (is_better_or_equal(at_back.external_price, target_price, sd)) {
                const qty_t spray = spray_quantity(s, c, sd, qty, bk.top(), at_back.external_price);
                if (spray) {
                    cost += at_back.internal_price * spray;
                    qty += spray;
                }
            }
        }
    }

    // Legacy :852-864: the size-weighted internal price, then the same three-way decision.
    // `cost / qty` is a truncating division (PLAN.md §4 site 6).
    if (qty != 0) {
        const price_t internal_price = legacy_trunc(cost / qty);
        const price_t external_price = m_.round(c, internal_price, sd);
        if (is_better_price(target_price, external_price, sd)) {
            return {price, target_price, 0};
        }
        if (external_price != target_price) {
            return {price, target_price, qty};
        }
        return {internal_price, external_price, qty};
    }
    return {price, target_price, 0};
}

bool quote_engine::update_side(contract_state& s, const order_queue& q, uint32_t c, side sd,
                               bool top_book_changed, bool quote_externally, bool force_update,
                               intent_log* log) const noexcept {
    // Legacy :605: nothing is recalculated while the maxTicksWide latch is set.
    if (s.max_ticks_wide_breached) return false;

    if (top_book_changed || force_update) {
        // Either side's top change can move the weighted mid, so both sides recalculate
        // (:608-611).
        return build_quote(s, q, c, sd, quote_externally, force_update, log);
    }
    if (!quote_externally) return false;

    // Deeper change: only a change at or in front of our own quote level matters (:612-624).
    // quote_level is -1 when the quote is better than the touch or not in the book.
    const book& bk = s.side_book(sd);
    const int32_t quote_level = bk.level_index(s.quote_of(sd).external_price);
    if (bk.price_change_index() != book::NO_CHANGE) {
        if (bk.qty_change_index() <= quote_level) {
            return build_quote(s, q, c, sd, quote_externally, false, log);
        }
    } else if (bk.qty_change_index() == quote_level) {
        return build_quote(s, q, c, sd, quote_externally, false, log);
    }
    return false;
}

bool quote_engine::build_quote(contract_state& s, const order_queue& q, uint32_t c, side sd,
                               bool quote_externally, bool force_update,
                               intent_log* log) const noexcept {
    const level& book = s.side_book(sd).top();
    quote_state& q_state = s.quote_of(sd);

    // Legacy :663: the ordinary price is the mid moved by the width, away from the touch.
    const price_t price = sd == side::bid ? s.mid_point - s.width : s.mid_point + s.width;

    price_and_size ps = price_of(s, q, c, sd, price, quote_externally);
    price_t internal_price = ps.internal_price;
    price_t external_price = ps.external_price;
    qty_t size = ps.size;

    qty_t max_by_market = max_quote_size_based_on_market(c, sd, book, m_.pct_tob_max_quote[c],
                                                         external_price);
    // Legacy :667 decides the payup mode from the size we could actually quote externally.
    const bool payup_mode = in_payup(c, external_quote_size(s, c, sd, size, max_by_market), book);

    bool change_in_price = false;

    if (!payup_mode) {
        // Legacy :674-682: the change test compares against the price we last *sent*. The
        // difference is taken overflow-free (see abs_diff in types.hpp).
        change_in_price = abs_diff(internal_price, q_state.last_internal_price_sent) >
                          static_cast<uint64_t>(m_.min_internal_price_change[c]);
        q_state.internal_price = internal_price;
        if (quote_externally) q_state.external_price = external_price;
    } else {
        // Legacy :683-699: in payup the internal price is worsened to the touch (less the
        // configured allowance) and the external price is that internal price rounded.
        const price_t ordinary = sd == side::bid ? s.mid_point - s.width : s.mid_point + s.width;
        const price_t worsened = worsen_price(sd, book.price, m_.worsen_payup_amount[c]);
        internal_price = payup_internal_price(sd, ordinary, worsened);
        const price_t external_worsened = m_.round(c, internal_price, sd);
        if (external_price != external_worsened) {
            // The worsened price sits on a different level: re-evaluate the size there.
            ps = price_of(s, q, c, sd, internal_price, quote_externally);
            internal_price = ps.internal_price;
            external_price = ps.external_price;
            size = ps.size;
        }
        change_in_price = internal_price != q_state.internal_price;
        q_state.internal_price = internal_price;
        if (quote_externally) q_state.external_price = external_worsened;
    }

    // ---- size (legacy :700-737) ----
    bool change_in_size = false;
    max_by_market = max_quote_size_based_on_market(c, sd, book, m_.pct_tob_max_quote[c],
                                                   q_state.external_price);
    if (quote_externally) {
        if (q_state.external_size == 0) {
            // Join only once the book shows the configured minimum size at our price.
            if (enough_size(sd, book, m_.join_size[c], q_state.external_price)) {
                q_state.external_size = candidate_external_size(s, c, sd, size, max_by_market);
                change_in_size |= q_state.external_size != 0;
            }
        } else if (!enough_size(sd, book, m_.leave_size[c], q_state.external_price)) {
            q_state.external_size = 0;  // leave the book
            change_in_size = true;
        } else {
            const qty_t candidate = candidate_external_size(s, c, sd, size, max_by_market);
            if (candidate != q_state.external_size) {
                q_state.external_size = candidate;
                change_in_size = true;
            }
        }
    } else {
        change_in_size |= q_state.external_size != 0;
        q_state.external_size = 0;
    }

    const qty_t candidate_internal = m_.valid_qty(c, internal_quote_size(s, c, sd, max_by_market));
    if (candidate_internal != q_state.internal_size) {
        q_state.internal_size = candidate_internal;
        change_in_size = true;
    }

    if (change_in_price | change_in_size | force_update) {
        if (log != nullptr) {
            emit_quote(*log, c, sd, q_state, quote_externally);
        } else {
            // No sink: still record what would have been sent, so a host-less run is exact.
            q_state.last_internal_price_sent = q_state.internal_price;
            q_state.last_external_size_sent = q_state.external_size;
        }
        return true;
    }
    return false;
}

void quote_engine::emit_quote(intent_log& log, uint32_t c, side sd, quote_state& q,
                              bool persist) const noexcept {
    // Legacy modifyQuoteOrder (:878-891): one request per changed side, then remember the sent
    // values so the next change detection compares against them.
    log.add_quote(c, sd, q.internal_price, q.internal_size, q.external_price, q.external_size,
                  persist);
    q.last_internal_price_sent = q.internal_price;
    q.last_external_size_sent = q.external_size;
}

void quote_engine::cross_quote(contract_state& s, intent_log& log, uint32_t c,
                               bool quote_externally, bool& bid_changed,
                               bool& ask_changed) const noexcept {
    // Legacy :513-537: when exactly one side changed, the *other* side may now cross it, so it is
    // forced out with the price it already has.
    if ((bid_changed && ask_changed) || (!bid_changed && !ask_changed)) return;

    if (!bid_changed) {
        quote_state& bid = s.quote_of(side::bid);
        const quote_state& ask = s.quote_of(side::ask);
        if (bid.last_internal_price_sent != tachyon::NULL_PRICE &&
            ask.internal_price != tachyon::NULL_PRICE &&
            bid.last_internal_price_sent >= ask.internal_price) {
            emit_quote(log, c, side::bid, bid, quote_externally);
            bid_changed = true;
        }
        return;
    }

    quote_state& ask = s.quote_of(side::ask);
    const quote_state& bid = s.quote_of(side::bid);
    if (ask.last_internal_price_sent != tachyon::NULL_PRICE &&
        bid.internal_price != tachyon::NULL_PRICE &&
        ask.last_internal_price_sent <= bid.internal_price) {
        emit_quote(log, c, side::ask, ask, quote_externally);
        ask_changed = true;
    }
}

}  // namespace tachyon::trade::mm
