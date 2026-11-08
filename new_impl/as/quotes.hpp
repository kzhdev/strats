#pragma once

// The quote engine: turn the best path of a contract into the two quotes it publishes, decide
// whether either of them needs sending, and guard them with the maxTicksThroughMarket bound.
//
//   internal price = best path price ∓ edge                    (the edge widens the quote)
//   lean price     = best path price ± path commission         (the un-adjusted basket price)
//   external price = internal price rounded to the tick        (bid floors, ask ceils)
//   external size  = internal size, clamped when it would cross the opposite external book
//
// Legacy reference: updateQuote<Side> (src/ArbieSpreader.h:1048-1113), getExternalQuoteSize
// (:1006-1013), isAggressiveQuote (:1000-1004) and checkMaxTicksThroughMarket (:1115-1122).
//
// The engine sends nothing: it records the desired quote in `cycle_state` and reports whether
// that state changed, which is the host's cue to emit. Emission order and transport stay in one
// place (the host) while the arithmetic is testable here.

#include <algorithm>
#include <cstdint>

#include "model.hpp"
#include "paths.hpp"
#include "policy.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::as {

enum class quote_status : uint8_t {
    unchanged = 0,        // nothing to send
    emit,                 // the quote (or its cancellation) must be sent
    max_ticks_violation,  // the quote is invalid; legacy disables AS and its external quoting
};

// Per-contract gates the host owns: the strategy-wide enabled flag and whether this contract may
// quote externally (canQuoteExternally() && the venue gate && the contract's own flag).
struct quote_gates {
    bool enabled = true;
    bool can_quote_external = false;
};

struct quote_result {
    quote_status bid = quote_status::unchanged;
    quote_status ask = quote_status::unchanged;

    bool changed() const noexcept {
        return bid == quote_status::emit || ask == quote_status::emit;
    }
    bool violated() const noexcept {
        return bid == quote_status::max_ticks_violation ||
               ask == quote_status::max_ticks_violation;
    }
};

class quote_engine {
public:
    quote_engine(const model& m, const path_engine& paths, const quote_policy& policy) noexcept
        : m_(m), paths_(paths), policy_(policy) {}

    // legacy updateQuotes(Contract&, force) (ArbieSpreader.cpp:414-435): both sides, with the ask
    // side evaluated as disabled once the bid side has violated the bound -- which is what legacy
    // gets for free by disabling the strategy inside the bid call.
    quote_result build(cycle_state& s, uint32_t contract, bool force,
                       const quote_gates& gates) const noexcept {
        quote_result r;
        r.bid = build_side<side::bid>(s, contract, force, gates.can_quote_external, gates.enabled);
        const bool ask_enabled = gates.enabled && r.bid != quote_status::max_ticks_violation;
        r.ask = build_side<side::ask>(s, contract, force, gates.can_quote_external, ask_enabled);
        return r;
    }

    template <side S>
    quote_status build_side(cycle_state& s, uint32_t contract, bool force, bool can_quote_external,
                            bool enabled) const noexcept;

    // legacy isAggressiveQuote<Side> (:1000-1004): our quote would trade through the opposite
    // side of the same contract's external book.
    template <side S>
    bool is_aggressive(const cycle_state& s, uint32_t contract, price_t price) const noexcept {
        const market_state& opposite = s.book(contract, other(S), true);
        return opposite.price != NULL_PRICE &&
               is_price_better_or_equal<S>(price, opposite.price);
    }

    // legacy getExternalQuoteSize<Side> (:1006-1013): a crossing quote may only take what the
    // opposite external book shows, never more than our own internal size.
    template <side S>
    qty_t external_size(const cycle_state& s, uint32_t contract, const quote& q) const noexcept {
        if (q.external_price != NULL_PRICE && is_aggressive<S>(s, contract, q.external_price)) {
            const qty_t crossing = s.adjusted_size(policy_, contract, other(S), true);
            return std::min(crossing, q.internal_size);
        }
        return q.internal_size;
    }

    // legacy checkMaxTicksThroughMarket<Side> (:1115-1122): the quote is valid while it sits
    // *strictly* closer to the opposite side's best price than maxTicksThroughMarket. A missing
    // price on either side accepts, which is why an internal-only quote can never violate.
    template <side S>
    bool within_max_ticks(const cycle_state& s, uint32_t contract, const quote& q) const noexcept {
        const price_t opposite_best = s.best_book(contract, other(S)).price;
        if (q.external_price == NULL_PRICE || opposite_best == NULL_PRICE) return true;
        return ticks_through_market<S>(q.external_price, opposite_best) <
               m_.max_ticks_through_market[contract];
    }

private:
    const model& m_;
    const path_engine& paths_;
    quote_policy policy_;
};

}  // namespace tachyon::trade::as
