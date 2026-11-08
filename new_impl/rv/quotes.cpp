#include "quotes.hpp"

#include <algorithm>
#include <cmath>

namespace tachyon::trade::rv {

// ---- quote_engine ------------------------------------------------------------

namespace {

// Hot-path kernels of the quote build. They are file-local on purpose: as class members with
// external linkage GCC keeps them out of line, and `quote_engine::build` then pays a call per
// contract/side plus a reload of every register-only value (measured: +10% on the quote stage,
// see PLAN §7.3). Internal linkage lets them inline into the caller exactly as the previous
// free-function implementation did, while the *interface* stays the quote_engine object.

// One lean that produced a usable price (the winning one, for the quote).
struct best_lean {
    price_t price = tachyon::NULL_PRICE;
    double size = 0.0;
    uint32_t lean = 0;
    bool external = false;
    bool found = false;
};

// External quote: club aggregate over leans whose rounded price equals the first rounded price
// (legacy createQuote external branch, S6).
struct club {
    price_t price = tachyon::NULL_PRICE;
    qty_t size = 0;
    uint32_t count = 0;
};

inline best_lean find_best(const model& m, const quote_policy& policy, const cycle_state& s,
                           uint32_t contract, side sd) {
    best_lean best;
    for (uint32_t k = m.lean_begin[contract]; k < m.lean_begin[contract + 1]; ++k) {
        const uint32_t lean = m.lean_index[k];
        const bool external = s.best_is_external(lean, sd);
        const price_t price = lean_calculator::price(m, s, contract, sd, lean, external);
        if (price == tachyon::NULL_PRICE) {
            continue;
        }
        const bool better =
            !best.found || (sd == side::bid ? price > best.price : price < best.price);
        if (!better) {
            continue;
        }
        if (policy.validate != nullptr &&
            !policy.validate(policy.validate_ctx, contract, sd, price, lean)) {
            continue;  // legacy invalidates the whole quote; the caller disables quoting
        }
        const market_state& lm = s.market(lean, sd, external);
        best.price = price;
        // legacy _bestSize uses the *computed* lean size (calculateSize), i.e. capped by the
        // hedge ratio and by the precomputed quote size, on the reservation-adjusted market
        // size -- not the raw displayed size.
        best.size = static_cast<double>(lean_calculator::size(m, s, contract, sd, lean, external, lm.size, policy));
        best.lean = lean;
        best.external = external;
        best.found = true;
    }
    return best;
}

inline club build_club(const model& m, const quote_policy& policy, const cycle_state& s,
                       uint32_t contract, side sd, bool enabled) {
    club c;
    for (uint32_t k = m.lean_begin[contract]; k < m.lean_begin[contract + 1]; ++k) {
        const uint32_t lean = m.lean_index[k];
        const bool external = s.best_is_external(lean, sd);
        const price_t price = lean_calculator::price(m, s, contract, sd, lean, external);
        if (price == tachyon::NULL_PRICE) {
            continue;
        }
        const price_t rounded = round_to_tick(price, m.tick[contract], sd);
        if (c.price == tachyon::NULL_PRICE) {
            c.price = rounded;
            c.size = lean_calculator::size(m, s, contract, sd, lean, external,
                                                     s.market(lean, sd, external).size, policy);
            c.count = 1;
        } else if (rounded == c.price) {
            c.size += lean_calculator::size(m, s, contract, sd, lean, external,
                                                     s.market(lean, sd, external).size, policy);
            ++c.count;
        } else {
            break;  // leans are price ordered; first different price ends the club
        }
        if (enabled && policy.validate != nullptr &&
            !policy.validate(policy.validate_ctx, contract, sd, price, lean)) {
            return club{};
        }
    }
    return c;
}

}  // namespace

void quote_engine::build(cycle_state& s, uint32_t contract, side sd) const noexcept {
    quote& q = s.quote_of(contract, sd);
    if (m_.lean_begin[contract] == m_.lean_begin[contract + 1]) {
        // no leans at all -> quote becomes "market gone" once (legacy :247-254)
        if (!q.market_gone) {
            q.reset();
            q.market_gone = true;
            q.has_change = true;
        }
        return;
    }
    q.market_gone = false;

    const best_lean best = find_best(m_, policy_, s, contract, sd);
    if (!best.found) {
        q.reset();
        return;
    }

    const price_t old_price = q.price;
    const qty_t old_size = q.size;
    const price_t old_ext_price = q.external_price;
    const qty_t old_ext_size = q.external_size;

    q.price = best.price;  // unrounded, as legacy _bestPrice
    q.size = to_valid_qty(static_cast<qty_t>(best.size + 0.0001), m_.lot[contract]);
    q.lean = best.lean;

    const price_t threshold = m_.threshold[contract];
    bool has_change = std::abs(old_price - q.price) > threshold || old_size != q.size;

    if (m_.quote_external[contract]) {
        const club c = build_club(m_, policy_, s, contract, sd, /*enabled=*/true);
        q.external_price = c.price;
        if (m_.static_quote[contract]) {
            q.external_size =
                to_valid_qty(static_cast<qty_t>(m_.static_quote_size[contract]), m_.lot[contract]);
        } else {
            const double average =
                c.count > 0 ? static_cast<double>(c.size) / static_cast<double>(c.count) + 0.0001
                            : 0.0;
            const qty_t club_qty = static_cast<qty_t>(average);
            const qty_t candidate = std::max(q.size, club_qty);
            q.external_size = to_valid_qty(candidate, m_.lot[contract]);
        }
        has_change = has_change || std::abs(old_ext_price - q.external_price) > threshold ||
                     old_ext_size != q.external_size;
    } else {
        q.external_price = tachyon::NULL_PRICE;
        q.external_size = 0;
        has_change = has_change || std::abs(old_ext_price - q.external_price) > threshold ||
                     old_ext_size != q.external_size;
    }

    q.has_change = q.has_change || has_change;
}

void quote_engine::build_all(cycle_state& s) const noexcept {
    // `traded` is a flag, not a range: a contract may be priced and quoted at once
    // (config/2msgw), in which case it sits below first_traded (PLAN §12).
    for (uint32_t c = 0; c < m_.n_all; ++c) {
        if (!m_.traded[c]) {
            continue;
        }
        build(s, c, side::bid);
        build(s, c, side::ask);
    }
}

bool quote_engine::within_max_ticks_through_market(const cycle_state& s, uint32_t contract,
                                                   side sd, uint32_t lean) const noexcept {
    // Legacy: otherSideBest comes from the traded contract's own book; no opposite side means
    // nothing to measure against and the quote is accepted.
    const price_t other_side_best = s.best_market(contract, other(sd)).price;
    if (other_side_best == tachyon::NULL_PRICE) {
        return true;
    }
    const bool lean_external = s.best_is_external(lean, sd);
    const price_t quote_without_cap =
        lean_calculator::price_without_cost_cap(m_, s, contract, sd, lean, lean_external);
    if (quote_without_cap == tachyon::NULL_PRICE) {
        return true;  // lean vanished between passes; the caller skips it anyway
    }
    const price_t through = ticks_through_market(quote_without_cap, other_side_best, sd);
    // Legacy compares in price units against maxTicksThroughMarket * tickSize and rejects on
    // `>=`. A configured 0 therefore rejects every quote (legacy then disables), which is
    // reproduced rather than special-cased as "no limit".
    return through < m_.max_ticks_through_market[contract];
}

}  // namespace tachyon::trade::rv
