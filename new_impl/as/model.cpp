#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace tachyon::trade::as {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("as::model::build: " + what);
}

// Returns an actionable message when a cap is exceeded, empty otherwise.
std::string capacity_error(uint32_t used, uint32_t cap, const char* what) {
    if (used <= cap) return {};
    return std::string("too many ") + what + " (" + std::to_string(used) + " > " +
           std::to_string(cap) + "), raise the cap in as/limits.hpp";
}

// Prefix-sum per-key counts into a CSR begin table.
void make_csr_begin(std::vector<uint32_t>& begin, const std::vector<uint32_t>& count) {
    begin.assign(count.size() + 1, 0);
    for (size_t k = 0; k < count.size(); ++k) begin[k + 1] = begin[k] + count[k];
}

// legacy tryReplaceSTFsWithSpreadSTF (ArbieSpreader.cpp:180-204): if every leg of `contract`
// is present with a consistent integer ratio, drop the legs and lean on the spread's own
// short-term fair instead. Integer division and the "add only if absent" rule are transcribed
// exactly, including the case where the computed ratio is 0.
void replace_with_spread(std::map<uint32_t, int32_t>& terms, uint32_t contract, const model& m) {
    const uint32_t begin = m.leg_begin[contract];
    const uint32_t count = m.leg_count(contract);
    if (count == 0) return;

    int32_t ratio = 0;
    for (uint32_t j = begin; j < begin + count; ++j) {
        const int32_t leg_ratio = m.leg_ratio[j];
        if (leg_ratio == 0) return;  // legacy would divide by zero here
        const auto it = terms.find(m.leg_index[j]);
        if (it == terms.end()) return;  // a leg is missing: not a switch path
        const int32_t candidate = it->second / leg_ratio;
        if (ratio == 0) {
            ratio = candidate;
        } else if (candidate != ratio) {
            return;  // inconsistent ratios: keep the legs
        }
    }
    for (uint32_t j = begin; j < begin + count; ++j) terms.erase(m.leg_index[j]);
    if (terms.find(contract) == terms.end()) terms.emplace(contract, ratio);
}

// legacy calculateRVSTFMove (ArbieSpreader.cpp:234-310): how much a path's fair value moves
// per unit of exposure, from the risk operator and the per-vector range / max position.
void compute_rv_stf_move(const model_inputs& in, model& m) {
    for (uint32_t v = 0; v < m.n_vectors; ++v) {
        if (in.max_position[v] == 0.0) {
            fail("vector " + std::to_string(v) +
                 " has maxPos 0; the fair-move formula divides by it");
        }
    }
    m.offset_rv_stf_move.assign(m.n_offsets, 0);
    std::vector<double> factor_net_pos(m.n_vectors, 0.0);
    std::vector<double> factor_move(m.n_vectors, 0.0);
    std::vector<double> contract_move(m.n_all, 0.0);

    for (uint32_t o = 0; o < m.n_offsets; ++o) {
        std::fill(factor_net_pos.begin(), factor_net_pos.end(), 0.0);
        std::fill(contract_move.begin(), contract_move.end(), 0.0);

        for (uint32_t k = m.offset_term_begin[o]; k < m.offset_term_begin[o + 1]; ++k) {
            const uint32_t c = m.offset_term_contract[k];
            const double ratio = static_cast<double>(m.offset_term_ratio[k]);
            const double qty_multiplier = m.quantity_multiplier[c];
            if (m.is_spread(c)) {
                for (uint32_t j = m.leg_begin[c]; j < m.leg_begin[c + 1]; ++j) {
                    const double r =
                        ratio * static_cast<double>(m.leg_ratio[j]) * qty_multiplier;
                    for (uint32_t v = 0; v < m.n_vectors; ++v) {
                        factor_net_pos[v] += r * in.risk[static_cast<size_t>(v) * m.n_pricing +
                                                         m.leg_index[j]];
                    }
                }
            } else {
                const double r = ratio * qty_multiplier;
                for (uint32_t v = 0; v < m.n_vectors; ++v) {
                    factor_net_pos[v] += r * in.risk[static_cast<size_t>(v) * m.n_pricing + c];
                }
            }
        }

        for (uint32_t v = 0; v < m.n_vectors; ++v) {
            factor_move[v] = -factor_net_pos[v] * in.range[v] / in.max_position[v];
        }

        // ascending contract order, legs before their spread (:278-286)
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (m.is_spread(c)) {
                double sum = 0.0;
                for (uint32_t j = m.leg_begin[c]; j < m.leg_begin[c + 1]; ++j) {
                    sum += contract_move[m.leg_index[j]] *
                           static_cast<double>(m.leg_ratio[j]);
                }
                contract_move[c] = sum;
            } else {
                double sum = 0.0;
                for (uint32_t v = 0; v < m.n_vectors; ++v) {
                    sum += in.risk[static_cast<size_t>(v) * m.n_pricing + c] * factor_move[v];
                }
                contract_move[c] = sum / m.pricing_multiplier[c];
            }
        }

        double stf_move = 0.0;
        for (uint32_t k = m.offset_term_begin[o]; k < m.offset_term_begin[o + 1]; ++k) {
            stf_move += static_cast<double>(m.offset_term_ratio[k]) *
                        contract_move[m.offset_term_contract[k]];
        }
        m.offset_rv_stf_move[o] = to_legacy_price(stf_move / 2.0);
    }
}

}  // namespace

