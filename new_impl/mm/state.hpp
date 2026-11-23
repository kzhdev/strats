#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "book.hpp"
#include "limits.hpp"
#include "model.hpp"
#include "order_queue.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// What the strategy wants resting on one side (legacy SimpleMarketMaker::Quote, :159-174).
struct quote_state {
    price_t internal_price = tachyon::NULL_PRICE;
    qty_t internal_size = 0;
    price_t external_price = tachyon::NULL_PRICE;
    qty_t external_size = 0;
    // Last values actually sent; the change detection compares against these (:675-676,
    // :747-751). They are *not* cleared by reset(), exactly like legacy.
    price_t last_internal_price_sent = tachyon::NULL_PRICE;
    qty_t last_external_size_sent = 0;

    void reset() noexcept {  // legacy Quote::reset (:168-173)
        internal_price = tachyon::NULL_PRICE;
        internal_size = 0;
        external_price = tachyon::NULL_PRICE;
        external_size = 0;
    }
};

// What the strategy wants resting for one side of the hedge book (legacy Hedge, :176-190).
//
// The flip behaviour lives in the size reconciliation (modifyHedgeSize, :1119-1215): only one
// side carries a size at a time, and flipping resets the other side explicitly.
struct hedge_state {
    price_t external_price = tachyon::NULL_PRICE;
    price_t internal_price = tachyon::NULL_PRICE;
    // Kept by reset(), exactly like legacy (:186-190) — the change detection compares against it.
    price_t last_internal_price_sent = tachyon::NULL_PRICE;
    qty_t size_to_quote = 0;
    bool in_payup = false;

    void reset() noexcept {  // legacy Hedge::reset
        external_price = tachyon::NULL_PRICE;
        internal_price = tachyon::NULL_PRICE;
        size_to_quote = 0;
        in_payup = false;
    }
};

// Everything the cycle needs for one contract: the two books, the pricing values, the quotes,
// the two positions and the per-contract flags of the legacy object. A plain record — the hot
// path reads and writes the members directly (no getter chains, no virtuals).
struct contract_state {
    book books[SIDE_COUNT];
    quote_state quotes[SIDE_COUNT];
    hedge_state hedges[SIDE_COUNT];

    book& side_book(side s) noexcept { return books[to_index(s)]; }
    const book& side_book(side s) const noexcept { return books[to_index(s)]; }
    quote_state& quote_of(side s) noexcept { return quotes[to_index(s)]; }
    const quote_state& quote_of(side s) const noexcept { return quotes[to_index(s)]; }
    hedge_state& hedge_of(side s) noexcept { return hedges[to_index(s)]; }
    const hedge_state& hedge_of(side s) const noexcept { return hedges[to_index(s)]; }

    // ---- pricing (legacy _midPoint / _width / _widthPercentage / _establishedLevel) ----
    price_t mid_point = 0;
    price_t width = 0;
    double width_pct = 0.0;
    double ema_est_level = 0.0;   // legacy _emaEstLevel
    double last_tob_qty = 0.0;    // legacy _lastTobQty
    uint64_t last_ema_time = 0;   // legacy _lastEmaTime
    // Legacy initialises this to 1 (:1703) and only then lets the EMA raise it, so a fresh
    // session quotes against an established level of 1.
    qty_t established_level = 1;

    // ---- positions (legacy _internallyInitiatedPosition / _externallyInitiatedPosition) ----
    qty_t internal_pos = 0;
    qty_t external_pos = 0;

    // ---- flags ----
    bool enabled = false;                 // legacy _enabled
    bool quote_external = false;          // legacy _quoteExternal
    bool quote_external_before_event = false;  // legacy _quoteExternalBeforeEconomicNumber
    bool max_ticks_wide_breached = false;      // legacy _maxTicksWideBreached
    bool contract_can_trade = false;
    bool contract_can_cancel = false;
    bool gateway_active = true;
    uint32_t order_execution_token = 0;   // legacy _orderExecutionToken
    uint64_t last_md_bid = 0;             // legacy _lastMDbid (committed at the end of a batch)
    uint64_t pending_md_bid = 0;          // the batch id of the updates seen since then

    // Legacy isEnabled() (:363-366): the contract flag AND the strategy-wide state AND whether
    // the contract may trade. Kept here so the cycle and the tests agree on one definition.
    bool is_enabled(bool strategy_enabled) const noexcept {
        return enabled && strategy_enabled && contract_can_trade && gateway_active;
    }
    // Legacy isHedging<Side> (:341-344).
    bool is_hedging(side s) const noexcept { return hedge_of(s).size_to_quote != 0; }
    // Legacy calculateHedgeQuoteSize (:1102-1105): hedge against the net position, so a short
    // book hedges with a bid of exactly that size.
    qty_t hedge_size() const noexcept { return -(internal_pos + external_pos); }
    // Legacy isHedging<Side> (:341-344) is phase 5.
};

// The whole engine's state: one contract_state per configured contract, one order arena per
// contract (allocated at init, never in the cycle), and the dirty set that drives the cycle.
class cycle_state {
public:
    cycle_state() noexcept = default;
    explicit cycle_state(const model& m) { init(m); }

    // Fresh session: books empty, widths from the model's initial shift, EMA seeded from
    // EstLevelEMA, positions from the config, quotes cleared.
    void init(const model& m);

    contract_state& contract(uint32_t c) noexcept { return contracts_[c]; }
    const contract_state& contract(uint32_t c) const noexcept { return contracts_[c]; }
    order_queue& queue(uint32_t c) noexcept { return queues_[c]; }
    const order_queue& queue(uint32_t c) const noexcept { return queues_[c]; }

    uint32_t n_contracts() const noexcept { return n_contracts_; }

    // ---- dirty tracking (bitset + packed list, no hashing: PLAN.md §2 P1) ----
    // A contract is marked at most once per cycle and the list keeps first-touch order, which
    // is what makes the cycle order deterministic.
    void mark_dirty(uint32_t c) noexcept {
        const uint64_t bit = uint64_t(1) << (c & 63u);
        uint64_t& word = dirty_[c >> 6u];
        if ((word & bit) == 0) {
            word |= bit;
            changed_[changed_count_++] = c;
        }
    }
    void clear_dirty() noexcept {
        for (uint32_t i = 0; i < changed_count_; ++i) {
            dirty_[changed_[i] >> 6u] = 0;
        }
        changed_count_ = 0;
    }
    uint32_t changed_count() const noexcept { return changed_count_; }
    uint32_t changed_at(uint32_t i) const noexcept { return changed_[i]; }

    uint64_t now_cycles = 0;

private:
    std::array<contract_state, MAX_CONTRACTS> contracts_{};
    std::vector<order_queue> queues_;  // sized to the configured contracts at init
    uint32_t n_contracts_ = 0;

    std::array<uint64_t, CONTRACT_WORDS> dirty_{};
    std::array<uint32_t, MAX_CHANGED> changed_{};
    uint32_t changed_count_ = 0;
};

}  // namespace tachyon::trade::mm
