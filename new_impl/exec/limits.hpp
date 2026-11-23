#pragma once

#include <cstdint>

#include <common/limits.hpp>

// Bounded sizes so that every hot array is a single static allocation and the drain path
// performs no heap traffic (PLAN.md D-EX2). Exceeding a cap is an init-time error or a
// counted cold-path drop, never a runtime allocation.
namespace tachyon::trade::exec {

// Contracts the executor can route, sides (bid/ask) and the width of a contract bitmask are
// process-wide: the executor hands contract indices to the strategies and the ledger, so a
// local cap could drift from theirs without any build error. See common/limits.hpp.
using tachyon::trade::MAX_CONTRACTS;
using tachyon::trade::SIDE_COUNT;
using tachyon::trade::CONTRACT_WORDS;

// Strategies that own a mailbox: MM, RV, AS (common/types.hpp strategy_id). Pinned against the
// enum in types.hpp, so adding a strategy cannot silently leave a mailbox out.
inline constexpr uint32_t STRATEGY_COUNT = 3;

// Request kinds (types.hpp).
inline constexpr uint32_t KIND_COUNT = 2;   // quote, hedge

// One addressable request stream per (strategy, kind, side) within a contract -- the exact key
// space of the legacy per-contract OrderRequestQueue (InternalMatchingEngine.h:1943-1971).
inline constexpr uint32_t SLOTS_PER_CONTRACT = STRATEGY_COUNT * KIND_COUNT * SIDE_COUNT;

// Resting streams (the ones whose request becomes part of the internal book). RV/AS hedges are
// FAK and never rest, so they need no staged slot.
inline constexpr uint32_t RESTING_SLOTS = 6;

// Mailbox capacity, in requests, per strategy. Must be a power of two. One executor cycle drains
// at most MAX_CONTRACTS * SLOTS_PER_CONTRACT ~= 1.5 k requests in the pathological case where
// every contract and every key moved, so 1024 covers a cycle plus the backlog of a slow cycle;
// the ring is 64 B per entry, i.e. 64 KB per strategy.
inline constexpr uint32_t DEFAULT_QUEUE_CAPACITY = 1u << 10;

// Internal book depth per side: at most one resting order per (strategy, kind) key, so
// SLOTS_PER_CONTRACT / SIDE_COUNT.
inline constexpr uint32_t MAX_INTERNAL_ORDERS = SLOTS_PER_CONTRACT / SIDE_COUNT;

// Fills a single cross can produce. Every fill exhausts at least one of the two sides' current
// quantity, and the external-leg continuation adds at most one more round per order, so
// MAX_INTERNAL_ORDERS per side plus headroom bounds it.
inline constexpr uint32_t MAX_FILLS_PER_FLUSH = 32;

// Legacy MAX_SELF_MATCH_THRESHOLD (Common.h): 50 non-AS self matches shut the process down.
inline constexpr uint32_t MAX_SELF_MATCH_THRESHOLD = 50;

// Legacy MAX_SELF_MATCH_THRESHOLD for the AS hedge/quote self-match counter.
inline constexpr uint32_t MAX_AS_SELF_MATCH_THRESHOLD = 50;

// Cache line, used for the queue's cursor padding and the request POD's static assertion.
inline constexpr uint32_t CACHE_LINE = 64;

}  // namespace tachyon::trade::exec
