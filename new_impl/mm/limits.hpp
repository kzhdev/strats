#pragma once

#include <cstdint>

#include <common/limits.hpp>

// Bounded sizes so that every hot array is a single static allocation and the per-cycle
// path performs no heap traffic (PLAN.md §3). Exceeding a cap is an init-time error, never
// a runtime allocation.
namespace tachyon::trade::mm {

// Contracts the engine can track (market-made contracts, including those currently
// disabled). The cap is process-wide (common/limits.hpp), not an mm choice: a contract
// index has to mean the same thing here, in the executor and in the ledger, and a local
// copy could drift from those without any build error.
using tachyon::trade::MAX_CONTRACTS;
using tachyon::trade::CONTRACT_WORDS;

// Depth of the market-data book. md::top_t::LEVEL_NUM == 10
// (rhonepublic/neorhone/common/MarketDataTypes.h:31), and SimpleMarketMaker reads every
// level in getPriceLevelIndex (:413), so the new book must hold all ten.
inline constexpr uint32_t BOOK_LEVELS = 10;

// Width percentages are configured per trading shift
// (MarketMakerParametersColumns::AsiaWidth..SafeModeWidth, Common.h:90-94).
inline constexpr uint32_t SHIFT_COUNT = 4;  // Asia, Europe, US, SafeMode

// Resting-order arena per contract, shared by both sides. The legacy implementation keeps
// these in two std::unordered_map + std::vector<OrderInfo*> containers that allocate on
// every order update (SimpleMarketMaker.h:1745-1746); the new one preallocates.
//
// Footprint of one order_queue: 512 slots x 40 B + 1024-entry id table x 4 B + 2 x 64 level
// records x 24 B ~= 27 KB. `cycle_state` will therefore hold the arena only for the contracts
// that are actually market-made (a handful in every config in config/), not for all
// MAX_CONTRACTS, so the state stays small and cache-friendly.
inline constexpr uint32_t MAX_ORDERS = 512;
inline constexpr uint32_t MAX_ORDERS_PER_LEVEL = 32;

// Distinct price levels that can hold resting orders, per contract per side. The spray
// logic adds orders at new prices (calculateQuotePriceAndSizeByOrders, :835-851), so this
// is larger than BOOK_LEVELS on purpose.
inline constexpr uint32_t MAX_LEVEL_SLOTS = 64;

// Fixed-size decision trace (PLAN.md §1 P4). Records are PODs; the cycle never formats.
inline constexpr uint32_t MAX_TRACE = 8192;

// Order requests a single contract may emit in one cycle. The legacy cycle emits at most four
// (two quotes, two hedges) plus the cross-check re-emissions and one spray, so this is ample;
// exceeding it is a cold path (the intent log reports overflow).
inline constexpr uint32_t MAX_INTENTS = 16;

// Upper bound on contracts changed between two cycles (the packed dirty list length).
inline constexpr uint32_t MAX_CHANGED = MAX_CONTRACTS;

}  // namespace tachyon::trade::mm
