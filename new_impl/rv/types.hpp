#pragma once

#include <tachyon/core/common/types.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "limits.hpp"

namespace tachyon::trade::rv {

using price_t = tachyon::price_t;
using qty_t = tachyon::qty_t;
using timestamp_t = tachyon::timestamp_t;

// Fixed-point scale for prices: 1e9, matching tachyon's
// DEFAULT_FIXED_PRICE_MULTIPLIER (CME exponent -9). Note this is one decade finer
// than the legacy hf::price_t / ToRhonePrice scale (1e-8); see PARITY_NOTES F5.
inline constexpr int64_t PRICE_SCALE = 1'000'000'000;
static_assert(PRICE_SCALE == tachyon::DEFAULT_FIXED_PRICE_MULTIPLIER,
              "PRICE_SCALE must match tachyon's fixed price multiplier");

enum class side : uint8_t { bid = 0, ask = 1 };
// Process-wide (common/limits.hpp): a side is part of every module's vocabulary and of the
// wire format, so it must not be a per-module guess.
using tachyon::trade::SIDE_COUNT;

constexpr uint32_t to_index(side s) noexcept { return static_cast<uint32_t>(s); }
constexpr side other(side s) noexcept { return s == side::bid ? side::ask : side::bid; }

// S1: production ToRhonePrice truncates (the +0.5 variant is unit-test only).
inline price_t to_rhone_price(double value) noexcept {
    return static_cast<price_t>(value * static_cast<double>(PRICE_SCALE));
}
inline double to_dollars(price_t value) noexcept {
    return static_cast<double>(value) / static_cast<double>(PRICE_SCALE);
}

// Dollars -> price units at compile time, for tests and configuration constants.
constexpr price_t to_scaled(double dollars) noexcept {
    return static_cast<price_t>(dollars * static_cast<double>(PRICE_SCALE));
}

// Saturating double -> price units. The legacy 1e-8 scale has ten times the range of the 1e9
// scale, so a config whose ltf column holds numbers at legacy scale still runs there while
// overflowing here (PARITY_NOTES F6). Signed overflow is UB, so clamp instead; in-range values
// are unchanged and therefore bit-identical to a plain cast.
inline constexpr double PRICE_SATURATION = 9.2e18;

inline price_t saturate_price(double value) noexcept {
    if (!(value > -PRICE_SATURATION && value < PRICE_SATURATION)) {  // also catches NaN
        if (std::isnan(value) || value == 0.0) {
            return 0;
        }
        return value > 0 ? static_cast<price_t>(PRICE_SATURATION)
                         : -static_cast<price_t>(PRICE_SATURATION);
    }
    return static_cast<price_t>(value);
}

inline price_t to_rhone_price_saturated(double dollars) noexcept {
    return saturate_price(dollars * static_cast<double>(PRICE_SCALE));
}

// ---------------------------------------------------------------------------
// Exact tick division (replaces the per-lean int64 `%` of RoundToTickSize).
//
// floor_divmod() reproduces (x / d, x % d) with floor semantics; divmod_trunc()
// reproduces C++ trunc semantics. round_to_tick() reproduces
// Common.h:1196 RoundToTickSize exactly, including negative prices.
// ---------------------------------------------------------------------------

// Above this magnitude the double reciprocal can be off by more than a couple of
// units, so we fall back to hardware division. This is a *performance* threshold,
// not a correctness one: both paths return exactly the same quotient/remainder.
// 2^52 keeps the correction loops to <=2 iterations; 2^53 is the largest magnitude
// where (double)x is still exact. At the 1e9 scale it covers prices up to $4.5m.
inline constexpr int64_t FAST_DIV_LIMIT = int64_t(1) << 52;

struct fast_div {
    int64_t divisor = 1;
    double reciprocal = 1.0;
    bool power_of_two = false;
    uint8_t shift = 0;
};

inline fast_div make_fast_div(int64_t divisor) noexcept {
    fast_div fd;
    fd.divisor = divisor;
    fd.reciprocal = 1.0 / static_cast<double>(divisor);
    if ((divisor & (divisor - 1)) == 0) {  // divisor is a power of two
        fd.power_of_two = true;
        uint8_t shift = 0;
        int64_t d = divisor;
        while (d > 1) {
            d >>= 1;
            ++shift;
        }
        fd.shift = shift;
    }
    return fd;
}

// Floor division: q = floor(x / d), r = x - q*d, 0 <= r < d.  d > 0.
inline void floor_divmod(int64_t x, const fast_div& fd, int64_t& q, int64_t& r) noexcept {
    if (fd.power_of_two) {
        q = x >> fd.shift;             // arithmetic shift == floor
        r = x & (fd.divisor - 1);      // non-negative remainder
        return;
    }
    if (x <= -FAST_DIV_LIMIT || x >= FAST_DIV_LIMIT) [[unlikely]] {
        q = x / fd.divisor;
        r = x % fd.divisor;
        if (r < 0) {
            r += fd.divisor;
            --q;
        }
        return;
    }
    int64_t estimate = static_cast<int64_t>(static_cast<double>(x) * fd.reciprocal);
    int64_t remainder = x - estimate * fd.divisor;
    while (remainder < 0) {
        remainder += fd.divisor;
        --estimate;
    }
    while (remainder >= fd.divisor) {
        remainder -= fd.divisor;
        ++estimate;
    }
    q = estimate;
    r = remainder;
}

// C++ truncating division semantics, i.e. exactly (x / d, x % d).
inline void divmod_trunc(int64_t x, const fast_div& fd, int64_t& q, int64_t& r) noexcept {
    floor_divmod(x, fd, q, r);
    if (r != 0 && x < 0) {
        ++q;
        r -= fd.divisor;
    }
}

// S2: RoundToTickSize — bids floor, asks ceil, negative prices included.
// The legacy expression, used for the compile-time-constant fast paths below. Proven
// bit-identical to the floor_divmod form (rv_tests::test_fast_div, 40M cases).
template <int64_t TickSize>
inline price_t round_to_tick_constant(price_t price, side s) noexcept {
    static_assert(TickSize > 0, "tick size must be positive");
    const price_t remainder = price % TickSize;
    if (!remainder) {
        return price;
    }
    const price_t truncated = price - remainder;  // truncates toward zero
    if (s == side::bid) {
        return remainder > 0 ? truncated : truncated - TickSize;
    }
    return remainder > 0 ? truncated + TickSize : truncated;
}

// Compile-time constant dispatch for the tick sizes present in the configs. Set to 0 to
// measure the fast_div path alone (kept as a switch so the choice stays measurable).
#ifndef RV_CONSTANT_TICK_DISPATCH
#define RV_CONSTANT_TICK_DISPATCH 1
#endif

inline price_t round_to_tick(price_t price, const fast_div& fd, side s) noexcept {
#if RV_CONSTANT_TICK_DISPATCH
    // Fast paths for the tick sizes present in the configs (frequency-ordered from
    // config/*/secdb*.csv), expressed relative to PRICE_SCALE so the list stays correct
    // if the scale changes. A compile-time divisor lets the compiler emit an integer
    // multiply/shift instead of either a hardware idiv or the double-reciprocal path;
    // anything else falls through to floor_divmod, which accepts any runtime divisor.
    switch (fd.divisor) {
        case PRICE_SCALE / 100:        return round_to_tick_constant<PRICE_SCALE / 100>(price, s);       // 0.01
        case PRICE_SCALE / 1000:       return round_to_tick_constant<PRICE_SCALE / 1000>(price, s);      // 0.001
        case PRICE_SCALE / 10000:      return round_to_tick_constant<PRICE_SCALE / 10000>(price, s);     // 0.0001
        case PRICE_SCALE / 64:         return round_to_tick_constant<PRICE_SCALE / 64>(price, s);        // 1/64
        case PRICE_SCALE / 20:         return round_to_tick_constant<PRICE_SCALE / 20>(price, s);        // 0.05
        case PRICE_SCALE / 4:          return round_to_tick_constant<PRICE_SCALE / 4>(price, s);         // 0.25
        case PRICE_SCALE / 100000:     return round_to_tick_constant<PRICE_SCALE / 100000>(price, s);    // 0.00001
        case PRICE_SCALE / 200:        return round_to_tick_constant<PRICE_SCALE / 200>(price, s);       // 0.005
        case PRICE_SCALE / 40:         return round_to_tick_constant<PRICE_SCALE / 40>(price, s);        // 0.025
        case PRICE_SCALE:              return round_to_tick_constant<PRICE_SCALE>(price, s);             // 1.0
        case PRICE_SCALE / 2:          return round_to_tick_constant<PRICE_SCALE / 2>(price, s);         // 0.5
        case PRICE_SCALE / 128:        return round_to_tick_constant<PRICE_SCALE / 128>(price, s);       // 1/128
        case PRICE_SCALE / 400:        return round_to_tick_constant<PRICE_SCALE / 400>(price, s);       // 0.0025
        case PRICE_SCALE / 10:         return round_to_tick_constant<PRICE_SCALE / 10>(price, s);        // 0.1
        case PRICE_SCALE / 2000000:    return round_to_tick_constant<PRICE_SCALE / 2000000>(price, s);   // 5e-7 (6J outright)
        case PRICE_SCALE / 10000000:   return round_to_tick_constant<PRICE_SCALE / 10000000>(price, s);  // 1e-7 (6J calendar)
        default:                       break;
    }
#endif

    int64_t q = 0;
    int64_t r = 0;
    floor_divmod(price, fd, q, r);
    if (r == 0) {
        return price;
    }
    const price_t truncated = q * fd.divisor;  // == price - r (floor)
    return s == side::bid ? truncated : truncated + fd.divisor;
}

// Configuration inputs (framework agnostic; config_adapter fills these from
// ViperConfig — PLAN.md D2).
struct leg_input {
    uint32_t index = 0;
    double ratio = 0.0;
};

struct vector_input {
    double range = 0.0;
    double max_position = 0.0;
    double initial_ltf = 0.0;
    uint64_t ema_period_cycles = 0;
    bool use_ema = false;
    bool set_to_market = false;
    bool factor_hedged = false;
};

struct contract_input {
    price_t tick_size = 1;
    double pricing_multiplier = 1.0;
    double quantity_multiplier = 1.0;
    price_t width = 0;                 // configured RV width
    price_t extra_width = 0;           // computed extra width
    price_t price_change_threshold = 0;
    price_t established_level = 0;
    // rvConfig._maxTicksThroughMarket * tickSize (RelativeValue.h:2193). Quote validity bound:
    // a lean price may not sit more than this many ticks through the opposite side of the
    // traded contract's own book (validateMaxTicksThroughMarket, :2795-2824).
    price_t max_ticks_through_market = 0;
    qty_t lot_size = 1;
    double weight = 1.0;               // ODM weighting
    uint32_t venue = 0;
    uint32_t static_quote_size = 0;
    uint32_t leg_begin = 0;
    uint32_t leg_count = 0;
    bool traded = false;
    bool quote_external = false;
    bool weighted_mid = false;
    bool static_quote = false;
};

struct model_inputs {
    uint32_t n_contracts = 0;
    uint32_t n_pricing = 0;
    uint32_t n_vectors = 0;
    uint32_t first_traded = 0;

