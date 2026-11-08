#pragma once

// The hedger: what AS does with a fill.
//
// A path is a basket, so one AS fill is hedged leg by leg along the best path's leans: each lean
// is traded in the direction that closes the basket, priced off the very book the path leaned on
// plus that contract's hedge payup. When the basket carries a short-term-fair offset (a "switch
// path"), the position is handed to RV instead of being held, which is what the switch-fill
// transfer describes.
//
// Legacy reference: updateHedge<Side> (src/ArbieSpreader.h:552-708). The order *transport* and the
// FAK reservations are the host's (they need the ledger and the order manager); the hedger decides
// what to trade and at what price.
//
// Every leg and every number here is integer at 1e-9 (PLAN D2), and the plan is a fixed-size value
// type, so hedging allocates nothing.

#include <array>
#include <cstdint>

#include "limits.hpp"
#include "model.hpp"
#include "paths.hpp"
#include "policy.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::as {

inline constexpr uint32_t MAX_LEANS_PER_PATH = 16;

// One hedge order: what to trade, on which side, at which price (legacy sendHedgeOrder args).
struct hedge_leg {
    uint32_t contract = INVALID_INDEX;      // the lean contract to trade
    side order_side = side::bid;            // the side of the hedge order
    price_t internal_price = NULL_PRICE;    // price for the internal book
    price_t external_price = NULL_PRICE;    // price for the external book (the raw lean price)
    qty_t size = 0;                         // the fill quantity, untransformed (legacy `qty`)
    bool external = false;                  // the lean's best book is the external one
    uint32_t market_index = 0;              // legacy `marketId`
};

// One leg of the AS->RV transfer of a switch-path fill (legacy ASFillsToRV / the front FillMessages).
struct switch_leg {
    uint32_t contract = INVALID_INDEX;
    price_t stf = NULL_PRICE;               // legacy `_contract->_stf`
    side front_side = side::bid;            // legacy front FillMessage._side
    side rv_side = side::bid;               // legacy ASFillsToRV fill._side
};

struct hedge_plan {
    bool hedged = false;      // false == legacy "Unable to hedge. No valid path."
    bool recalc_forced = false;  // the canHedge veto forced a path recalculation
    uint32_t hedge_id = 0;    // legacy ++_hedgeId, shared by the legs and their FAK reservations
    uint32_t path_id = 0;     // legacy pBest->_id
    uint32_t contract = INVALID_INDEX;  // the contract that filled
    side fill_side = side::bid;         // the side that filled
    qty_t qty = 0;

    uint8_t leg_count = 0;
    std::array<hedge_leg, MAX_LEANS_PER_PATH> legs{};

    // Switch path: the position goes to RV rather than being held. `quoted` describes the
    // contract that filled, `legs` the leans of the basket.
    bool switch_transfer = false;
    switch_leg quoted{};
    uint8_t switch_leg_count = 0;
    std::array<switch_leg, MAX_LEANS_PER_PATH> switch_legs{};
    // legacy quoteAdjustedFillPrice = contract._stf + pBest->_stfOffset._rvSTFMove * qty, i.e. the
    // fair value of the filled contract plus the path's fair move times the *quantity* (that
    // multiplication by qty is legacy's, transcribed as-is: PARITY_NOTES D-AS8).
    price_t adjusted_fill_price = NULL_PRICE;
};

class hedger {
public:
    hedger(const model& m, const path_engine& paths, const quote_policy& policy) noexcept
        : m_(m), paths_(paths), policy_(policy) {}

    // legacy updateHedge<Side> (:552-708). In order:
    //   1. release the AS aggressive-quote reservations this fill invalidates,
    //   2. check the best path can still be hedged, forcing a recalculation if not,
    //   3. build the hedge leg-by-leg, or report "unable to hedge".
    // The host sends the legs (in order), notifies the front *before* the hedge orders, reserves
    // the FAK quantity per leg *after* sending, and -- for a switch path -- forwards the transfer.
    template <side S>
    hedge_plan hedge(cycle_state& s, uint32_t contract, qty_t qty, bool is_internal) noexcept;

    hedge_plan hedge(cycle_state& s, uint32_t contract, side fill_side, qty_t qty,
                     bool is_internal) noexcept {
        return fill_side == side::bid ? hedge<side::bid>(s, contract, qty, is_internal)
                                      : hedge<side::ask>(s, contract, qty, is_internal);
    }

private:
    const model& m_;
    const path_engine& paths_;
    quote_policy policy_;
    uint32_t hedge_id_ = 0;  // legacy _hedgeId, monotonic across hedge events
};

}  // namespace tachyon::trade::as