model model::build(const model_inputs& in) {
    model m;

    // ---- shape -----------------------------------------------------------------
    if (in.n_contracts == 0) fail("no contracts");
    if (in.names.size() != in.n_contracts) fail("contract names do not match n_contracts");
    if (in.n_vectors != in.n_pricing) {
        fail("square model required (vectors=" + std::to_string(in.n_vectors) +
             ", pricing=" + std::to_string(in.n_pricing) +
             "); the risk operator inverts the pricing matrix");
    }
    if (const std::string e = capacity_error(in.n_contracts, MAX_CONTRACTS, "contracts");
        !e.empty()) {
        fail(e);
    }
    m.n_all = in.n_contracts;
    m.n_pricing = in.n_pricing;
    m.n_vectors = in.n_vectors;

    // ---- per-contract tables ---------------------------------------------------
    m.tick = in.tick_size;
    m.edge.assign(m.n_all, 0);
    m.max_ticks_through_market.assign(m.n_all, 0);
    m.hedge_offset_price.assign(m.n_all, 0);
    m.ext_commission = in.ext_commission;
    m.lot_size = in.lot_size;
    m.quote_size.assign(m.n_all, 0);
    m.quoting.assign(m.n_all, 0);
    m.enabled.assign(m.n_all, 0);
    m.quote_external.assign(m.n_all, 0);
    m.venue = in.venue;
    m.pricing_multiplier = in.pricing_multiplier;
    m.quantity_multiplier = in.quantity_multiplier;
    m.leg_begin = in.leg_begin;
    m.leg_index = in.leg_index;
    m.leg_ratio = in.leg_ratio;

    m.tick_div.resize(m.n_all);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        if (m.tick[c] <= 0) {
            fail("contract '" + in.names[c] + "' has tick size " + std::to_string(m.tick[c]));
        }
        m.tick_div[c] = make_fast_div(m.tick[c]);
    }

    // A spread's legs must precede it: legacy aggregates a spread's fair move from its legs'
    // already-computed values in ascending contract order (ArbieSpreader.cpp:278-286), so an
    // inverted order would silently read a zero.
    for (uint32_t c = 0; c < m.n_all; ++c) {
        for (uint32_t k = m.leg_begin[c]; k < m.leg_begin[c + 1]; ++k) {
            if (m.leg_index[k] >= c) {
                fail("contract '" + in.names[c] + "' has leg '" + in.names[m.leg_index[k]] +
                     "' that does not precede it; contract ids must put outrights first");
            }
            if (m.leg_ratio[k] == 0) {
                fail("contract '" + in.names[c] + "' has a zero leg ratio");
            }
        }
    }

    std::map<std::string, uint32_t> by_name;
    for (uint32_t c = 0; c < m.n_all; ++c) {
        if (!by_name.emplace(in.names[c], c).second) {
            fail("duplicate contract name '" + in.names[c] + "'");
        }
    }

    // ---- path definitions ------------------------------------------------------
    m.path_lean_begin.assign(1, 0);
    std::vector<uint32_t> defs_per_contract(m.n_all, 0);
    for (const as_contract_input& ac : in.contracts) {
        const auto it = by_name.find(ac.name);
        if (it == by_name.end()) fail("ASConfigs contract '" + ac.name + "' is not in the model");
        const uint32_t id = it->second;
        if (m.quoting[id] != 0) fail("duplicate ASConfigs row for '" + ac.name + "'");

        m.quoting[id] = 1;
        m.enabled[id] = ac.enabled ? 1 : 0;
        m.quote_external[id] = ac.quote_external ? 1 : 0;
        m.quote_size[id] = ac.quote_size;
        m.edge[id] = ac.edge;
        m.max_ticks_through_market[id] = ac.max_ticks_through_market;
        m.hedge_offset_price[id] = ac.hedge_offset_price;
        if (ac.quote_size < 0) fail("'" + ac.name + "' has a negative quote size");
        if (ac.enabled && ac.paths.empty()) fail("'" + ac.name + "' is enabled but has no paths");

        defs_per_contract[id] = static_cast<uint32_t>(ac.paths.size());
        if (const std::string e =
                capacity_error(defs_per_contract[id], MAX_PATHS_PER_CONTRACT, "paths per contract");
            !e.empty()) {
            fail("'" + ac.name + "': " + e);
        }

        uint32_t local_id = 0;
        for (const path_input& p : ac.paths) {
            if (const std::string e = capacity_error(m.n_path_defs + 1, MAX_PATH_DEFS, "paths");
                !e.empty()) {
                fail(e);
            }
            // legacy `Path::_id`: the ordinal inside this contract's path block
            m.path_local_id.push_back(local_id++);
            m.n_path_defs++;
            m.path_contract.push_back(id);
            m.path_commission.push_back(p.commission);
            m.path_enabled.push_back(p.enabled ? 1 : 0);
            for (const lean_input& lean : p.leans) {
                if (lean.contract >= m.n_all) {
                    fail("path '" + p.name + "' of '" + ac.name +
                         "' leans on an unknown contract");
                }
                if (lean.multiplier == 0) {
                    fail("path '" + p.name + "' of '" + ac.name + "' has a zero lean multiplier");
                }
                if (const std::string e =
                        capacity_error(static_cast<uint32_t>(m.lean_contract.size()) + 1,
                                       MAX_LEAN_EDGES, "lean edges");
                    !e.empty()) {
                    fail(e);
                }
                m.lean_contract.push_back(lean.contract);
                m.lean_mult.push_back(lean.multiplier);
            }
            m.path_lean_begin.push_back(static_cast<uint32_t>(m.lean_contract.size()));
        }
    }

    m.n_path_slots = m.n_path_defs * SIDE_COUNT;
    if (const std::string e = capacity_error(m.n_path_slots, MAX_PATH_SLOTS, "path slots");
        !e.empty()) {
        fail(e);
    }

    // ---- slots -----------------------------------------------------------------
    m.slot_path.resize(m.n_path_slots);
    m.slot_side.resize(m.n_path_slots);
    m.slot_contract.resize(m.n_path_slots);
    for (uint32_t d = 0; d < m.n_path_defs; ++d) {
        for (uint32_t s = 0; s < SIDE_COUNT; ++s) {
            const uint32_t slot = path_slot(d, static_cast<side>(s));
            m.slot_path[slot] = d;
            m.slot_side[slot] = static_cast<uint8_t>(s);
            m.slot_contract[slot] = m.path_contract[d];
        }
    }

    // ---- inverse index: (contract, side) -> its own path slots ------------------
    {
        std::vector<uint32_t> count(static_cast<size_t>(m.n_all) * SIDE_COUNT, 0);
        for (uint32_t slot = 0; slot < m.n_path_slots; ++slot) {
            ++count[m.slot_contract[slot] * SIDE_COUNT + m.slot_side[slot]];
        }
        make_csr_begin(m.side_path_begin, count);
        std::vector<uint32_t> cursor(m.side_path_begin.begin(), m.side_path_begin.end() - 1);
        m.side_path_slot.resize(m.n_path_slots);
        for (uint32_t slot = 0; slot < m.n_path_slots; ++slot) {
            const uint32_t key = m.slot_contract[slot] * SIDE_COUNT + m.slot_side[slot];
            m.side_path_slot[cursor[key]++] = slot;
        }
    }

    // ---- inverse index: (lean contract, approximate side) -> path slots ----------
    // Legacy Contract::_affectedPaths[side] holds the path *instances* whose lean side is
    // `side` (ArbieSpreader.cpp:155-165): a path leaning with a positive multiplier is
    // affected on its own side, a negative multiplier flips it (getLeanMarkets, :542-549).
    {
        std::vector<uint32_t> count(static_cast<size_t>(m.n_all) * SIDE_COUNT, 0);
        for (uint32_t d = 0; d < m.n_path_defs; ++d) {
            for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
                for (uint32_t s = 0; s < SIDE_COUNT; ++s) {
                    const side path_side = static_cast<side>(s);
                    const side lean_side = m.lean_mult[k] > 0 ? path_side : other(path_side);
                    ++count[m.lean_contract[k] * SIDE_COUNT + to_index(lean_side)];
                }
            }
        }
        make_csr_begin(m.affected_begin, count);
        const uint32_t total = m.affected_begin.back();
        if (const std::string e = capacity_error(total, MAX_AFFECTED_EDGES, "affected edges");
            !e.empty()) {
            fail(e);
        }
        std::vector<uint32_t> cursor(m.affected_begin.begin(), m.affected_begin.end() - 1);
        m.affected_slot.resize(total);
        m.affected_edge.resize(total);
        for (uint32_t d = 0; d < m.n_path_defs; ++d) {
            for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
                for (uint32_t s = 0; s < SIDE_COUNT; ++s) {
                    const side path_side = static_cast<side>(s);
                    const side lean_side = m.lean_mult[k] > 0 ? path_side : other(path_side);
                    const uint32_t key = m.lean_contract[k] * SIDE_COUNT + to_index(lean_side);
                    const uint32_t at = cursor[key]++;
                    m.affected_slot[at] = path_slot(d, path_side);
                    m.affected_edge[at] = k;
                }
            }
        }
    }

    // ---- short-term-fair offsets ----------------------------------------------
    // Transcribed from initializeContracts (:69-225):
    //   * leans contribute -multiplier of their fair value (spread legs expanded),
    //   * the quoted contract contributes +1 (or its legs' ratios),
    //   * a spread whose legs are all present with a consistent ratio collapses to the spread's
    //     own fair value -- that is what makes a path a "switch path".
    m.offset_term_begin.assign(1, 0);
    std::vector<std::vector<int32_t>> offset_key;  // pre-replacement identity (legacy stfId)
    for (uint32_t d = 0; d < m.n_path_defs; ++d) {
        std::map<uint32_t, int32_t> terms;
        auto add = [&terms](uint32_t contract, int32_t ratio) {
            const auto it = terms.find(contract);
            if (it == terms.end()) {
                if (ratio != 0) terms.emplace(contract, ratio);
                return;
            }
            it->second += ratio;
            if (it->second == 0) terms.erase(it);
        };
        for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
            const uint32_t lean = m.lean_contract[k];
            const int32_t mult = m.lean_mult[k];
            if (!m.is_spread(lean)) {
                add(lean, -mult);
            } else {
                for (uint32_t j = m.leg_begin[lean]; j < m.leg_begin[lean + 1]; ++j) {
                    add(m.leg_index[j], -mult * m.leg_ratio[j]);
                }
            }
        }
        const uint32_t quoted = m.path_contract[d];
        if (!m.is_spread(quoted)) {
            add(quoted, 1);
        } else {
            for (uint32_t j = m.leg_begin[quoted]; j < m.leg_begin[quoted + 1]; ++j) {
                add(m.leg_index[j], m.leg_ratio[j]);
            }
        }

        // legacy stfId: "<ratio>*<contractId>" concatenated in ascending contract order
        std::string id;
        std::vector<int32_t> key;
        key.reserve(terms.size() * 2);
        for (const auto& entry : terms) {
            id.append(std::to_string(entry.second)).append("*").append(
                std::to_string(entry.first));
            key.push_back(static_cast<int32_t>(entry.first));
            key.push_back(entry.second);
        }

        uint32_t offset = INVALID_INDEX;
        if (!key.empty()) {  // legacy keeps the shared _zeroStfOffset for an empty term map
            for (uint32_t o = 0; o < m.n_offsets; ++o) {
                if (offset_key[o] == key) {
                    offset = o;
                    break;
                }
            }
        }
        if (offset == INVALID_INDEX && !key.empty()) {
            if (const std::string e = capacity_error(m.n_offsets + 1, MAX_OFFSETS, "stf offsets");
                !e.empty()) {
                fail(e);
            }
            offset = m.n_offsets++;
            offset_key.push_back(key);
            m.offset_id.push_back(id);

            replace_with_spread(terms, quoted, m);
            for (uint32_t c = 0; c < m.n_all; ++c) {
                if (c != quoted) replace_with_spread(terms, c, m);
            }
            for (const auto& entry : terms) {
                const uint32_t term_contract = entry.first;
                if (const std::string e =
                        capacity_error(static_cast<uint32_t>(m.offset_term_contract.size()) + 1,
                                       MAX_STF_TERMS, "stf terms");
                    !e.empty()) {
                    fail(e);
                }
                if (!m.is_spread(term_contract) && term_contract >= m.n_pricing) {
                    fail("offset term '" + in.names[term_contract] +
                         "' is not a pricing contract, but legacy reads the risk matrix by id");
                }
                m.offset_term_contract.push_back(term_contract);
                m.offset_term_ratio.push_back(entry.second);
            }
            m.offset_term_begin.push_back(static_cast<uint32_t>(m.offset_term_contract.size()));
        }
        m.path_offset.push_back(offset);
    }

    compute_rv_stf_move(in, m);

    // ---- per-path fair move + offset -> path slots ------------------------------
    m.path_rv_stf_move.resize(m.n_path_defs);
    std::vector<uint32_t> offset_path_count(m.n_offsets, 0);
    for (uint32_t d = 0; d < m.n_path_defs; ++d) {
        const uint32_t o = m.path_offset[d];
        m.path_rv_stf_move[d] = o == INVALID_INDEX ? 0 : m.offset_rv_stf_move[o];
        // each definition contributes both of its side slots
        if (o != INVALID_INDEX) offset_path_count[o] += SIDE_COUNT;
    }
    make_csr_begin(m.offset_path_begin, offset_path_count);
    std::vector<uint32_t> cursor(m.offset_path_begin.begin(), m.offset_path_begin.end() - 1);
    m.offset_path_slot.resize(m.offset_path_begin.back());
    for (uint32_t d = 0; d < m.n_path_defs; ++d) {
        const uint32_t o = m.path_offset[d];
        if (o == INVALID_INDEX) continue;
        for (uint32_t s = 0; s < SIDE_COUNT; ++s) {
            m.offset_path_slot[cursor[o]++] = path_slot(d, static_cast<side>(s));
        }
    }

    // ---- inverse index: short-term-fair contract -> the offsets that depend on it ----
    {
        std::vector<uint32_t> count(m.n_all, 0);
        for (uint32_t o = 0; o < m.n_offsets; ++o) {
            for (uint32_t k = m.offset_term_begin[o]; k < m.offset_term_begin[o + 1]; ++k) {
                ++count[m.offset_term_contract[k]];
            }
        }
        make_csr_begin(m.stf_offset_begin, count);
        std::vector<uint32_t> list_cursor(m.stf_offset_begin.begin(),
                                         m.stf_offset_begin.end() - 1);
        m.stf_offset_list.resize(m.stf_offset_begin.back());
        for (uint32_t o = 0; o < m.n_offsets; ++o) {
            for (uint32_t k = m.offset_term_begin[o]; k < m.offset_term_begin[o + 1]; ++k) {
                m.stf_offset_list[list_cursor[m.offset_term_contract[k]]++] = o;
            }
        }
    }

    return m;
}

}  // namespace tachyon::trade::as