    // Contract names in id order (PricingMatrix columns first, then RV-only contracts).
    // Reports the ordering rule of config_model.cpp:46-80 so that a consumer which keys its
    // own tables by contract id (e.g. the AS engine, whose path leans are given by name)
    // cannot drift from it. Not used by the RV engine itself.
    std::vector<std::string> names;

    std::vector<double> pricing_matrix;        // [n_vectors][n_pricing]
    std::vector<double> hedge_ratio;           // [n_all][n_all]
    std::vector<double> quote_size;            // [n_all][n_all]
    std::vector<price_t> commissions;          // [n_all][n_all]
    std::vector<uint8_t> leans;                // [n_all][n_all] lean matrix
    std::vector<int32_t> factor_hedged_index;  // [n_all][n_all]
    std::vector<price_t> rhone_max_clip;       // [n_all]

    std::vector<contract_input> contracts;
    std::vector<vector_input> vectors;
    std::vector<leg_input> legs;
};

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

struct market_state {
    price_t price = tachyon::NULL_PRICE;
    qty_t size = 0;
};

// Index: ((contract * 2 + side) * 2) + is_external
constexpr uint32_t market_index(uint32_t contract, side s, bool external) noexcept {
    return ((contract * SIDE_COUNT) + to_index(s)) * 2 + (external ? 1u : 0u);
}

struct quote {
    price_t price = tachyon::NULL_PRICE;  // best lean price, unrounded (legacy _bestPrice)
    qty_t size = 0;
    uint32_t lean = 0;
    price_t external_price = tachyon::NULL_PRICE;
    qty_t external_size = 0;
    bool has_change = false;
    bool market_gone = false;

    void reset() noexcept {
        price = tachyon::NULL_PRICE;
        size = 0;
        lean = 0;
        external_price = tachyon::NULL_PRICE;
        external_size = 0;
    }
};

struct hedge_order {
    uint32_t contract = 0;
    side side_ = side::bid;
    price_t price = tachyon::NULL_PRICE;
    qty_t size = 0;
    bool external = false;
    uint32_t hedge_id = 0;
    uint32_t market_index = 0;
};

// What the quote lifecycle emits for one contract and side (legacy Quote fields sent by
// sendQuoteOrder, RelativeValue.h:2916-2941). Emitted by the host, not by the engine: the
// engine only records the desired state in cycle_state.quotes[].
struct quote_request {
    uint32_t contract = 0;
    side side_ = side::bid;
    price_t price = tachyon::NULL_PRICE;           // legacy _bestPrice (internal book, unrounded)
    qty_t size = 0;                                // legacy _bestSize
    price_t external_price = tachyon::NULL_PRICE;  // legacy _externalPrice (rounded club price)
    qty_t external_size = 0;                       // legacy _externalSize
    bool quote_external = false;                   // canQuoteExternal: contract flag & venue gate
};

}  // namespace tachyon::trade::rv

