// Derives model_inputs from a parsed RV model config. This is the port of
// RelativeValue::calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes
// (src/RelativeValue.h:2239-2459) plus the per-contract setup from
// initializeContracts (:2224) and initializeFromRelativeValue (:2172).

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "config.hpp"

namespace tachyon::trade::rv {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("build_model_inputs: " + what);
}

inline bool is_zero(double v) noexcept { return std::fabs(v) < 1e-12; }

// legacy Round<N>
template <int Decimals>
inline double round_n(double v) noexcept {
    const double scale = std::pow(10.0, Decimals);
    return std::round(v * scale) / scale;
}

}  // namespace

model_inputs build_model_inputs(const rv_model_cfg& cfg,
                                const secdb_table& secdb,
                                double width_multiplier,
                                double cycles_per_second,
                                const std::unordered_map<std::string, double>& tick_overrides) {
    model_inputs in;
    const uint32_t n_pricing = static_cast<uint32_t>(cfg.pricing_contracts.size());
    const uint32_t n_vectors = static_cast<uint32_t>(cfg.vectors.size());
    if (n_vectors != n_pricing) {
        fail("square model required (vectors=" + std::to_string(n_vectors) +
             ", pricing=" + std::to_string(n_pricing) + ")");
    }

    // ---- contract order: PricingMatrix columns first, then RV-only contracts ----
    // The layout has to satisfy two constraints at once:
    //   * every PricingMatrix column keeps its index [0, n_pricing), because the ODM
    //     operator, the pricing matrix and the risk matrix are all keyed by it;
    //   * a contract listed in *both* matrices (config/2msgw lists GEH1/ZTH1 in
    //     PricingMatrix and RVConfigs) is one contract that is priced *and* quoted, the
    //     way legacy merges them -- not two contracts sharing a name.
    // So a traded contract may live below first_traded; anything that means "traded" must
    // test `traded[]`, not `index >= first_traded`.
    std::vector<std::string> names;
    std::vector<uint8_t> traded;
    std::unordered_map<std::string, uint32_t> index_of;
    names.reserve(n_pricing + cfg.rv_contracts.size());
    traded.reserve(n_pricing + cfg.rv_contracts.size());
    for (const auto& name : cfg.pricing_contracts) {
        if (!index_of.emplace(name, static_cast<uint32_t>(names.size())).second) {
            fail("duplicate pricing contract '" + name + "' in the PricingMatrix header");
        }
        names.push_back(name);
        traded.push_back(0);
    }
    for (const auto& rc : cfg.rv_contracts) {
        auto it = index_of.find(rc.name);
        if (it == index_of.end()) {
            it = index_of.emplace(rc.name, static_cast<uint32_t>(names.size())).first;
            names.push_back(rc.name);
            traded.push_back(0);
        }
        if (traded[it->second] != 0) {
            fail("duplicate RVConfigs row '" + rc.name + "'");
        }
        traded[it->second] = 1;
    }

    const uint32_t n_all = static_cast<uint32_t>(names.size());
    in.n_contracts = n_all;
    in.n_pricing = n_pricing;
    in.n_vectors = n_vectors;
    in.first_traded = n_pricing;  // first index that cannot be a pricing column
    in.names = names;             // id order, for consumers that key by contract id

    std::unordered_map<std::string, const rv_pricing_cfg*> pricing_cfg;
    for (const auto& p : cfg.pricing_configs) pricing_cfg[p.name] = &p;

    in.pricing_matrix.resize(static_cast<size_t>(n_vectors) * n_pricing);
    for (uint32_t v = 0; v < n_vectors; ++v) {
        for (uint32_t c = 0; c < n_pricing; ++c) {
            in.pricing_matrix[static_cast<size_t>(v) * n_pricing + c] = cfg.pricing_matrix[v][c];
        }
    }

    // ---- name resolution ----
    // secdb names carry a trailing `!` (`BRN FMQ0022!`) that spread names and the lean
    // matrix drop, so resolve with and without it.
    auto lookup = [&](const std::string& name) -> uint32_t {
        if (const auto it = index_of.find(name); it != index_of.end()) return it->second;
        std::string alt = name;
        if (!alt.empty() && alt.back() == '!') {
            alt.pop_back();
        } else {
            alt.push_back('!');
        }
        if (const auto it = index_of.find(alt); it != index_of.end()) return it->second;
        return UINT32_MAX;
    };

    // Legs implied by the spread name: `A-B` (ICE/CME) or `A:B` (EUREX), alternating
    // +1 / -1. EUREX names the legs after the first one by month/year only
    // (`efFGBMU1:Z1` = efFGBMU1 vs efFGBM + `Z1`), so a `:` leg that does not resolve on
    // its own gets the first leg's product root prepended.
    auto legs_from_name = [&](const std::string& name) {
        std::vector<leg_input> legs;
        std::vector<std::string> tokens;
        std::vector<char> separator;  // separator *before* the token, '\0' for the first
        std::string current;
        char previous = '\0';
        for (const char ch : name) {
            if (ch == '-' || ch == ':') {
                tokens.push_back(current);
                separator.push_back(previous);
                current.clear();
                previous = ch;
            } else {
                current.push_back(ch);
            }
        }
        tokens.push_back(current);
        separator.push_back(previous);
        if (tokens.size() < 2) return legs;  // not a spread

        double ratio = 1.0;
        for (size_t k = 0; k < tokens.size(); ++k) {
            if (tokens[k].empty()) {
                fail("empty leg in contract name '" + name + "'");
            }
            uint32_t index = lookup(tokens[k]);
            if (index == UINT32_MAX && separator[k] == ':' && !legs.empty()) {
                const std::string& root_source = names[legs.front().index];
                if (root_source.size() > 2) {
                    index = lookup(root_source.substr(0, root_source.size() - 2) + tokens[k]);
                }
            }
            if (index == UINT32_MAX) {
                fail("unknown leg '" + tokens[k] + "' in contract '" + name + "'");
            }
            legs.push_back({index, ratio});
            ratio = -ratio;
        }
        return legs;
    };


    // ---- legs ----
    // secdb's `details` column is authoritative (PLAN §12); the name splitter is the
    // fallback for configs whose secdb has no details. Where both are available they must
    // agree, otherwise the model contradicts the exchange description and we stop here
    // instead of trading one of the two.
    std::vector<uint32_t> leg_begin(n_all + 1, 0);
    for (uint32_t c = 0; c < n_all; ++c) {
        // Only traded contracts carry legs: a pricing column is an outright by
        // construction, since the pricing matrix is defined over them.
        if (traded[c] != 0) {
            std::vector<leg_input> legs;
            const auto secdb_it = secdb.find(names[c]);
            const bool have_details = secdb_it != secdb.end() && !secdb_it->second.legs.empty();
            if (have_details) {
                for (const secdb_leg& leg : secdb_it->second.legs) {
                    const uint32_t index = lookup(leg.sym);
                    if (index == UINT32_MAX) {
                        fail("secdb details of '" + names[c] + "' name the leg '" + leg.sym +
                             "', which is not a contract in the model");
                    }
                    legs.push_back({index, static_cast<double>(leg.qty)});
                }
                const std::vector<leg_input> from_name = legs_from_name(names[c]);
                if (from_name.size() == legs.size()) {
                    bool agree = true;
                    for (size_t k = 0; k < legs.size(); ++k) {
                        const bool unit = from_name[k].ratio == 1.0 || from_name[k].ratio == -1.0;
                        if (!unit || from_name[k].index != legs[k].index ||
                            (from_name[k].ratio > 0) != (legs[k].ratio > 0)) {
                            agree = false;
                        }
                    }
                    if (!agree) {
                        fail("secdb details of '" + names[c] +
                             "' disagree with the contract name (details legs vs name legs)");
                    }
                }
            } else {
                legs = legs_from_name(names[c]);
            }
            for (const leg_input& leg : legs) {
                in.legs.push_back(leg);
            }
        }
        leg_begin[c + 1] = static_cast<uint32_t>(in.legs.size());
    }

    // ---- per-contract configuration ----
    in.contracts.resize(n_all);
    std::vector<double> commission_input(n_all, 0.0);
    std::vector<double> pct_stf_move(n_all, 0.0);
    for (uint32_t c = 0; c < n_all; ++c) {
        contract_input& ci = in.contracts[c];
        const rv_pricing_cfg* pc = nullptr;
        if (const auto it = pricing_cfg.find(names[c]); it != pricing_cfg.end()) pc = it->second;

        ci.traded = traded[c] != 0;
        ci.leg_begin = leg_begin[c];
        ci.leg_count = leg_begin[c + 1] - leg_begin[c];

        // tick size: secdb, then explicit overrides, then the first leg's tick
        double tick = 0.0;
        uint32_t venue = 0;
        int64_t lot = 1;
        if (const auto t = secdb.find(names[c]); t != secdb.end()) {
            tick = t->second.tick;
            venue = t->second.venue;
            lot = t->second.order_increment_qty;
        } else if (const auto t = tick_overrides.find(names[c]); t != tick_overrides.end()) {
            tick = t->second;
        } else if (ci.leg_count > 0) {  // spreads are not in secdb: inherit from the first leg
            const std::string& leg_name = names[in.legs[ci.leg_begin].index];
            if (const auto t = secdb.find(leg_name); t != secdb.end()) {
                tick = t->second.tick;
                venue = t->second.venue;
                lot = t->second.order_increment_qty;
            }
        }
        if (tick <= 0.0) {
            fail("no tick size for '" + names[c] + "' (pass tick_overrides)");
        }
        ci.tick_size = static_cast<price_t>(std::llround(tick * static_cast<double>(PRICE_SCALE)));
        ci.venue = venue;
        ci.lot_size = static_cast<qty_t>(lot);

        if (pc != nullptr) {
            ci.weight = pc->weight;
            ci.weighted_mid = pc->weighted_mid;
            ci.established_level = pc->established_level;
            ci.pricing_multiplier = pc->pricing_multiplier;
            ci.quantity_multiplier = pc->quantity_multiplier;
        }
        // legacy _priceChangeThreshold = 0.05 * tickSize (:2232)
        ci.price_change_threshold = static_cast<price_t>(0.05 * static_cast<double>(ci.tick_size));
    }

    // ---- traded contracts: RVConfigs columns (:2189-2196) ----
    for (const rv_contract_cfg& rc : cfg.rv_contracts) {
        const uint32_t c = index_of.find(rc.name)->second;
        contract_input& ci = in.contracts[c];
        ci.quote_external = rc.quote_external;
        ci.static_quote = rc.static_quote;
        ci.static_quote_size = rc.quote_size;
        // legacy _maxTicksThroughMarket = rvConfig._maxTicksThroughMarket * tickSize (:2193).
        // Zero means "a quote may not sit at or through the opposite side at all" — legacy then
        // rejects every quote and disables, which is reproduced rather than special-cased.
        ci.max_ticks_through_market = static_cast<price_t>(
            static_cast<double>(rc.max_ticks_through_market) * static_cast<double>(ci.tick_size));
        // width = RVWidth * widthMultiplier; keep the configured part and the extra part
        const price_t width = to_rhone_price(rc.width);
        ci.width = width;
        ci.extra_width = static_cast<price_t>(static_cast<double>(width) * (width_multiplier - 1.0));
        commission_input[c] = rc.commission;
        pct_stf_move[c] = rc.pct_stf_move;
    }

    // ---- vectors ----
    in.vectors.resize(n_vectors);
    std::vector<double> fare_move(n_vectors, 0.0);
    for (uint32_t v = 0; v < n_vectors; ++v) {
        const rv_vector_cfg& vc = cfg.vectors[v];
        vector_input& vi = in.vectors[v];
        vi.range = vc.range;
        vi.max_position = vc.max_pos;
        vi.initial_ltf = vc.ltf;
        vi.use_ema = vc.ema;
        vi.set_to_market = vc.set_to_market;
        vi.factor_hedged = vc.hedged;
        // engine EMA period is in cycles; the host supplies cycles/sec (legacy:
        // GetCyclesFromSeconds(vc._EMALength, _frequency))
        vi.ema_period_cycles = static_cast<uint64_t>(static_cast<double>(vc.ema_length_secs) *
                                                    cycles_per_second);
        fare_move[v] = vc.max_pos != 0.0 ? vc.range / vc.max_pos : 0.0;
    }
    // ---- risk matrix, legacy orientation risk[vector][contract] ----
    Eigen::MatrixXd p(n_vectors, n_pricing);
    for (uint32_t v = 0; v < n_vectors; ++v) {
        for (uint32_t c = 0; c < n_pricing; ++c) {
            p(static_cast<int>(v), static_cast<int>(c)) =
                in.pricing_matrix[static_cast<size_t>(v) * n_pricing + c];
        }
    }
    Eigen::FullPivLU<Eigen::MatrixXd> lu(p);
    if (!lu.isInvertible()) {
        fail("pricing matrix is singular");
    }
    const Eigen::MatrixXd inv = lu.inverse();
    std::vector<double> risk(static_cast<size_t>(n_vectors) * n_pricing);
    for (uint32_t v = 0; v < n_vectors; ++v) {
        for (uint32_t c = 0; c < n_pricing; ++c) {
            risk[static_cast<size_t>(v) * n_pricing + c] =
                inv(static_cast<int>(c), static_cast<int>(v));
        }
    }
    // vector risk exposure of a contract (calculateVectorRiskExposureForContract)
    auto exposure = [&](uint32_t contract, uint32_t v) {
        const contract_input& ci = in.contracts[contract];
        double e = 0.0;
        if (ci.leg_count > 1) {
            for (uint32_t k = 0; k < ci.leg_count; ++k) {
                const leg_input& leg = in.legs[ci.leg_begin + k];
                e += risk[static_cast<size_t>(v) * n_pricing + leg.index] * leg.ratio *
                     ci.quantity_multiplier;
            }
        } else {
            // Only a pricing column has a risk row of its own: a traded contract that is
            // neither priced nor a spread cannot be valued at all (PLAN §4.8).
            if (contract >= n_pricing) {
                fail("contract '" + names[contract] +
                     "' is quoted but neither priced (PricingMatrix) nor a spread with legs");
            }
            e = risk[static_cast<size_t>(v) * n_pricing + contract] * ci.quantity_multiplier;
        }
        return e;
    };
    auto ratio_toward = [&](uint32_t contract, uint32_t outright) {
        const contract_input& ci = in.contracts[contract];
        if (ci.leg_count > 1) {
            for (uint32_t k = 0; k < ci.leg_count; ++k) {
                if (in.legs[ci.leg_begin + k].index == outright) {
                    return in.legs[ci.leg_begin + k].ratio;
                }
            }
            return 0.0;
        }
        return contract == outright ? 1.0 : 0.0;
    };

    const size_t nn = static_cast<size_t>(n_all) * n_all;
    in.hedge_ratio.assign(nn, 0.0);
    in.quote_size.assign(nn, 0.0);
    in.commissions.assign(nn, 0);
    in.factor_hedged_index.assign(nn, -1);
    in.leans.assign(nn, 0);
    in.rhone_max_clip.assign(n_all, 0);  // runtime input (SetRhoneLimits)

    // ---- lean matrix (names -> internal ids) ----
    for (size_t row = 0; row < cfg.leans.size(); ++row) {
        const auto row_it = index_of.find(cfg.leans[row].first);
        if (row_it == index_of.end()) continue;
        const size_t cols = cfg.leans[row].second.size();
        for (size_t col = 0; col < cols && col < cfg.leans.size(); ++col) {
            if (cfg.leans[row].second[col] == 0) continue;
            const auto col_it = index_of.find(cfg.leans[col].first);
            if (col_it == index_of.end()) continue;
            in.leans[static_cast<size_t>(row_it->second) * n_all + col_it->second] = 1;
        }
    }

    // ---- per-pair derivation: hedge ratio, commission, quote size (:2239-2459) ----
    std::vector<double> exp1(n_vectors, 0.0), exp2(n_vectors, 0.0), possible(n_vectors, 0.0);
    std::vector<double> vector_risk(static_cast<size_t>(n_vectors) * n_vectors, 0.0);
    std::vector<double> sums(n_vectors, 0.0);
    std::vector<double> one_lot(n_vectors, 0.0);
    std::vector<double> fair_move_for_contract(n_pricing, 0.0);

    // Pairs of *traded* contracts, in contract order. A traded contract may be a pricing
    // column (priced and quoted, e.g. 2msgw) or RV-only (index >= first_traded).
    std::vector<uint32_t> traded_ids;
    traded_ids.reserve(cfg.rv_contracts.size());
    for (uint32_t c = 0; c < n_all; ++c) {
        if (traded[c] != 0) traded_ids.push_back(c);
    }

    for (const uint32_t c : traded_ids) {
        for (const uint32_t j : traded_ids) {
            for (uint32_t v = 0; v < n_vectors; ++v) {
                exp1[v] = exposure(c, v);
                exp2[v] = exposure(j, v);
            }
            double hedge = 0.0;
            int32_t vector_index = -1;
            if (c != j) {
                for (uint32_t v = 0; v < n_vectors; ++v) {
                    possible[v] = is_zero(exp2[v]) ? 0.0 : exp1[v] / exp2[v];
                }
                for (uint32_t vr = 0; vr < n_vectors; ++vr) {
                    for (uint32_t vc = 0; vc < n_vectors; ++vc) {
                        vector_risk[static_cast<size_t>(vr) * n_vectors + vc] =
                            exp1[vr] - exp2[vr] * possible[vc];
                    }
                }
                for (uint32_t col = 0; col < n_vectors; ++col) {  // multiplyAbsColumn
                    double sum = 0.0;
                    for (uint32_t r = 0; r < n_vectors; ++r) {
                        sum += fare_move[r] *
                               std::fabs(vector_risk[static_cast<size_t>(r) * n_vectors + col]);
                    }
                    sums[col] = sum;
                }
                // pickBestHedge (:2322-2350)
                std::vector<uint32_t> ties;
                double min = sums[0];
                for (uint32_t v = 0; v < n_vectors; ++v) {
                    if (std::fabs(sums[v] - min) < 0.000001) {
                        ties.push_back(v);
                    } else if (sums[v] < min) {
                        ties.clear();
                        ties.push_back(v);
                        min = sums[v];
                    }
                }
                uint32_t index = ties[0];
                if (ties.size() > 1) {
                    double move = fare_move[index];
                    for (const uint32_t i : ties) {
                        if (fare_move[i] > move) {
                            move = fare_move[i];
                            index = i;
                        }
                    }
                }
                vector_index = in.vectors[index].factor_hedged ? static_cast<int32_t>(index) : -1;
                hedge = round_n<4>(possible[index]);
            }
            const size_t k = static_cast<size_t>(c) * n_all + j;
            in.hedge_ratio[k] = hedge;
            in.factor_hedged_index[k] = vector_index;

            // normalisation (:2363-2407), then commission (:2352-2359)
            double normalizer = std::numeric_limits<double>::max();
            for (uint32_t o = 0; o < n_pricing; ++o) {
                const double total = ratio_toward(c, o) - ratio_toward(j, o) * hedge;
                if (!is_zero(total)) {
                    normalizer = std::min(normalizer, std::fabs(total));
                }
            }
            const double commission =
                (commission_input[c] * in.contracts[c].pricing_multiplier +
                 std::fabs(hedge) * commission_input[j] * in.contracts[j].pricing_multiplier) /
                normalizer;
            in.commissions[k] = to_rhone_price(commission);

            // quote size (:2409-2459)
            for (uint32_t v = 0; v < n_vectors; ++v) {
                one_lot[v] = (exp1[v] - exp2[v] * hedge) * fare_move[v];
            }
            for (uint32_t o = 0; o < n_pricing; ++o) {
                double move = 0.0;
                for (uint32_t v = 0; v < n_vectors; ++v) {
                    move += one_lot[v] * risk[static_cast<size_t>(v) * n_pricing + o] /
                            in.contracts[o].pricing_multiplier;
                }
                fair_move_for_contract[o] = move;
            }
            double min_size = std::numeric_limits<double>::max();
            for (const uint32_t s : traded_ids) {
                const contract_input& ci = in.contracts[s];
                double move = 0.0;
                if (ci.leg_count > 0) {
                    for (uint32_t t = 0; t < ci.leg_count; ++t) {
                        const leg_input& leg = in.legs[ci.leg_begin + t];
                        move += fair_move_for_contract[leg.index] * leg.ratio;
                    }
                } else {
                    move = fair_move_for_contract[s];
                }
                // legacy divides by the configured width, not width+extra (:2447)
                const double move_pct = std::fabs(move) / to_dollars(ci.width);
                min_size = std::min(min_size, pct_stf_move[s] / move_pct);
            }
            if (!std::isfinite(min_size) || min_size < 0 ||
                min_size == std::numeric_limits<double>::max()) {
                min_size = 0.0;
            }
            in.quote_size[k] = min_size;
        }
    }

    return in;
}

}  // namespace tachyon::trade::rv
