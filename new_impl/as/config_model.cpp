// Derives the AS engine's `model_inputs`.
//
// Contract order, tick sizes, lots, venues and spread legs come from the rv layer
// (rv::build_model_inputs) so there is exactly one implementation of the secdb / leg /
// name-splitting rules. On top of that this file computes what legacy
// ArbieSpreader::initializeContracts (:30-232) and calculateRVSTFMove (:234-310) derived.
//
// This is the only translation unit in the AS engine that uses Eigen, and it runs once at
// init (PLAN D6): the pricing-matrix inverse is consumed immediately into per-path numbers,
// so the cycle never touches a matrix.

#include <Eigen/Dense>

#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "config.hpp"

namespace tachyon::trade::as {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("as::build_model_inputs: " + what);
}

// legacy canEnable (ArbieSpreader.h:445-447): 1.0 <= ASEdgeMultiplier <= 10.0. Legacy checks
// it at enable time; failing at init is strictly better (a config that can never be enabled
// should not start).
void validate_edge_multiplier(double value) {
    if (!(value >= 1.0 && value <= 10.0)) {
        fail("ASEdgeMultiplier " + std::to_string(value) + " is outside [1,10]");
    }
}

// Legacy legs are integers (`ContractLeg::_ratio` is int32); rv carries them as double
// because other strategies allow fractional ratios.
int32_t leg_ratio_of(const rv::leg_input& leg, const std::string& name) {
    const double rounded = std::nearbyint(leg.ratio);
    if (rounded != leg.ratio || std::fabs(rounded) > 10000.0) {
        fail("contract '" + name + "' has a non-integral leg ratio " +
             std::to_string(leg.ratio));
    }
    return static_cast<int32_t>(rounded);
}

}  // namespace

