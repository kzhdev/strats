#pragma once

#include <common/types.hpp>

#include <cstdint>

#include "limits.hpp"

// Vocabulary of the executor and the implied engine (PLAN.md §2).
//
// `strategy_id` and `liquidity_source` are reused from new_impl/common so that the strategies,
// the liquidity ledger and the executor all speak the same identifiers: the implied engine
// publishes exactly the liquidity sources the ledger already understands
// (LIQUIDITY_SOURCE_MM_HEDGE .. LIQUIDITY_SOURCE_AS_QUOTE).
namespace tachyon::trade::exec {

using price_t = tachyon::price_t;  // int64, 1e-9 dollars (tachyon's fixed point)
using qty_t = int64_t;             // signed, matching the legacy hf::qty_t (mm/as D10)

inline constexpr price_t NULL_PRICE = tachyon::NULL_PRICE;

using strategy_id = tachyon::trade::strategy_id;
using liquidity_source = tachyon::trade::liquidity_source;

inline constexpr uint32_t INVALID_CONTRACT = 0xFFFFFFFFu;

enum class side : uint8_t { bid = 0, ask = 1 };

// A side must be one of the shared SIDE_COUNT values, not a per-module invention.
static_assert(static_cast<uint32_t>(side::ask) + 1 == SIDE_COUNT, "side must span SIDE_COUNT");

constexpr uint32_t to_index(side s) noexcept { return static_cast<uint32_t>(s); }
constexpr side other(side s) noexcept { return s == side::bid ? side::ask : side::bid; }

enum class request_kind : uint8_t { quote = 0, hedge = 1 };

// The executor's per-strategy tables are sized by STRATEGY_COUNT; pin it to the shared enum so that
// adding a strategy is a compile error here rather than a mailbox that silently goes missing.
static_assert(STRATEGY_COUNT == static_cast<uint32_t>(strategy_id::__COUNT__),
              "STRATEGY_COUNT must match strategy_id::__COUNT__");


// ---------------------------------------------------------------------------
// Per-key behaviour -- the spine of the design.
//
// The legacy engine decides, per (strategy, kind), whether a burst of requests is collapsed to
// the last one ("skip to the last request, to avoid order flickering") or consumed one at a
// time (InternalMatchingEngine.h:1952-1971):
//
//   collapse : MM hedge (id 0), MM quote (id 1), RV quote (id 3)
//   FIFO     : RV hedge (id 2), AS quote (id 5), AS hedge (id 4)
//
// AS quotes are not collapsed because of the aggressive-quote adjustment (:1967): each AS quote
// is a fresh path decision that must be applied -- and crossed -- in order, not overwritten.
//
// `mode` says what happens to the request once the engine owns it:
//   rest       -- becomes part of the internal book; the contract's cross runs at flush time
//   cross_now  -- FAK: not stored, crosses the book immediately and sends the remainder out
//
// `source` is the liquidity_source the resting order is published under; only orders that can
// rest internally have one (common/types.hpp: only MM hedges among hedges do).
// ---------------------------------------------------------------------------
enum class collapse : uint8_t { last_wins, one_at_a_time };
enum class apply_mode : uint8_t { rest, cross_now };

struct key_traits {
    collapse policy;
    apply_mode mode;
    bool rests;
    liquidity_source source;
};

constexpr key_traits traits_of(strategy_id s, request_kind k) noexcept {
    switch (s) {
        case strategy_id::MM:
            return k == request_kind::quote
                       ? key_traits{collapse::last_wins, apply_mode::rest, true,
                                    liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE}
                       : key_traits{collapse::last_wins, apply_mode::rest, true,
                                    liquidity_source::LIQUIDITY_SOURCE_MM_HEDGE};
        case strategy_id::RV:
            // RV hedge is FAK: it never rests, so it has no published source.
            return k == request_kind::quote
                       ? key_traits{collapse::last_wins, apply_mode::rest, true,
                                    liquidity_source::LIQUIDITY_SOURCE_RV_QUOTE}
                       : key_traits{collapse::one_at_a_time, apply_mode::cross_now, false,
                                    liquidity_source::__LIQUIDITY_SOURCE_END__};
        case strategy_id::AS:
            return k == request_kind::quote
                       ? key_traits{collapse::one_at_a_time, apply_mode::rest, true,
                                    liquidity_source::LIQUIDITY_SOURCE_AS_QUOTE}
                       : key_traits{collapse::one_at_a_time, apply_mode::cross_now, false,
                                    liquidity_source::__LIQUIDITY_SOURCE_END__};
        default:
            return key_traits{collapse::one_at_a_time, apply_mode::cross_now, false,
                              liquidity_source::__LIQUIDITY_SOURCE_END__};
    }
}

constexpr bool is_last_wins(strategy_id s, request_kind k) noexcept {
    return traits_of(s, k).policy == collapse::last_wins;
}

constexpr bool rests_internally(strategy_id s, request_kind k) noexcept {
    return traits_of(s, k).rests;
}

// Address of a request stream inside a contract: strategy-major, then kind, then side. The
// layout *is* the key: a request is routed with two multiplies and no lookup (PLAN.md D-EX9).
constexpr uint32_t slot_of(strategy_id s, request_kind k, side sd) noexcept {
    return (to_int(s) * KIND_COUNT + to_int(k)) * SIDE_COUNT + to_index(sd);
}

struct request_key {
    strategy_id strategy = strategy_id::MM;
    request_kind kind = request_kind::quote;
    side sd = side::bid;

