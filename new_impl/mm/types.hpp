#pragma once

#include <tachyon/core/common/types.hpp>

#include <cstdint>

#include "limits.hpp"

namespace tachyon::trade::mm {

using price_t = tachyon::price_t;
using timestamp_t = tachyon::timestamp_t;

// Quantities are *signed*, to match the legacy type exactly: hf::qty_t is
// rhone::common::QtyT = int_fast64_t (rhonehf/include_public/hf/hf.h:54,
// neorhone/common/RhoneTypes.h:16). The strategy relies on that signedness in three places that
// would silently change meaning with an unsigned type:
//   * positions flip sign and are offset with `position < 0` / `-position`
//     (tryOffsetPosition, SimpleMarketMaker.h:28-48);
//   * a queue position of -1 means "not set yet" and is tested with `qp >= 0` and
//     `qp == NOT_SET` (:816, :835, :1300-1304);
//   * the hedge size is the negated net position (:1102-1105).
// tachyon::qty_t is unsigned, so it is used only at the order-port boundary (wire_qty_t).
using qty_t = int64_t;
using wire_qty_t = tachyon::qty_t;

// ---------------------------------------------------------------------------
// Fixed-point price scale — decision D2 (see PLAN.md and PARITY_NOTES.md §3).
//
// The new engine works at 1e-9, matching tachyon's DEFAULT_FIXED_PRICE_MULTIPLIER and
// new_impl/rv. The legacy strategy works at 1e-8 (Common.h:267 ToRhonePrice).
//
// Every *configuration* price goes through to_mm_price(), which reproduces the legacy
// double -> int64 truncation at 1e-8 and only then multiplies by exactly 10. That keeps
// all config-derived values exactly ten times their legacy counterparts, so all
// scaling-invariant arithmetic (+, -, *, comparisons, % tick rounding) is
// decision-identical. A plain `(int64)(x * 1e9)` would NOT be: the MMConfigs value
// 0.65477777777 is 654'777'777 at 1e-9 but 654'777'770 by the legacy rule (65'477'777 * 10),
// because the legacy product truncates the .777... tail first. Clean decimals such as 0.025
// agree either way; the difference only shows up for values with more than eight decimals,
// which is exactly what MMWorsenPayUpCondPct and the EstLevel columns contain.
//
// The places where 1e-9 can still differ from legacy are the truncating divisions and
// truncating double products listed in PLAN.md §D2; each is recorded in PARITY_NOTES §3
// and bounded by the differential gate.
// ---------------------------------------------------------------------------
inline constexpr int64_t LEGACY_PRICE_SCALE = 100'000'000;  // legacy 1e-8
inline constexpr int64_t PRICE_SCALE = 1'000'000'000;       // new engine 1e-9
inline constexpr int64_t LEGACY_TO_MM = PRICE_SCALE / LEGACY_PRICE_SCALE;  // 10

static_assert(PRICE_SCALE == tachyon::DEFAULT_FIXED_PRICE_MULTIPLIER,
              "PRICE_SCALE must match tachyon's fixed price multiplier");
static_assert(LEGACY_TO_MM == 10, "legacy scale is exactly one decade coarser");

// Legacy truncation of a config double, then an exact power-of-ten rescale (D2).
inline price_t to_mm_price(double dollars) noexcept {
    return static_cast<price_t>(dollars * static_cast<double>(LEGACY_PRICE_SCALE)) * LEGACY_TO_MM;
}

inline double to_dollars(price_t value) noexcept {
    return static_cast<double>(value) / static_cast<double>(PRICE_SCALE);
}

// D2 safety valve (PLAN.md §D2). When true, the truncating sites run their intermediate
// arithmetic in the legacy 1e-8 domain, which makes the engine bit-identical to the legacy
// strategy. Shipped false (pure 1e-9 semantics, as decided); flipped on only if the
// differential gate needs it, and the reason is then recorded in PARITY_NOTES §3.
inline constexpr bool kLegacyTruncation = false;

inline price_t legacy_trunc(price_t value) noexcept {
    // The flag is a compile-time constant, so the ternary folds away entirely when off.
    return kLegacyTruncation ? (value / LEGACY_TO_MM) * LEGACY_TO_MM : value;
}

// Legacy IsZero (Common.h:921), used by the config validation rules
// (ViperConfig.h:901 rejects a worsen-payup percentage that is zero or negative).
inline constexpr double EPSILON = 0.00000001;
inline bool is_zero(double value) noexcept {
    return value > 0 ? value <= EPSILON : value >= -EPSILON;
}

enum class side : uint8_t { bid = 0, ask = 1 };
// Process-wide (common/limits.hpp): a side is part of every module's vocabulary and of the
// wire format, so it must not be a per-module guess.
using tachyon::trade::SIDE_COUNT;

constexpr uint32_t to_index(side s) noexcept { return static_cast<uint32_t>(s); }
constexpr side other(side s) noexcept { return s == side::bid ? side::ask : side::bid; }

// Legacy applyPriceOffset (Common.h:433-440). Note the inverted-looking sign convention:
// a positive offset moves a bid up and an ask down.
inline constexpr price_t apply_price_offset(price_t price, price_t offset, side s) noexcept {
    return s == side::bid ? price + offset : price - offset;
}

// Legacy IsBetterPrice / IsPriceBetterOrEqual (Common.h:331-363).
inline constexpr bool is_better_price(price_t one, price_t two, side s) noexcept {
    return s == side::bid ? one > two : one < two;
}

inline constexpr bool is_better_or_equal(price_t one, price_t two, side s) noexcept {
    return s == side::bid ? one >= two : one <= two;
}

// Legacy BetterPrice (Common.h:344-350): a NULL price always loses.
inline constexpr price_t better_price(price_t lhs, price_t rhs, side s) noexcept {
    if (lhs == tachyon::NULL_PRICE) return rhs;
    if (rhs == tachyon::NULL_PRICE) return lhs;
    return is_better_price(lhs, rhs, s) ? lhs : rhs;
}

// Legacy ConfigContract::toValidQty (ViperConfig.h:85-87). Applied to every quantity the
// strategy emits.
inline constexpr qty_t to_valid_qty(qty_t qty, qty_t lot) noexcept {
    return lot <= 1 ? qty : qty - (qty % lot);
}

// |a - b| for prices, computed without signed overflow.
//
// Legacy writes `std::abs(internalPrice - _quotes[Side]._lastInternalPriceSent)`
// (SimpleMarketMaker.h:675) and `std::abs(hedge._lastInternalPriceSent - internalPrice)`
// (:648). On the first cycle the "last sent" value is still NULL_PRICE (INT64_MAX), so the
// int64 difference overflows — undefined behaviour that happens to wrap to a huge magnitude and
// therefore always reports "changed". The widened computation below is well defined and yields
// the same decision (PARITY_NOTES §2e).
inline uint64_t abs_diff(price_t a, price_t b) noexcept {
#if defined(__SIZEOF_INT128__)
    const __int128 d = static_cast<__int128>(a) - static_cast<__int128>(b);
    return static_cast<uint64_t>(d < 0 ? -d : d);
#else
    const uint64_t d = static_cast<uint64_t>(a) - static_cast<uint64_t>(b);
    return (d >> 63) ? (~d + 1) : d;  // two's-complement magnitude
#endif
}

// ---------------------------------------------------------------------------
// Exact tick division.
//
// round_to_tick() reproduces Common.h:1196 RoundToTickSize exactly, including negative
// prices, but replaces the hardware int64 `%` with a magic reciprocal. These primitives
// are intentionally a copy of new_impl/rv/types.hpp:65-202 (same semantics, proven there by
// rv_tests::test_fast_div over 40M cases) rather than a shared header, so this phase leaves
// the already-verified RV translation units untouched. Consolidating both into
// common/price.hpp is a Phase-7 cleanup (PLAN.md §7).
// ---------------------------------------------------------------------------

// Above this magnitude the double reciprocal can be off by more than a couple of units, so
// we fall back to hardware division. This is a performance threshold, not a correctness
// one: both paths return exactly the same quotient/remainder.
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

// RoundToTickSize: bids floor, asks ceil, negative prices included.
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

// Compile-time constant dispatch for the tick sizes present in config/*/secdb*.csv. Set to
// 0 to measure the fast_div path alone (kept as a switch so the choice stays measurable,
// mirroring RV_CONSTANT_TICK_DISPATCH).
#ifndef MM_CONSTANT_TICK_DISPATCH
#define MM_CONSTANT_TICK_DISPATCH 1
#endif

inline price_t round_to_tick(price_t price, const fast_div& fd, side s) noexcept {
#if MM_CONSTANT_TICK_DISPATCH
    // Expressed relative to PRICE_SCALE so the list stays correct if the scale changes. MM
    // tick sizes are exactly ten times the legacy ones at 1e-9, so the same divisor set
    // applies (0.025 is the common one in config/backtest).
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
        case PRICE_SCALE / 2000000:    return round_to_tick_constant<PRICE_SCALE / 2000000>(price, s);   // 5e-7
        case PRICE_SCALE / 10000000:   return round_to_tick_constant<PRICE_SCALE / 10000000>(price, s);  // 1e-7
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

}  // namespace tachyon::trade::mm
