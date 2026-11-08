#pragma once

// ArbieSpreader configuration: the AS-relevant sections of a Viper model CSV (`ASConfigs`
// plus the shared `PricingMatrix` / `FactorConfigs` / `RVConfigs` / `PricingConfigs` /
// `GlobalConfigs`), the AS paths CSV and the secdb.
//
// The shared sections are read by rv::rv_model_cfg, which already parses them (and already
// skips ASConfigs as a block, config.cpp:268-271). This layer therefore adds only what AS
// needs on top, instead of duplicating a 400-line CSV reader: the cold config layer is the
// one place where a dependency on `new_impl/rv` is allowed (PLAN D7). The engine headers
// (model/state/paths/quotes/hedger/strategy/host) never include rv.
//
// Deliberately framework-free: no ViperConfig, no boost, no protobuf.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "rv/config.hpp"  // rv_model_cfg, secdb_table, shift_column (cold path only)
#include "types.hpp"

namespace tachyon::trade::as {

// One lean column of a path row: contract name (resolved to an id by build_model_inputs)
// and its multiplier.
struct as_lean_cfg {
    std::string contract;
    int32_t multiplier = 0;
};

// One path row of the paths CSV (`QV-UV,0,-1,1,...,0.001,TRUE`). Names are resolved later so
// that parsing stays independent of the contract id assignment.
struct as_path_cfg {
    std::string name;
    std::vector<as_lean_cfg> leans;  // non-zero multipliers, header column order
    double commission = 0.0;
    bool enabled = false;
};

// One row of `ASConfigs` plus its path block from the paths CSV.
struct as_contract_cfg {
    std::string name;
    bool enabled = false;
    bool quote_external = false;
    qty_t quote_size = 0;
    double edge = 0.0;
    double hedge_offset = 0.0;
    double max_ticks_through_market = 0.0;
    std::vector<as_path_cfg> paths;
};

struct as_model_cfg {
    // Shared sections + secdb, parsed by the rv layer.
    rv::rv_model_cfg rv;
    rv::secdb_table secdb;

    // AS-specific.
    std::vector<as_contract_cfg> as_contracts;
    double edge_multiplier = 1.0;  // GlobalConfigs `ASEdgeMultiplier`, shift-resolved

    // `SEdgeMultiplier` for a shift, defaulting to the last column then 1.0 (legacy
    // viperConfig.getGlobal(AS_EDGE_MULTIPLIER) is shift-resolved by FrontSide).
    double edge_multiplier_for(rv::shift_column shift) const;

    // Parses the model CSV, the AS paths CSV and the secdb. Throws
    // std::runtime_error("<file>:<line>: <problem>") on malformed input, a paths row that
    // names a contract the model does not have, or an ASConfigs row without a path block.
    static as_model_cfg load(const std::string& model_csv, const std::string& paths_csv,
                             const std::string& secdb_csv,
                             rv::shift_column shift = rv::shift_column::us);
};

// Derives the engine's `model_inputs`: contract order and legs from the rv layer (single
// source of truth for ticks/lots/venues/spread legs), then the AS per-contract numbers and
// the risk operator that feeds each path's short-term-fair move (PLAN §4.8 / D6).
model_inputs build_model_inputs(const as_model_cfg& cfg);

}  // namespace tachyon::trade::as
