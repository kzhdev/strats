#include "host.hpp"

namespace tachyon::trade::as {
namespace {

const char* kMaxTicks = "AS MaxTicksThroughMarketCheckFailed";
const char* kUnableToHedge = "AS Unable to hedge. No valid path.";

}  // namespace

host::host(const model& m, cycle_state& s, liquidity_ledger* ledger,
           const host_ports& ports) noexcept
    : m_(m),
      s_(s),
      bridge_storage_(ledger != nullptr ? std::make_unique<ledger_bridge>(*ledger, m.n_all)
                                        : nullptr),
      bridge_(bridge_storage_.get()),
      ports_(ports),
      // The collaborators keep the policy *by value*, so it is fully populated here, in the
      // member-init list, before they are constructed -- they are not assignable (they hold
      // references) and that is deliberate: a half-wired engine cannot exist.
      policy_(build_policy(bridge_, s_)),
      paths_(m_, policy_),
      quotes_(m_, paths_, policy_),
      hedger_(m_, paths_, policy_) {
    venue_enabled_.fill(1);
}

host::~host() = default;

quote_policy host::build_policy(ledger_bridge* bridge, cycle_state& s) noexcept {
    if (bridge == nullptr) return quote_policy{};
    bridge->set_book_size_source(&host::book_size_at, &s);
    return ledger_bridge::make_policy(*bridge);
}

qty_t host::book_size_at(void* ctx, uint32_t contract, side sd, bool external) noexcept {
    return static_cast<cycle_state*>(ctx)->book(contract, sd, external).size;
}

void host::wire_market(uint32_t contract, side sd, bool external, market_key key,
                       liquidity_source source) noexcept {
    if (bridge_ != nullptr) bridge_->set_market(contract, sd, external, key, source);
}

void host::set_venue_enabled(uint32_t venue, bool on) noexcept {
    if (venue < venue_enabled_.size()) venue_enabled_[venue] = on ? 1 : 0;
}

void host::enable(bool on) noexcept {
    enabled_ = on;
    if (!on) {
        disable(nullptr);
        return;
    }
    // legacy handleEnabled(): a re-enable re-quotes everything from scratch
    for (uint32_t c = 0; c < m_.n_all; ++c) {
        if (!m_.is_quoting(c)) continue;
        s_.mark_dirty(c);
        s_.set_force_update(c, side::bid, true);
        s_.set_force_update(c, side::ask, true);
    }
}

void host::on_market_data(uint32_t contract, side sd, bool external, price_t price,
                          qty_t size) noexcept {
    if (contract >= m_.n_all) return;
    if (!m_.is_quoting(contract) && m_.affected_count(contract, sd) == 0) return;

    // legacy captures the commission-adjusted best price *before* the book moves, because the
    // difference is what the paths apply as a delta (:1126)
    const price_t old_best = s_.best_price_adjusted(m_, contract, sd);

    bool changed;
    if (external) {
        changed = s_.update_external(policy_, contract, sd, price, size);
    } else {
        s_.update_internal(contract, sd, price, size);
        changed = true;  // legacy treats an internal update as a change unconditionally (:288)
    }
    if (!changed) return;
    if (!s_.refresh_best(m_, policy_, contract, sd, external)) return;
    paths_.on_best_market_update(s_, contract, sd, old_best);
}

void host::on_fill(uint32_t contract, side sd, qty_t qty, bool is_internal) noexcept {
    if (contract >= m_.n_all || qty <= 0) return;

    // legacy handleFill: re-quote this contract immediately, then hedge
    s_.set_force_update(contract, sd, true);
    s_.mark_dirty(contract);

    if (!enabled_) {
        // legacy forwards the fill to the market maker when AS is disabled
        if (ports_.forward_fill != nullptr) ports_.forward_fill(ports_.ctx, contract, sd, qty);
        return;
    }

    const hedge_plan plan = hedger_.hedge(s_, contract, sd, qty, is_internal);
    if (!plan.hedged) {
        ++stats_.unhedged_fills;
        if (ports_.on_alert != nullptr) ports_.on_alert(ports_.ctx, kUnableToHedge);
        if (ports_.forward_fill != nullptr) ports_.forward_fill(ports_.ctx, contract, sd, qty);
        return;
    }

    // legacy sendFillToFront() runs *before* the hedge orders go out
    if (ports_.send_fill != nullptr) {
        ports_.send_fill(ports_.ctx, contract, sd, qty, NULL_PRICE);
    }

    ++stats_.hedges_sent;
    for (uint8_t i = 0; i < plan.leg_count; ++i) {
        const hedge_leg& leg = plan.legs[i];
        ++stats_.hedge_legs;
        if (ports_.send_hedge == nullptr) continue;
        hedge_order order;
        order.contract = contract;
        order.hedge_contract = leg.contract;
        order.sd = leg.order_side;
        order.internal_price = leg.internal_price;
        order.external_price = leg.external_price;
        order.size = leg.size;
        order.external = leg.external;
        order.hedge_id = plan.hedge_id;
        order.market_index = leg.market_index;
        order.target = bridge_ != nullptr ? bridge_->key_at(leg.market_index) : 0;
        order.path_id = plan.path_id;
        ports_.send_hedge(ports_.ctx, order);
    }

    if (plan.switch_transfer) {
        ++stats_.switch_transfers;
        if (ports_.send_switch_fill != nullptr) ports_.send_switch_fill(ports_.ctx, plan);
    }

    // legacy reserves the FAK quantity only after the hedge orders are out (:686-691)
    reserve_hedge_legs(plan);
}

void host::reserve_hedge_legs(const hedge_plan& plan) noexcept {
    // the lean's own paths must see the reduced liquidity either way
    for (uint8_t i = 0; i < plan.leg_count; ++i) s_.mark_dirty(plan.legs[i].contract);
    if (bridge_ == nullptr) return;
    for (uint8_t i = 0; i < plan.leg_count; ++i) {
        const hedge_leg& leg = plan.legs[i];
        if (bridge_->reserve_hedge(leg.market_index, leg.size, plan.hedge_id)) {
            ++stats_.reservations_taken;
        }
    }
}

void host::on_hedge_ack(uint32_t hedge_id, market_key target, ord_id_t order_id,
                        qty_t actual) noexcept {
    if (bridge_ != nullptr) bridge_->ack_hedge(hedge_id, target, order_id, actual);
}

void host::on_hedge_done(uint32_t hedge_id, market_key target) noexcept {
    if (bridge_ != nullptr) bridge_->complete_hedge(hedge_id, target);
}

// ---------------------------------------------------------------------------
// short-term fairs
// ---------------------------------------------------------------------------
void host::on_stf(uint32_t contract, price_t fair) noexcept {
    if (contract >= m_.n_all) return;
    s_.set_stf(contract, fair);
    // legacy handleSTFUpdate: every offset that depends on this contract is recomputed, and the
    // contracts that quote through it are revisited
    for (uint32_t i = m_.stf_offset_begin[contract]; i < m_.stf_offset_begin[contract + 1]; ++i) {
        const uint32_t offset = m_.stf_offset_list[i];
        if (paths_.update_offset(s_, offset)) {
            for (uint32_t k = m_.offset_path_begin[offset];
                 k < m_.offset_path_begin[offset + 1]; ++k) {
                s_.mark_dirty(m_.slot_contract[m_.offset_path_slot[k]]);
            }
        }
    }
}


// ---------------------------------------------------------------------------
// the cycle
// ---------------------------------------------------------------------------
void host::run_cycle(uint64_t) noexcept {
    ++stats_.cycles;
    if (s_.dirty_count() == 0) return;

    // legacy: while a transaction is open the quote change is retained, not dropped
    if (!can_send_quotes_) {
        stats_.quotes_blocked += s_.dirty_count();
        return;
    }

    const uint32_t count = s_.dirty_count();
    static thread_local std::array<uint32_t, MAX_DIRTY> pending{};
    for (uint32_t i = 0; i < count; ++i) pending[i] = s_.dirty_at(i);
    s_.clear_dirty();

    for (uint32_t i = 0; i < count; ++i) cycle_contract(pending[i]);
}

void host::cycle_contract(uint32_t contract) noexcept {
    if (!m_.is_quoting(contract)) return;

    // legacy updateQuotes(): re-price first, whether or not quotes may be sent
    paths_.update_paths(s_, contract, false);

    if (!enabled_ && !external_quoting_disabled_) return;

    quote_gates gates;
    gates.enabled = enabled_;
    gates.can_quote_external = can_quote_external(contract);
    const quote_result r = quotes_.build(s_, contract, false, gates);

    if (r.violated()) {
        ++stats_.validation_failures;
        disable(kMaxTicks);
        return;
    }
    if (!r.changed()) return;

    const uint32_t slot_bid = paths_.best_slot(s_, contract, side::bid);
    const uint32_t slot_ask = paths_.best_slot(s_, contract, side::ask);
    if (r.bid == quote_status::emit) {
        emit_side(contract, side::bid,
                  slot_bid != INVALID_INDEX && s_.path(slot_bid).lean_on_rv);
    }
    if (r.ask == quote_status::emit) {
        emit_side(contract, side::ask,
                  slot_ask != INVALID_INDEX && s_.path(slot_ask).lean_on_rv);
    }
    ++stats_.quote_notifications;
    if (ports_.notify_quote != nullptr) ports_.notify_quote(ports_.ctx, contract);
}

void host::emit_side(uint32_t contract, side sd, bool lean_on_rv) noexcept {
    const quote& q = s_.quote_of(contract, sd);
    ++stats_.quotes_emitted;
    if (ports_.send_quote == nullptr) return;

    quote_request request;
    request.contract = contract;
    request.sd = sd;
    request.internal_price = q.internal_price;
    request.internal_size = q.internal_size;
    request.external_price = q.external_price;
    request.external_size = q.external_size;
    request.quote_external = can_quote_external(contract);
    request.lean_on_rv = lean_on_rv;
    ports_.send_quote(ports_.ctx, request);

    // legacy tryAddAggressiveQuoteAdjustment: a quote that trades through the opposite external
    // book reserves that liquidity (and the hedge it implies) in the ledger
    if (bridge_ == nullptr || request.external_size == 0) return;
    const market_state& opposite = s_.book(contract, other(sd), true);
    const bool crosses = opposite.price != NULL_PRICE &&
                         is_price_better_or_equal(sd, q.external_price, opposite.price);
    if (!crosses) return;

    // the hedge the aggressive quote implies is the lean side of the very path we just priced
    market_key hedge_markets[MAX_LEANS_PER_PATH];
    uint8_t hedge_count = 0;
    const uint32_t slot = paths_.best_slot(s_, contract, sd);
    if (slot != INVALID_INDEX) {
        const uint32_t d = m_.slot_path[slot];
        for (uint32_t k = m_.path_lean_begin[d]; k < m_.path_lean_begin[d + 1] && hedge_count < MAX_LEANS_PER_PATH; ++k) {
            const uint32_t lean = m_.lean_contract[k];
            const side lean_side = m_.lean_mult[k] > 0 ? sd : other(sd);
            const bool external_lean = s_.best_is_external(lean, lean_side);
            hedge_markets[hedge_count++] =
                make_market_key(lean, static_cast<order_side>(to_index(lean_side)),
                                external_lean ? EXTERNAL : INTERNAL);
        }
    }
    bridge_->set_aggressive_quote(contract, sd, request.external_size,
                                  std::span<const market_key>(hedge_markets, hedge_count));
}

bool host::can_quote_external(uint32_t contract) const noexcept {
    if (!external_quoting_enabled()) return false;
    if (m_.quote_external[contract] == 0) return false;
    const uint32_t venue = m_.venue[contract];
    return venue < venue_enabled_.size() && venue_enabled_[venue] != 0;
}

void host::disable(const char* reason) noexcept {
    const bool was_enabled = enabled_;
    enabled_ = false;
    external_quoting_disabled_ = true;
    stats_.disabled = true;
    if (reason != nullptr && ports_.on_alert != nullptr) ports_.on_alert(ports_.ctx, reason);
    // legacy handleDisabled(): withdraw every quote and cancel everything
    for (uint32_t c = 0; c < m_.n_all; ++c) {
        if (!m_.is_quoting(c)) continue;
        s_.quote_of(c, side::bid).reset();
        s_.quote_of(c, side::ask).reset();
        s_.last_sent(c, side::bid).reset();
        s_.last_sent(c, side::ask).reset();
    }
    if (was_enabled && ports_.cancel_all != nullptr) ports_.cancel_all(ports_.ctx);
}

}  // namespace tachyon::trade::as

