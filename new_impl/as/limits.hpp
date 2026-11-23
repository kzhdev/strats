#pragma once

#include <cstdint>

#include <common/limits.hpp>

// Bounded sizes so that every hot array is a single fixed allocation and the per-cycle path
// performs no heap traffic (PLAN.md D3). Exceeding a cap is an init-time failure with an
// actionable message, never a runtime path.
namespace tachyon::trade::as {

// All contracts (outrights + spreads). Process-wide: see common/limits.hpp.
using tachyon::trade::MAX_CONTRACTS;
using tachyon::trade::CONTRACT_WORDS;

inline constexpr uint32_t MAX_PATHS_PER_CONTRACT = 16;    // configured paths of one quoting contract
inline constexpr uint32_t MAX_PATH_DEFS = 512;            // configured paths, all contracts
inline constexpr uint32_t MAX_PATH_SLOTS = 1024;          // defs * 2 sides
inline constexpr uint32_t MAX_LEAN_EDGES = 4096;          // path -> lean contract edges
inline constexpr uint32_t MAX_AFFECTED_EDGES = 8192;      // lean market -> path slot edges
inline constexpr uint32_t MAX_OFFSETS = 256;              // distinct short-term-fair offsets
inline constexpr uint32_t MAX_STF_TERMS = 256;            // distinct (offset, contract) ratios
inline constexpr uint32_t MAX_DIRTY = MAX_CONTRACTS;

// Reference shapes these caps come from (config/minimal_cme): 10 contracts, 4 vectors,
// 2 quoting contracts with 4 paths each, <= 3 leans per path. The caps leave 1+ orders of
// magnitude of headroom while keeping every table inside a few cache lines per contract.
static_assert(MAX_PATH_SLOTS == MAX_PATH_DEFS * 2, "one path definition drives both sides");
static_assert(MAX_AFFECTED_EDGES >= 2 * MAX_LEAN_EDGES, "each lean edge registers both side slots");
static_assert(MAX_DIRTY == MAX_CONTRACTS, "the dirty list holds each contract at most once");

}  // namespace tachyon::trade::as
