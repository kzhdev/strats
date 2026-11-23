#pragma once

#include <cstdint>

#include <common/limits.hpp>

// Bounded sizes so that every hot array is a single static allocation and the
// per-cycle path performs no heap traffic. Exceeding a cap is an init-time error.
namespace tachyon::trade::rv {

// n_all (outrights + spreads). Process-wide, see common/limits.hpp.
using tachyon::trade::MAX_CONTRACTS;
using tachyon::trade::CONTRACT_WORDS;

inline constexpr uint32_t MAX_PRICING = 32;     // n_pricing
inline constexpr uint32_t MAX_VECTORS = 32;     // n_vectors
inline constexpr uint32_t MAX_LEANS = 4096;     // total CSR edges
inline constexpr uint32_t MAX_LEGS = 512;       // total spread legs
inline constexpr uint32_t MAX_CHANGED = MAX_CONTRACTS;

// D3: the legacy model inverts a square pricing matrix, so the factor space and
// the pricing-contract space must have the same dimension.
static_assert(MAX_PRICING == MAX_VECTORS, "square model assumption (D3)");

}  // namespace tachyon::trade::rv
