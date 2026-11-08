#pragma once

// Standalone MarketMaker configuration: parses the MMConfigs section of a Viper model CSV
// plus the tick / lot / venue columns of a secdb, and derives the per-contract inputs the
// engine's immutable model is built from.
//
// Deliberately framework-free: no ViperConfig, no boost, no protobuf (PLAN.md §1, mirroring
// new_impl/rv). Column names are resolved through the file header, so a config whose column
// order differs still parses — the field semantics match the legacy positional reader
// (ViperConfig.h:872-913) exactly.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "model.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// Venue, as far as the engine cares (mirrors new_impl/rv's local mapping; the host adapter
// maps these onto hf::VENUE). SimpleMarketMaker only uses the venue for the per-exchange
// external-quoting gate and for the B3 quantity assert (:209, :1113).
enum class venue : uint8_t { cme = 0, ice, eurex, b3, other };

constexpr const char* to_string(venue v) noexcept {
    switch (v) {
        case venue::cme: return "CME";
        case venue::ice: return "ICE";
        case venue::eurex: return "EUREX";
        case venue::b3: return "B3";
        default: return "OTHER";
    }
}

// One MMConfigs row (legacy MarketMakerConfig, ViperConfig.h:170-196). Raw config values:
// the legacy invariants (quote size non-zero, warn-payup percentage positive, queue
// importance clamped) are enforced in model::build so they apply to synthetic inputs too.
struct mm_contract_cfg {
    std::string name;
    bool enabled = false;
    bool quote_external = false;
    uint32_t quote_size = 0;
    uint32_t quote_gap = 0;
    double pct_tob_max_quote = 0.0;
    double width_pct[SHIFT_COUNT] = {0.0, 0.0, 0.0, 0.0};  // Asia, Europe, US, SafeMode
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
    double est_level_ema = 0.0;        // EstLevelEMA: the initial EMA value
    uint32_t est_level_ema_seconds = 0;
    double est_level_pct_ema = 0.0;
    int32_t max_ticks_wide = -1;       // < 0 means "no bound" (legacy INT64_MAX)
    double worsen_payup_pct = 0.0;     // MMWorsenPayUpCondPct
    uint32_t queue_importance = 0;
    int32_t internal_position = 0;     // position carried in from a previous session
    int32_t external_position = 0;
};

struct mm_model_cfg {
    std::vector<mm_contract_cfg> contracts;

    // Throws std::runtime_error("<file>:<line>: <problem>") on malformed input or a
    // missing MMConfigs section.
    static mm_model_cfg load(const std::string& path);
};

// The three secdb columns MarketMaker needs. `tick` is in dollars, as written in the file.
// (The RV engine also reads `order_increment_qty` and the spread legs; MM needs no legs.)
struct secdb_row {
    double tick = 0.0;
    int64_t order_increment_qty = 1;
    venue venue_ = venue::other;
};

using secdb_table = std::unordered_map<std::string, secdb_row>;

// Header-driven reader. Handles RFC-4180 quoting, because secdb's `details` column is JSON
// full of commas (config/backtest/secdb.cme.csv:11 shows the shape) and a naive split would
// shift every column after it.
secdb_table load_secdb(const std::string& path);

// Resolves cfg + secdb into the engine's inputs: tick sizes converted at the legacy scale
// (to_mm_price), lot sizes, venues, and the trading shift that selects the active width.
//
// `tick_overrides` (dollars) lets a caller pin a tick for a symbol missing from secdb, the
// same escape hatch new_impl/rv uses. Throws when a contract has neither.
model_inputs build_model_inputs(const mm_model_cfg& cfg,
                                const secdb_table& secdb,
                                double cycles_per_second,
                                uint32_t current_shift,
                                const std::unordered_map<std::string, double>& tick_overrides = {});

}  // namespace tachyon::trade::mm
