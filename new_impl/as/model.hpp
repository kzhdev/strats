#pragma once

// Immutable, cap-sized model of the ArbieSpreader engine: per-contract parameters, the path
// baskets, the short-term-fair offsets and every index the cycle needs to turn a market-data
// change into the paths it touches.
//
// Everything is a flat array indexed by contract id / path def / slot, so the cycle performs
// no hashing and no allocation (PLAN D3). Three inverse indices are built once here:
//
//   side_path  (contract, side) -> path slots of that contract/side   (the quoting scan)
//   affected   (contract, side) -> path slots leaning on that book    (the delta fan-out)
//   offset_path  offset        -> path slots sharing it               (the STF fan-out)
//
// `model::build` is the only place that knows how a path becomes an offset: it transcribes
// ArbieSpreader::initializeContracts (src/ArbieSpreader.cpp:30-232).

#include <cstdint>
#include <string>
#include <vector>

#include "limits.hpp"
#include "types.hpp"

namespace tachyon::trade::as {

class model {
public:
    // Freezes `in` into CSR tables. Throws std::runtime_error with an actionable message on a
    // cap violation, an unresolvable contract, a non-invertible/ill-shaped operator, or a
    // config the legacy engine would have dereferenced (PLAN §5.1).
    static model build(const model_inputs& in);

    // ---- shape -----------------------------------------------------------------
    uint32_t n_all = 0;
    uint32_t n_pricing = 0;
    uint32_t n_vectors = 0;
    uint32_t n_path_defs = 0;
    uint32_t n_path_slots = 0;
    uint32_t n_offsets = 0;

    // ---- per contract ----------------------------------------------------------
    std::vector<price_t> tick;                     // tick size, 1e-9 dollars
    std::vector<fast_div> tick_div;                // exact divider for `tick`
    std::vector<price_t> edge;                     // quote edge, already * ASEdgeMultiplier
    std::vector<price_t> max_ticks_through_market;  // quote validity bound (:61)
    std::vector<price_t> hedge_offset_price;       // hedge payup (:62)
    std::vector<price_t> ext_commission;           // external-book commission (:48)
    std::vector<qty_t> lot_size;
    std::vector<qty_t> quote_size;                 // ASConfigs quoteSize for quoting contracts
    std::vector<uint8_t> quoting;                  // has an ASConfigs row (:53)
    std::vector<uint8_t> enabled;                  // ASConfigs `enabled`
    std::vector<uint8_t> quote_external;            // ASConfigs `quoteExternal`
    std::vector<uint32_t> venue;
    std::vector<double> pricing_multiplier;
    std::vector<double> quantity_multiplier;

    // Spread legs (CSR): a contract with leg_count > 0 is a spread.
    std::vector<uint32_t> leg_begin;  // [n_all + 1]
    std::vector<uint32_t> leg_index;
    std::vector<int32_t> leg_ratio;

    // ---- per path definition ---------------------------------------------------
    std::vector<uint32_t> path_contract;    // the quoting contract that owns the path
    std::vector<price_t> path_commission;   // ArbieSpreader_Path.cpp:11
    std::vector<uint8_t> path_enabled;      // paths CSV `Enabled`
    std::vector<uint32_t> path_lean_begin;  // [n_path_defs + 1] into the lean arrays
    std::vector<uint32_t> lean_contract;
    std::vector<int32_t> lean_mult;
    std::vector<uint32_t> path_offset;      // offset index, or INVALID_INDEX for "no offset"
    std::vector<price_t> path_rv_stf_move;  // denormalised offset_rv_stf_move
    // The path's ordinal inside its contract's path list. Legacy `Path::_id` (assigned per config
    // block, ArbieSpreader.cpp:64-227) travels in the quote as `_hedgePath`, so telemetry and the
    // "a quote is outstanding" test need the same numbering.
    std::vector<uint32_t> path_local_id;

    // ---- per slot (one path definition on one side) ----------------------------
    std::vector<uint32_t> slot_path;  // slot -> path def
    std::vector<uint8_t> slot_side;
    std::vector<uint32_t> slot_contract;

    // ---- inverse index: (contract, side) -> slots of that contract's own paths --
    std::vector<uint32_t> side_path_begin;  // [n_all * 2 + 1]
    std::vector<uint32_t> side_path_slot;

    // ---- inverse index: (contract, side) -> slots leaning on that book ---------
    std::vector<uint32_t> affected_begin;  // [n_all * 2 + 1]
    std::vector<uint32_t> affected_slot;
    // The lean edge each affected slot came from, so the incremental update reads its
    // multiplier directly instead of scanning the path's lean list (legacy indexes a dense
    // `_multipliers[contractId]` array, ArbieSpreader.h:873).
    std::vector<uint32_t> affected_edge;

    // ---- short-term-fair offsets ----------------------------------------------
    std::vector<uint32_t> offset_term_begin;  // [n_offsets + 1]
    std::vector<uint32_t> offset_term_contract;
    std::vector<int32_t> offset_term_ratio;
    std::vector<std::string> offset_id;       // legacy id string (diagnostics + dedupe evidence)
    std::vector<price_t> offset_rv_stf_move;  // calculateRVSTFMove (:234-310)
    std::vector<uint32_t> offset_path_begin;  // [n_offsets + 1]
    std::vector<uint32_t> offset_path_slot;
    // Inverse index for an inbound short-term fair: contract -> the offsets that depend on it
    // (legacy handleSTFUpdate walks _mapShortTermFairOffset, ArbieSpreader_MessageHandler.cpp:153-188).
    std::vector<uint32_t> stf_offset_begin;  // [n_all + 1]
    std::vector<uint32_t> stf_offset_list;

    // ---- queries ---------------------------------------------------------------
    static constexpr uint32_t path_slot(uint32_t path, side s) noexcept {
        return path * SIDE_COUNT + to_index(s);
    }
    bool is_quoting(uint32_t contract) const noexcept { return quoting[contract] != 0; }
    bool is_enabled(uint32_t contract) const noexcept { return enabled[contract] != 0; }
    uint32_t leg_count(uint32_t contract) const noexcept {
        return leg_begin[contract + 1] - leg_begin[contract];
    }
    bool is_spread(uint32_t contract) const noexcept { return leg_count(contract) > 0; }
    uint32_t lean_count(uint32_t path) const noexcept {
        return path_lean_begin[path + 1] - path_lean_begin[path];
    }
    uint32_t path_count(uint32_t contract, side s) const noexcept {
        const uint32_t key = contract * SIDE_COUNT + to_index(s);
        return side_path_begin[key + 1] - side_path_begin[key];
    }
    uint32_t affected_count(uint32_t contract, side s) const noexcept {
        const uint32_t key = contract * SIDE_COUNT + to_index(s);
        return affected_begin[key + 1] - affected_begin[key];
    }
};

}  // namespace tachyon::trade::as
