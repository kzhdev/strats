#pragma once

// Reference oracle for the AS quote stage: a transcription of legacy
// ArbieSpreader::updateQuote<Side> (src/ArbieSpreader.h:1048-1113), getExternalQuoteSize
// (:1006-1013), isAggressiveQuote (:1000-1004) and checkMaxTicksThroughMarket (:1115-1122),
// plus the two-side driver updateQuotes(Contract&, force) (ArbieSpreader.cpp:414-435).
//
// It shares nothing with as/quotes.hpp: it keeps its own quote/last-sent/force mirror and
// re-derives every expression from the legacy text, so a shared bug cannot make the differential
// pass (rv PARITY_NOTES §5).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "model.hpp"
#include "policy.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::as::oracle {

enum class quote_status : uint8_t { unchanged = 0, emit, max_ticks_violation };

struct quote_result {
    quote_status status = quote_status::unchanged;
    quote q;
};

// legacy isAggressiveQuote<Side>: the opposite external book is quoted, and our price is at or
// through it.
template <side S>
bool is_aggressive(const cycle_state& s, uint32_t contract, price_t price) noexcept {
    const market_state& opposite = s.book(contract, other(S), true);
    if (opposite.price == NULL_PRICE) return false;
    return S == side::bid ? price >= opposite.price : price <= opposite.price;
}

// legacy getExternalQuoteSize<Side>
template <side S>
qty_t external_size(const cycle_state& s, const quote_policy& policy, uint32_t contract,
                    const quote& q) noexcept {
    if (is_aggressive<S>(s, contract, q.external_price)) {
        const qty_t opposite = s.adjusted_size(policy, contract, other(S), true);
        return std::min(opposite, q.internal_size);
    }
    return q.internal_size;
}

// legacy checkMaxTicksThroughMarket<Side>
template <side S>
bool within_max_ticks(const model& m, const cycle_state& s, uint32_t contract,
                      const quote& q) noexcept {
    const price_t opposite_best = s.best_book(contract, other(S)).price;
    if (q.external_price == NULL_PRICE || opposite_best == NULL_PRICE) return true;
    const price_t ticks = S == side::bid ? q.external_price - opposite_best
                                        : opposite_best - q.external_price;
    return ticks < m.max_ticks_through_market[contract];
}

// legacy updateQuote<Side>. `previous` is the quote record before the call (legacy mutates it in
// place, and reads `_hedgePath` from it in the "no path" branch), `last_sent`/`force_update` are
// the oracle's own mirrors.
template <side S>
quote_result build_side(const model& m, const cycle_state& s, const quote_policy& policy,
                        uint32_t contract, const quote& previous, quote& last_sent,
                        bool& force_update, bool force, bool can_quote_external,
                        bool enabled) noexcept {
    quote_result r;
    r.q = previous;

    const uint32_t key = contract * SIDE_COUNT + to_index(S);
    const uint32_t count = m.side_path_begin[key + 1] - m.side_path_begin[key];
    // legacy `_paths[Side][0]`: the engine's ranking is an input to the quote stage (it is gated
    // by as_paths / as_incremental), so both sides read the same rank 0.
    const uint32_t slot = count == 0 ? INVALID_INDEX : s.order_at(m, contract, S, 0);
    const bool valid = count != 0 && s.path(slot).valid();

    if (valid) {
        const path_state& p = s.path(slot);
        const uint32_t d = m.slot_path[slot];
        r.q.lean_price = apply_price_offset<S>(p.price, m.path_commission[d]);
        r.q.internal_price = apply_price_offset<S>(p.price, -m.edge[contract]);
        r.q.internal_size = p.size;
        r.q.hedge_path = static_cast<int32_t>(m.path_local_id[d]);
        if (can_quote_external) {
            // legacy RoundToTickSize(price, tickSize, Side): bid floors, ask ceils
            const price_t tick = m.tick[contract];
            const price_t remainder = r.q.internal_price % tick;
            price_t rounded = r.q.internal_price;
            if (remainder) {
                const price_t truncated = r.q.internal_price - remainder;
                rounded = S == side::bid ? (remainder > 0 ? truncated : truncated - tick)
                                         : (remainder > 0 ? truncated + tick : truncated);
            }
            r.q.external_price = rounded;
            r.q.external_size = external_size<S>(s, policy, contract, r.q);
        } else {
            r.q.external_price = NULL_PRICE;
            r.q.external_size = 0;
        }

        if (!enabled) return r;
        if (!within_max_ticks<S>(m, s, contract, r.q)) {
            r.status = quote_status::max_ticks_violation;
            return r;
        }
        const bool differs = r.q.internal_price != last_sent.internal_price ||
                             r.q.internal_size != last_sent.internal_size ||
                             r.q.external_price != last_sent.external_price ||
                             r.q.external_size != last_sent.external_size;
        if (differs || force_update || force) {
            last_sent = r.q;
            force_update = false;
            r.status = quote_status::emit;
        }
        return r;
    }

    if (previous.hedge_path != -1) {
        r.q.reset();
        if (!enabled) return r;
        last_sent = r.q;
        force_update = false;
        r.status = quote_status::emit;
    }
    return r;
}

}  // namespace tachyon::trade::as::oracle
