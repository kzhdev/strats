#pragma once

// Primitive layer of the ArbieSpreader engine: fixed-point helpers, side-signed price
// arithmetic, the exact tick divider, and the POD types that cross the config/host boundary.
//
// Prices are int64 at 1e9 (`PRICE_SCALE`), matching tachyon's
// DEFAULT_FIXED_PRICE_MULTIPLIER. Legacy `hf::price_t` was 1e-8, so every value that the
// legacy code produced by *scaling a config double* is now produced by to_legacy_price(),
// which truncates at the legacy scale first (PARITY_NOTES F-AS1). Engine quantities are
// signed int64 because path sizes carry the legacy `max()` sentinel and are compared with
// `<` against lean sizes (PLAN D10, same rule as the MM engine).

#include <tachyon/core/common/types.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "limits.hpp"

namespace tachyon::trade::as {

using price_t = tachyon::price_t;        // int64, 1e-9 dollars
using wire_qty_t = tachyon::qty_t;       // unsigned, only at the order-port boundary
using qty_t = int64_t;                   // engine quantities are signed (D10)
using timestamp_t = tachyon::timestamp_t;

inline constexpr int64_t PRICE_SCALE = 1'000'000'000;
static_assert(PRICE_SCALE == tachyon::DEFAULT_FIXED_PRICE_MULTIPLIER,
              "PRICE_SCALE must match tachyon's fixed price multiplier");

// Legacy hf::price_t scale (1e-8) and the ratio between the two scales.
inline constexpr int64_t LEGACY_PRICE_SCALE = 100'000'000;
inline constexpr int64_t SCALE_RATIO = PRICE_SCALE / LEGACY_PRICE_SCALE;
static_assert(SCALE_RATIO == 10, "1e-9 is one decade finer than legacy 1e-8");

inline constexpr price_t NULL_PRICE = tachyon::NULL_PRICE;
inline constexpr uint32_t INVALID_INDEX = UINT32_MAX;

// Legacy ToRhonePrice, evaluated at the *legacy* scale and then widened, so an init-time
// number derived from a config double is bit-identical to what legacy computed (F-AS1).
// A plain 1e-9 conversion (rv's to_scaled) truncates one decade finer and can differ by up
// to 9 units at 1e-9 (0.9 legacy units); the gate pins this choice.
inline price_t to_legacy_price(double dollars) noexcept {
    const double scaled = dollars * static_cast<double>(LEGACY_PRICE_SCALE);
    if (!(scaled > -9.0e17 && scaled < 9.0e17)) {  // also catches NaN; avoids UB on the cast
        return scaled > 0 ? static_cast<price_t>(9.0e17) : static_cast<price_t>(-9.0e17);
    }
    return static_cast<price_t>(scaled) * SCALE_RATIO;
}

// Dollars -> 1e-9 units, truncating (used for test fixtures only).
constexpr price_t to_scaled(double dollars) noexcept {
    return static_cast<price_t>(dollars * static_cast<double>(PRICE_SCALE));
}

inline double to_dollars(price_t value) noexcept {
    return static_cast<double>(value) / static_cast<double>(PRICE_SCALE);
}

// ---------------------------------------------------------------------------
// Sides
// ---------------------------------------------------------------------------

enum class side : uint8_t { bid = 0, ask = 1 };
// Process-wide (common/limits.hpp): a side is part of every module's vocabulary and of the
// wire format, so it must not be a per-module guess.
using tachyon::trade::SIDE_COUNT;

constexpr uint32_t to_index(side s) noexcept { return static_cast<uint32_t>(s); }
constexpr side other(side s) noexcept { return s == side::bid ? side::ask : side::bid; }

// legacy applyPriceOffset<Side> (Common.h:437-440): BID adds the offset, ASK subtracts it.
template <side S>
constexpr price_t apply_price_offset(price_t price, price_t offset) noexcept {
    if constexpr (S == side::bid) {
        return price + offset;
    } else {
        return price - offset;
    }
}

constexpr price_t apply_price_offset(side s, price_t price, price_t offset) noexcept {
    return s == side::bid ? price + offset : price - offset;
}

// legacy IsBetterPrice<Side> (Common.h:335-342).
template <side S>
constexpr bool is_better_price(price_t one, price_t two) noexcept {
    if constexpr (S == side::bid) {
        return one > two;
    } else {
        return one < two;
    }
}

template <side S>
constexpr bool is_price_better_or_equal(price_t one, price_t two) noexcept {
    if constexpr (S == side::bid) {
        return one >= two;
    } else {
        return one <= two;
    }
}

// legacy BetterPrice<Side> (Common.h:345-350): NULL loses against any real price.
template <side S>
constexpr price_t better_price(price_t lhs, price_t rhs) noexcept {
    if (lhs == NULL_PRICE) return rhs;
    if (rhs == NULL_PRICE) return lhs;
    return is_better_price<S>(lhs, rhs) ? lhs : rhs;
}

// legacy calculateTicksThroughMarket<Side> (Common.h:446-453).
template <side S>
constexpr price_t ticks_through_market(price_t quote_price, price_t other_side_best) noexcept {
    if constexpr (S == side::bid) {
        return quote_price - other_side_best;
    } else {
        return other_side_best - quote_price;
    }
}

// ---------------------------------------------------------------------------
// Exact tick division (byte-identical to rv/types.hpp; consolidation into
// common/price.hpp is pending, the same open item as mm PLAN R6).
// ---------------------------------------------------------------------------

// Above this magnitude the double reciprocal can be off by more than a couple of units, so
// the hardware division is used instead. Both paths return exactly the same quotient.
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

// legacy RoundToTickSizeConstant (Common.h:1199-1210): bids floor, asks ceil, negatives
// included. This is the oracle for the fast_div path below.
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
#ifndef AS_CONSTANT_TICK_DISPATCH
#define AS_CONSTANT_TICK_DISPATCH 1
#endif

inline price_t round_to_tick(price_t price, const fast_div& fd, side s) noexcept {
#if AS_CONSTANT_TICK_DISPATCH
    // Frequency-ordered from config/*/secdb*.csv, expressed relative to PRICE_SCALE so the
    // list stays correct if the scale changes. Anything else falls through to floor_divmod.
    switch (fd.divisor) {
        case PRICE_SCALE / 100:      return round_to_tick_constant<PRICE_SCALE / 100>(price, s);       // 0.01
        case PRICE_SCALE / 1000:     return round_to_tick_constant<PRICE_SCALE / 1000>(price, s);      // 0.001
        case PRICE_SCALE / 10000:    return round_to_tick_constant<PRICE_SCALE / 10000>(price, s);     // 0.0001
        case PRICE_SCALE / 64:       return round_to_tick_constant<PRICE_SCALE / 64>(price, s);        // 1/64
        case PRICE_SCALE / 20:       return round_to_tick_constant<PRICE_SCALE / 20>(price, s);        // 0.05
        case PRICE_SCALE / 4:        return round_to_tick_constant<PRICE_SCALE / 4>(price, s);         // 0.25
        case PRICE_SCALE / 100000:   return round_to_tick_constant<PRICE_SCALE / 100000>(price, s);    // 0.00001
        case PRICE_SCALE / 200:      return round_to_tick_constant<PRICE_SCALE / 200>(price, s);       // 0.005
        case PRICE_SCALE / 40:       return round_to_tick_constant<PRICE_SCALE / 40>(price, s);        // 0.025
        case PRICE_SCALE:            return round_to_tick_constant<PRICE_SCALE>(price, s);             // 1.0
        case PRICE_SCALE / 2:        return round_to_tick_constant<PRICE_SCALE / 2>(price, s);         // 0.5
        case PRICE_SCALE / 128:      return round_to_tick_constant<PRICE_SCALE / 128>(price, s);       // 1/128
        case PRICE_SCALE / 400:      return round_to_tick_constant<PRICE_SCALE / 400>(price, s);       // 0.0025
        case PRICE_SCALE / 10:       return round_to_tick_constant<PRICE_SCALE / 10>(price, s);        // 0.1
        default:                     break;
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

// Runtime side dispatch for the templated price predicates in types.hpp (the hot path uses the
// template form; the state layer needs the runtime form for a side it learns from data).
inline bool is_better_price(side s, price_t one, price_t two) noexcept {
    return s == side::bid ? one > two : one < two;
}

inline bool is_price_better_or_equal(side s, price_t one, price_t two) noexcept {
    return s == side::bid ? one >= two : one <= two;
}

inline price_t better_price(side s, price_t lhs, price_t rhs) noexcept {
    if (lhs == NULL_PRICE) return rhs;
    if (rhs == NULL_PRICE) return lhs;
    return is_better_price(s, lhs, rhs) ? lhs : rhs;
}

inline price_t ticks_through_market(side s, price_t quote_price, price_t other_side_best) noexcept {
    return s == side::bid ? quote_price - other_side_best : other_side_best - quote_price;
}

// ---------------------------------------------------------------------------
// Configuration inputs (framework agnostic). `config.cpp` fills these from the model CSV,
// the AS paths CSV and the secdb; `model::build` freezes them into CSR tables.
// ---------------------------------------------------------------------------

// One entry of a path's basket: `multiplier` units of `contract`.
struct lean_input {
    uint32_t contract = INVALID_INDEX;
    int32_t multiplier = 0;
};

struct path_input {
    std::string name;               // paths CSV row label; diagnostics only
    std::vector<lean_input> leans;  // non-zero multipliers, in file column order
    price_t commission = 0;         // paths CSV `Commission`, legacy-quantised (F-AS1)
    bool enabled = false;           // paths CSV `Enabled` column
};

// One row of the `ASConfigs` section, with the per-contract numbers already derived the way
// legacy initializeContracts computes them (ArbieSpreader.cpp:52-62):
//   edge                  = ToRhonePrice(edge * ASEdgeMultiplier)        (:58)
//   max_ticks_through_market = maxTicksThroughMarket * tickSize          (:61)
//   hedge_offset_price    = hedgeOffset * tickSize                       (:62)
struct as_contract_input {
    std::string name;
    bool enabled = false;
    bool quote_external = false;
    qty_t quote_size = 0;
    price_t edge = 0;
    price_t hedge_offset_price = 0;
    price_t max_ticks_through_market = 0;
    std::vector<path_input> paths;
};

struct model_inputs {
    uint32_t n_contracts = 0;
    uint32_t n_pricing = 0;  // PricingMatrix columns; contract id == column index
    uint32_t n_vectors = 0;

    std::vector<std::string> names;  // [n_contracts], id order

    // Per contract (all contracts, not just the quoting ones).
    std::vector<price_t> tick_size;
    std::vector<price_t> ext_commission;  // legacy-quantised dollars (F-AS1)
    std::vector<qty_t> lot_size;
    std::vector<uint32_t> venue;
    std::vector<double> pricing_multiplier;
    std::vector<double> quantity_multiplier;
    std::vector<uint32_t> leg_begin;  // [n_contracts + 1]
    std::vector<uint32_t> leg_index;
    std::vector<int32_t> leg_ratio;

    // Per vector.
    std::vector<double> range;
    std::vector<double> max_position;

    // Operators. Init-time only (PLAN D6): `risk` is the only place Eigen is used, and it is
    // consumed once here to derive each path's short-term-fair move.
    std::vector<double> pricing_matrix;  // [n_vectors][n_pricing]
    std::vector<double> risk;            // [n_vectors][n_pricing] = (P^-1)^T

    std::vector<as_contract_input> contracts;  // ASConfigs rows, enabled or not
};

// ---------------------------------------------------------------------------
// Runtime state PODs
// ---------------------------------------------------------------------------

struct market_state {
    price_t price = NULL_PRICE;
    qty_t size = 0;
};

// Index of a book: ((contract * 2 + side) * 2) + is_external.
constexpr uint32_t market_index(uint32_t contract, side s, bool external) noexcept {
    return ((contract * SIDE_COUNT) + to_index(s)) * 2 + (external ? 1u : 0u);
}

// One path instance = a (path definition, side) pair.
struct path_state {
    price_t price = NULL_PRICE;                // legacy _priceSize._price
    qty_t size = 0;                            // legacy _priceSize._size
    uint32_t size_determiner = INVALID_INDEX;  // legacy _sizeDetermineContract
    bool recalc = false;                       // legacy _recalc
    bool resort = false;                       // legacy _resort
    bool dirty = false;                        // legacy _dirty
    bool lean_on_rv = false;                   // legacy _leanOnRV

    bool valid() const noexcept { return price != NULL_PRICE; }
    // legacy PriceQtyPair::reset() (ArbieSpreader.h:60-63) clears the price and the size only.
    // `size_determiner` deliberately survives: legacy keeps `_sizeDetermineContract` across a
    // reset, and the incremental update's "can quote more" branch reads it (P3).
    void reset() noexcept {
        price = NULL_PRICE;
        size = 0;
    }
};

// What the quote stage produces for one contract and side (legacy Contract::Quote).
struct quote {
    price_t lean_price = NULL_PRICE;
    price_t internal_price = NULL_PRICE;
    price_t external_price = NULL_PRICE;
    qty_t internal_size = 0;
    qty_t external_size = 0;
    int32_t hedge_path = -1;

    bool operator==(const quote& rhs) const noexcept {
        return internal_price == rhs.internal_price && internal_size == rhs.internal_size &&
               external_price == rhs.external_price && external_size == rhs.external_size;
    }
    bool operator!=(const quote& rhs) const noexcept { return !(*this == rhs); }

    void reset() noexcept {
        lean_price = internal_price = external_price = NULL_PRICE;
        internal_size = external_size = 0;
        hedge_path = -1;
    }
};

}  // namespace tachyon::trade::as
