#pragma once

#include <cstdint>

// The limits that are genuinely shared, in `tachyon::trade` so that every module sees them without
// qualification. Only constants that describe the *process* live here; everything that describes a
// module's own data shape stays in that module's `limits.hpp`, because those caps are allowed to
// differ and merging them would imply a coupling that does not exist.
//
// A module does not re-declare these: it brings them into its own namespace with a using-declaration
// (e.g. `mm/limits.hpp`), which both keeps call sites unqualified and turns a future duplicate
// definition into a compile error instead of a silent divergence.
//
// What deliberately stays per-module, even where the numbers happen to coincide:
//
//   rv::MAX_LEANS (4096)  vs  as::MAX_LEAN_EDGES (4096)   two unrelated graphs
//   rv::MAX_LEGS (512)    vs  as::MAX_PATHS_PER_CONTRACT  different shapes
//   mm::BOOK_LEVELS, mm::MAX_ORDERS, as::MAX_STF_TERMS, exec::MAX_FILLS_PER_FLUSH, ...
//
// Sharing a name would assert a coupling that does not exist: RV's lean graph and AS's lean graph are
// different structures with different growth rules, so tying their caps together would make one
// module's tuning change another's memory footprint for no reason. The test for inclusion here is not
// "same value" but "same object" -- the same index space, the same bitmask, the same id pool.
namespace tachyon::trade {

// Contracts the process knows about: outrights plus spreads, indexed identically everywhere.
//
// This one is shared for a reason beyond tidiness: a contract index is a *handle* that the executor
// hands to the strategies and the liquidity ledger. Four independent copies of this cap would mean
// four opportunities to drift, and a drift is not a build error -- it is an index that is valid in one
// module and past the end of an array in another. One definition makes the index space global by
// construction.
inline constexpr uint32_t MAX_CONTRACTS = 128;

// Bid and ask. A side is part of every module's vocabulary and part of the wire format, so it is one
// constant rather than four.
inline constexpr uint32_t SIDE_COUNT = 2;

// Words in a `MAX_CONTRACTS`-wide bitmask (dirty lists, touched-contract sets). Derived, so a cap
// change cannot leave one module with a bitmask that is one word too short -- the failure mode would
// be a silently untracked contract, which is exactly the kind of bug that survives for months.
inline constexpr uint32_t CONTRACT_WORDS = (MAX_CONTRACTS + 63) / 64;

// Hedge ids one host can be tracking at a time (RV and AS hedge trackers). Sized for far more
// concurrent hedges than any config in config/ produces.
inline constexpr uint32_t MAX_TRACKED_HEDGES = 256;

static_assert(MAX_CONTRACTS > 0, "a zero contract cap cannot hold anything");
static_assert(CONTRACT_WORDS * 64 >= MAX_CONTRACTS, "the bitmask must cover every contract");
static_assert(SIDE_COUNT == 2, "bid and ask; the legacy Side enum and the wire format assume two");

}  // namespace tachyon::trade