model_inputs build_model_inputs(const as_model_cfg& cfg) {
    // ---- contract order / ticks / lots / venues / legs: the rv layer decides ----
    const rv::model_inputs base = rv::build_model_inputs(cfg.rv, cfg.secdb, 1.0);

    model_inputs in;
    in.n_contracts = base.n_contracts;
    in.n_pricing = base.n_pricing;
    in.n_vectors = base.n_vectors;
    in.names = base.names;
    if (in.names.size() != in.n_contracts) {
        fail("contract names are not reported by the rv layer");
    }
    const uint32_t n = in.n_contracts;

    in.tick_size.resize(n);
    in.ext_commission.assign(n, 0);
    in.lot_size.resize(n);
    in.venue.resize(n);
    in.pricing_multiplier.assign(n, 1.0);
    in.quantity_multiplier.assign(n, 1.0);
    in.leg_begin.assign(n + 1, 0);

    for (uint32_t c = 0; c < n; ++c) {
        const rv::contract_input& ci = base.contracts[c];
        in.tick_size[c] = ci.tick_size;
        in.lot_size[c] = static_cast<qty_t>(ci.lot_size);
        in.venue[c] = ci.venue;
        in.pricing_multiplier[c] = ci.pricing_multiplier;
        in.quantity_multiplier[c] = ci.quantity_multiplier;
        in.leg_begin[c + 1] = in.leg_begin[c] + ci.leg_count;
    }
    in.leg_index.reserve(base.legs.size());
    for (const rv::leg_input& leg : base.legs) in.leg_index.push_back(leg.index);
    in.leg_ratio.assign(base.legs.size(), 0);
    for (uint32_t c = 0; c < n; ++c) {
        const rv::contract_input& ci = base.contracts[c];
        for (uint32_t k = ci.leg_begin; k < ci.leg_begin + ci.leg_count; ++k) {
            in.leg_ratio[k] = leg_ratio_of(base.legs[k], in.names[c]);
        }
    }

    // ---- per-contract pricing config (AS reads asExtCommission, ArbieSpreader.cpp:48) ----
    std::unordered_map<std::string, const rv::rv_pricing_cfg*> pricing_cfg;
    for (const auto& p : cfg.rv.pricing_configs) pricing_cfg[p.name] = &p;
    for (uint32_t c = 0; c < n; ++c) {
        const auto it = pricing_cfg.find(in.names[c]);
        if (it == pricing_cfg.end()) {
            fail("contract '" + in.names[c] + "' has no PricingConfigs row");
        }
        in.ext_commission[c] = to_legacy_price(it->second->as_ext_commission);
    }

    // ---- vectors + operators ----
    in.pricing_matrix = base.pricing_matrix;
    in.range.resize(in.n_vectors);
    in.max_position.resize(in.n_vectors);
    for (uint32_t v = 0; v < in.n_vectors; ++v) {
        in.range[v] = cfg.rv.vectors[v].range;
        in.max_position[v] = cfg.rv.vectors[v].max_pos;
    }

    // risk = (P^-1)^T, i.e. legacy `_riskMatrix = pricingMatrix.inverse().transpose()`
    // (ArbieSpreader.cpp:21), stored row-major [vector][pricing column].
    {
        using row_major = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
        const Eigen::Map<const row_major> p(in.pricing_matrix.data(), in.n_vectors, in.n_pricing);
        Eigen::FullPivLU<Eigen::MatrixXd> lu(p);
        if (!lu.isInvertible()) {
            fail("pricing matrix is singular");
        }
        const Eigen::MatrixXd inv = lu.inverse();  // [pricing][vector], column-major
        in.risk.assign(static_cast<size_t>(in.n_vectors) * in.n_pricing, 0.0);
        for (uint32_t v = 0; v < in.n_vectors; ++v) {
            for (uint32_t pcol = 0; pcol < in.n_pricing; ++pcol) {
                in.risk[static_cast<size_t>(v) * in.n_pricing + pcol] = inv(pcol, v);
            }
        }
    }

    // ---- the AS quoting contracts ----
    std::unordered_map<std::string, uint32_t> index_of;
    for (uint32_t c = 0; c < n; ++c) index_of[in.names[c]] = c;

    // secdb names carry a trailing `!` that model names drop, so resolve both ways
    // (same rule as rv/config_model.cpp:99-109).
    auto lookup = [&index_of](const std::string& name) -> uint32_t {
        if (const auto it = index_of.find(name); it != index_of.end()) return it->second;
        std::string alt = name;
        if (!alt.empty() && alt.back() == '!') {
            alt.pop_back();
        } else {
            alt.push_back('!');
        }
        if (const auto it = index_of.find(alt); it != index_of.end()) return it->second;
        return INVALID_INDEX;
    };

    validate_edge_multiplier(cfg.edge_multiplier);

    for (const as_contract_cfg& c : cfg.as_contracts) {
        const uint32_t id = lookup(c.name);
        if (id == INVALID_INDEX) {
            fail("ASConfigs contract '" + c.name + "' is not in the model");
        }
        if (in.tick_size[id] <= 0) {
            fail("ASConfigs contract '" + c.name + "' has no tick size");
        }
        if (c.enabled && c.paths.empty()) {
            fail("ASConfigs contract '" + c.name +
                 "' is enabled but has no paths (legacy would quote from a null path)");
        }

        const double tick_dollars = to_dollars(in.tick_size[id]);
        as_contract_input out;
        out.name = c.name;
        out.enabled = c.enabled;
        out.quote_external = c.quote_external;
        out.quote_size = c.quote_size;
        // :58 edge = ToRhonePrice(config._edge * _edgeMultiplier)
        out.edge = to_legacy_price(c.edge * cfg.edge_multiplier);
        // :61/:62 multiply the config double by the tick size *in price units*, so the
        // result is the dollar product (the tick contributes its dollar value once).
        // Pinned by as_tests::test_legacy_quirks.
        out.max_ticks_through_market =
            to_legacy_price(c.max_ticks_through_market * tick_dollars);
        out.hedge_offset_price = to_legacy_price(c.hedge_offset * tick_dollars);

        for (const as_path_cfg& p : c.paths) {
            path_input pi;
            pi.name = p.name;
            pi.commission = to_legacy_price(p.commission);  // ArbieSpreader_Path.cpp:11
            pi.enabled = p.enabled;
            for (const as_lean_cfg& lean : p.leans) {
                const uint32_t lean_id = lookup(lean.contract);
                if (lean_id == INVALID_INDEX) {
                    fail("path '" + p.name + "' of '" + c.name + "' leans on '" +
                         lean.contract + "', which is not in the model");
                }
                pi.leans.push_back({lean_id, lean.multiplier});
            }
            out.paths.push_back(std::move(pi));
        }
        in.contracts.push_back(std::move(out));
    }

    return in;
}

}  // namespace tachyon::trade::as
