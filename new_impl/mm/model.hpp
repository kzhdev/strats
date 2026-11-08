#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "limits.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// Resolved raw inputs for one contract: the MMConfigs row plus the facts only secdb knows
// (tick size, lot, venue). `tick_size` is already at the engine's 1e-9 scale, produced by
// to_mm_price so it is exactly ten times the legacy value (D2).
struct contract_input {
    std::string name;  // diagnostics only: the model itself stores no strings

    price_t tick_size = 0;
    qty_t lot = 1;
    uint32_t venue = 0;

    bool enabled = false;
    bool quote_external = false;
    uint32_t quote_size = 0;
    uint32_t quote_gap = 0;
    double pct_tob_max_quote = 0.0;
    std::array<double, SHIFT_COUNT> width_pct{};
    double hedge_offset = 0.0;
    uint32_t join_size = 0;
    uint32_t leave_size = 0;
    uint32_t payup_qty = 0;
    uint32_t payup_orders = 0;
    double payup_multiple = 0.0;
    uint32_t max_pos_internal = 0;
    uint32_t max_pos_external = 0;
    double commission = 0.0;
    double min_internal_price_change = 0.0;
    double est_level_ema = 0.0;
    uint32_t est_level_ema_seconds = 0;
    double est_level_pct_ema = 0.0;
    int32_t max_ticks_wide = -1;
    double worsen_payup_pct = 0.0;
    uint32_t queue_importance = 0;
    int32_t internal_position = 0;
    int32_t external_position = 0;
};

struct model_inputs {
    std::vector<contract_input> contracts;
    double cycles_per_second = 1.0;  // legacy frequency, for GetCyclesFromSeconds
    uint32_t current_shift = 0;      // 0=Asia, 1=Europe, 2=US, 3=SafeMode
};

// Immutable after init. Every per-contract value is a flat array element, so the quoting
// path reads one index instead of walking a config object (PLAN.md §1 P5).
//
// What is precomputed here is exactly the arithmetic of the legacy
// SimpleMarketMaker::initialize (:192-236): unit conversions, the tick products
// (hedge offset, max-ticks-wide and its doubling, the worsen-payup amount), the EMA period
// in cycles, and the legacy validation rules. The cycle therefore never converts a config
// value, it only compares and adds.
class model {
public:
    // Throws std::runtime_error("<contract>: <problem>") on an invalid input: too many
    // contracts (caps), non-positive tick, zero quote size, a non-positive
    // worsen-payup percentage (ViperConfig.h:901). Also applies the legacy clamps:
    // queue_importance > 10 becomes 0 (ViperConfig.h:905-907).
    static model build(const model_inputs& in);

    uint32_t n_contracts = 0;
    double cycles_per_second = 1.0;
    uint32_t initial_shift = 0;

    // Identity / static facts
    std::vector<price_t> tick_size;
    std::vector<fast_div> tick;   // magic-number divider for the tick size
    std::vector<qty_t> lot;       // legacy _orderIncrementQty, >= 1
    std::vector<uint32_t> venue;  // venue enum value
    std::vector<uint8_t> enabled;
    std::vector<uint8_t> quote_external;

    // Quoting parameters
    std::vector<qty_t> quote_size;   // post-toValidQty (legacy _quoteSize)
    std::vector<qty_t> quote_gap;
    std::vector<double> pct_tob_max_quote;
    std::vector<std::array<double, SHIFT_COUNT>> width_pct;
    std::vector<qty_t> join_size;
    std::vector<qty_t> leave_size;
    std::vector<qty_t> payup_qty;
    std::vector<uint32_t> payup_orders;
    std::vector<double> payup_multiple;

    // Position limits (legacy _maxInternallyInitiatedPosition / _maxExternallyInitiatedPosition)
    std::vector<qty_t> max_pos_internal;
    std::vector<qty_t> max_pos_external;

    // Price-domain parameters, all in 1e-9 units
    std::vector<price_t> commission;                  // ToRhonePrice(config) * 10
    std::vector<price_t> min_internal_price_change;   // ToRhonePrice(config) * 10
    std::vector<price_t> hedge_offset_price;          // hedge_offset * tick_size
    std::vector<price_t> max_ticks_wide;              // INT64_MAX when configured < 0
    std::vector<price_t> max_ticks_wide_2x;           // same rule, then doubled
    std::vector<price_t> worsen_payup_amount;         // worsen_payup_pct * tick_size

    // Established-level EMA (legacy _emaEstLevel / _estLevelEmaPeriod / _estLevelPctEma)
    std::vector<double> est_level_ema;
    std::vector<uint64_t> est_level_ema_period;
    std::vector<double> est_level_pct_ema;

    std::vector<uint32_t> queue_importance;

    // Positions restored from the previous session (legacy _internallyInitiatedPosition...)
    std::vector<int32_t> init_internal_position;
    std::vector<int32_t> init_external_position;

    // Legacy ConfigContract::toValidQty applied with this contract's lot.
    qty_t valid_qty(uint32_t c, qty_t qty) const noexcept {
        return to_valid_qty(qty, lot[c]);
    }

    // Legacy roundToTickSize<Side> (SimpleMarketMaker.h:1792-1799): the commission is folded
    // into the price *before* the tick rounding, with the same sign convention as
    // applyPriceOffset (bid: price - commission, ask: price + commission).
    price_t round(uint32_t c, price_t price, side s) const noexcept {
        const price_t adjusted = s == side::bid ? price - commission[c] : price + commission[c];
        return round_to_tick(adjusted, tick[c], s);
    }

    double width_pct_of(uint32_t c, uint32_t shift) const noexcept {
        return width_pct[c][shift < SHIFT_COUNT ? shift : initial_shift];
    }
};

}  // namespace tachyon::trade::mm