    constexpr uint32_t slot() const noexcept { return slot_of(strategy, kind, sd); }
};


// ---------------------------------------------------------------------------
// Wire types between the engine and its ports (ports.hpp). Plain data only: the ports are the
// only place that knows about a transport, and they never see an engine object.
// ---------------------------------------------------------------------------

// What the engine asks the exchange -- or the OM layer sitting in front of it -- to do.
//
// The quote leg is the *aggregated residual* of one side of the internal book, exactly what
// legacy synchronizeQuote sends after getPriceAndSize (InternalMatchingEngine.h:835-852,
// :543-567): one order per (contract, side), change-detected. The hedge leg is one order per
// hedge request (legacy sendHedge :569-607 / sendExternalFAKHedge :1884).
struct exchange_order {
    uint32_t contract = INVALID_CONTRACT;
    strategy_id strategy = strategy_id::MM;
    side sd = side::bid;
    price_t price = NULL_PRICE;
    qty_t size = 0;
    bool hedge = false;    // hedge leg rather than a quote leg
    bool persist = false;  // quote leg: keep the price when the size goes to zero
    bool pay_up = false;   // hedge leg: the strategy asked to pay up
    uint32_t hedge_id = 0;
};

// A fill the internal cross produced, for one of the two orders involved.
struct internal_fill {
    uint32_t contract = INVALID_CONTRACT;
    strategy_id strategy = strategy_id::MM;
    request_kind kind = request_kind::quote;
    side sd = side::bid;  // the side of the *filled order*, not the aggressor's
    price_t price = NULL_PRICE;
    qty_t size = 0;
    // Legacy FillSource::MissedHedge (:2033-2042): an internal-only FAK hedge that could not be
    // filled completely is reported back on the *other* side so the strategy can unwind. It is
    // not a trade; endpoints must treat it as a correction.
    bool missed = false;
};

// The internal best per (subscriber strategy, side): what sendInternalMarketData publishes
// (InternalMatchingEngine.h:1249-1307). `source` is which liquidity the best belongs to, so the
// subscriber can tell MM hedge from RV quote. An empty book publishes NULL_PRICE / 0 / __END__.
struct internal_top {
    price_t price = NULL_PRICE;
    qty_t size = 0;
    liquidity_source source = liquidity_source::__LIQUIDITY_SOURCE_END__;

    constexpr bool empty() const noexcept { return price == NULL_PRICE || size <= 0; }
};

constexpr bool operator==(const internal_top& a, const internal_top& b) noexcept {
    return a.price == b.price && a.size == b.size && a.source == b.source;
}

constexpr bool operator!=(const internal_top& a, const internal_top& b) noexcept {
    return !(a == b);
}

// One internal price level, for the MM-side L2 view (mm::adapter::on_internal_book).
struct book_level {
    price_t price = NULL_PRICE;
    qty_t size = 0;
    liquidity_source source = liquidity_source::__LIQUIDITY_SOURCE_END__;
};

// Legacy IsBetterPrice / IsPriceBetterOrEqual (Common.h:331-363).
constexpr bool is_better_price(price_t one, price_t two, side s) noexcept {
    return s == side::bid ? one > two : one < two;
}

constexpr bool is_better_or_equal(price_t one, price_t two, side s) noexcept {
    return s == side::bid ? one >= two : one <= two;
}

// A NULL price always loses (Common.h:344-350).
constexpr price_t better_price(price_t lhs, price_t rhs, side s) noexcept {
    if (lhs == NULL_PRICE) return rhs;
    if (rhs == NULL_PRICE) return lhs;
    return is_better_price(lhs, rhs, s) ? lhs : rhs;
}

}  // namespace tachyon::trade::exec
