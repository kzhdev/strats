#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "limits.hpp"
#include "types.hpp"

namespace tachyon::trade::rv {

// Immutable after init. All operators are flat row-major double arrays so that
// Eigen::Map can wrap them at the use site (no aligned-allocator pitfalls, and
// the whole model stays trivially copyable / reloadable).
class model {
public:
    // ---- construction --------------------------------------------------------
    // Immutable after this call. Throws std::runtime_error with an actionable message on an
    // invalid config (PLAN.md §4.8: caps, square model D3, invertibility V1, sanity V2/V3).
    static model build(const model_inputs& inputs);

    // ---- contract queries ----------------------------------------------------
    bool is_traded(uint32_t contract) const noexcept { return traded[contract] != 0; }
    bool is_priced(uint32_t contract) const noexcept {
        return used_in_pricing[contract] != 0;
    }
    // A spread has more than one leg; a bare or pricing contract has none.
    bool is_spread(uint32_t contract) const noexcept {
        return leg_begin[contract + 1] - leg_begin[contract] > 1;
    }
    uint32_t leg_count(uint32_t contract) const noexcept {
        return leg_begin[contract + 1] - leg_begin[contract];
    }
    uint32_t lean_count(uint32_t contract) const noexcept {
        return lean_begin[contract + 1] - lean_begin[contract];
    }

    // Index into the per-(contract, lean) tables (hedge ratio, commission, quote size, ...).
    size_t pair(uint32_t contract, uint32_t lean) const noexcept {
        return static_cast<size_t>(contract) * n_all + lean;
    }

    uint32_t n_all = 0;
    uint32_t n_pricing = 0;
    uint32_t n_vectors = 0;
    uint32_t first_traded = 0;

    // Operators
    std::vector<double> odm;         // [n_pricing][n_all]  over-determined operator
    std::vector<double> pricing;     // [n_vectors][n_pricing]
    std::vector<double> risk;        // [n_pricing][n_vectors]  (P^-1 transposed, by contract)
    std::vector<double> mult;        // [n_pricing] pricing multiplier

    // Per contract
    std::vector<price_t> width_extra;   // configured + extra width
    std::vector<price_t> threshold;     // price change threshold
    std::vector<price_t> est_level;     // weighted-mid clip
    std::vector<price_t> max_ticks_through_market;  // quote validity bound (ticks * tick size)
    std::vector<double> qmult;          // quantity multiplier
    std::vector<double> pmult;          // pricing multiplier (all contracts)
    std::vector<fast_div> tick;         // tick magic divider
    std::vector<qty_t> lot;             // valid-qty lot
    std::vector<uint8_t> traded;
    std::vector<uint8_t> used_in_pricing;  // legacy !IsZero(weight): mid is computed for these
    std::vector<uint8_t> quote_external;
    std::vector<uint8_t> weighted_mid;
    std::vector<uint8_t> static_quote;
    std::vector<uint32_t> static_quote_size;
    std::vector<uint32_t> venue;
    std::vector<price_t> rhone_max_clip;

    // Per (contract, lean) — removes ~5 divisions per lean from the cycle
    std::vector<double> hedge_ratio;       // < 0 => not quotable (pre-folded)
    std::vector<uint8_t> hedge_unity;      // 1 => all four multipliers are exactly 1.0
    std::vector<price_t> commission_scaled;
    std::vector<double> quote_size;
    std::vector<int32_t> factor_hedged_index;

    // CSR lean list: only pairs that are both leanable and quotable
    std::vector<uint32_t> lean_begin;  // [n_all + 1]
    std::vector<uint32_t> lean_index;  // [lean_count]

    // Per vector
    std::vector<double> fare_move;   // range / max_position
    std::vector<double> init_ltf;
    std::vector<uint64_t> ema_period;
    std::vector<uint8_t> use_ema;
    std::vector<uint8_t> set_to_market;
    std::vector<uint8_t> factor_hedged;

    // Spread legs
    std::vector<uint32_t> leg_begin;  // [n_all + 1]
    std::vector<uint32_t> leg_index;
    std::vector<double> leg_ratio;

    uint32_t lean_count() const noexcept { return static_cast<uint32_t>(lean_index.size()); }
};

}  // namespace tachyon::trade::rv
