#pragma once

// Minimal RV configuration: parses the RV-relevant sections of a Viper model CSV
// (PricingMatrix / FactorConfigs / RVConfigs / PricingConfigs / LeanMatrix /
// GlobalConfigs) plus a secdb tick table, and derives the full `model_inputs` —
// including the init-time tables the legacy strategy computed in
// calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes (RelativeValue.h:2239-2459).
//
// Deliberately framework-free: no ViperConfig, no boost, no protobuf.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "model.hpp"
#include "types.hpp"

namespace tachyon::trade::rv {

struct rv_contract_cfg {  // one RVConfigs row
    std::string name;
    bool quote_external = false;
    double hedge_tolerance = 0.0;
    double commission = 0.0;
    double width = 0.0;
    double pct_stf_move = 0.0;
    int64_t max_ticks_through_market = 0;
    bool static_quote = false;
    uint32_t quote_size = 0;
};

struct rv_vector_cfg {  // one FactorConfigs row
    std::string name;
    double range = 0.0;
    double max_pos = 0.0;
    double ltf = 0.0;
    bool ema = false;
    uint32_t ema_length_secs = 0;
    int32_t ema_on = -1;   // HHMM, -1 = none
    int32_t ema_off = -1;
    bool set_to_market = false;
    uint32_t chart_decimals = 0;
    bool hedged = false;
};

struct rv_pricing_cfg {  // one PricingConfigs row
    std::string name;
    double weight = 0.0;
    bool weighted_mid = false;
    int64_t established_level = 0;
    double pricing_multiplier = 1.0;
    double quantity_multiplier = 1.0;
    int32_t sweep_ticks = 0;
    int64_t sweep_qty = 0;
    int64_t max_ticks_wide = -1;
    double as_ext_commission = 0.0;
};

enum class shift_column : uint32_t { asia = 0, europe = 1, us = 2, safe_mode = 3 };

struct rv_model_cfg {
    std::vector<std::string> pricing_contracts;          // PricingMatrix header
    std::vector<std::vector<double>> pricing_matrix;     // [vector][pricing]
    std::vector<rv_vector_cfg> vectors;                  // FactorConfigs
    std::vector<rv_contract_cfg> rv_contracts;           // RVConfigs (traded contracts)
    std::vector<rv_pricing_cfg> pricing_configs;         // PricingConfigs (all contracts)
    std::vector<std::pair<std::string, std::vector<int32_t>>> leans;  // LeanMatrix rows
    
    
    

    // RVWidthMultiplier for a shift (GlobalConfigs row, default = US column).
    double width_multiplier(shift_column shift = shift_column::us) const;

    // Throws std::runtime_error("<file>:<line>: <problem>") on malformed input.
    static rv_model_cfg load(const std::string& path);
};

// secdb: symbol -> tick size in dollars (the `tick` column).
std::unordered_map<std::string, double> load_secdb_ticks(const std::string& path);

// One leg of a spread, as secdb's `details` JSON describes it.
struct secdb_leg {
    std::string sym;
    int64_t qty = 0;  // signed: +1 buys the leg, -1 sells it (legacy ContractLeg)
};

// Richer secdb view: the RV model needs tick size, the lot used by toValidQty
// (`order_increment_qty`) and the venue (`activate`) for per-venue quoting gates.
// The venue codes are a local mapping; the host adapter maps them to hf::VENUE.
// JPX instruments are described by the separate jpx_* columns (see the note in
// PLAN §12 about tick sizes for JPX rows).
struct secdb_row {
    double tick = 0.0;
    int64_t order_increment_qty = 1;
    uint32_t venue = 0;  // 0=CME, 1=ICE, 2=EUREX, 3=JPX, 4=other
    // Authoritative spread legs from the `details` column, e.g.
    //   {"legs": [{"sym": "BRN FMQ0022!", "gid": 1557628, "qty": 1}, ...]}
    // Empty for outrights (secdb writes `{}`) and for secdb files without the column.
    // These are the only source that survives a name that cannot be split (ICE's
    // "BRN FMQ0022!" legs, EUREX's "efFGBMU1:Z1") or a leg ratio other than +/-1.
    std::vector<secdb_leg> legs;
};
using secdb_table = std::unordered_map<std::string, secdb_row>;
secdb_table load_secdb(const std::string& path);

// Derives everything the engine needs: ordering, legs, tick sizes, widths, thresholds,
// lean CSR inputs, and the per-pair hedge ratios / commissions / quote sizes.
// `tick_overrides` lets a caller pin tick sizes for symbols missing from secdb (spreads).
model_inputs build_model_inputs(
    const rv_model_cfg& cfg,
    const secdb_table& secdb,
    double width_multiplier,
    double cycles_per_second = 1.0,
    const std::unordered_map<std::string, double>& tick_overrides = {});

}  // namespace tachyon::trade::rv
