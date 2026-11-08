#include "model.hpp"

#include <Eigen/Dense>

#include <string>

namespace tachyon::trade::rv {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("rv::build_model: " + what);
}

using MatrixXd = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

}  // namespace

model model::build(const model_inputs& in) {
    model m;
    m.n_all = in.n_contracts;
    m.n_pricing = in.n_pricing;
    m.n_vectors = in.n_vectors;
    m.first_traded = in.first_traded;

    // ---- validation (fail at init instead of silently trading nonsense) ----
    if (m.n_all == 0 || m.n_pricing == 0 || m.n_vectors == 0) {
        fail("empty model");
    }
    if (m.n_all > MAX_CONTRACTS || m.n_pricing > MAX_PRICING || m.n_vectors > MAX_VECTORS) {
        fail("config exceeds rv caps (contracts/pricing/vectors = " + std::to_string(m.n_all) +
             "/" + std::to_string(m.n_pricing) + "/" + std::to_string(m.n_vectors) + ")");
    }
    if (m.n_vectors != m.n_pricing) {  // D3: reproduce the square-only assumption
        fail("RV requires a square pricing matrix (vectors=" + std::to_string(m.n_vectors) +
             ", pricing=" + std::to_string(m.n_pricing) + ")");
    }
    if (m.first_traded > m.n_all) {
        fail("first_traded out of range");
    }
    if (in.contracts.size() != m.n_all || in.vectors.size() != m.n_vectors) {
        fail("contract/vector input size mismatch");
    }
    const size_t nn = static_cast<size_t>(m.n_all) * m.n_all;
    if (in.pricing_matrix.size() != static_cast<size_t>(m.n_vectors) * m.n_pricing ||
        in.hedge_ratio.size() != nn || in.quote_size.size() != nn || in.commissions.size() != nn ||
        in.leans.size() != nn || in.factor_hedged_index.size() != nn ||
        in.rhone_max_clip.size() != m.n_all) {
        fail("matrix input size mismatch");
    }

    // Layout of the merged contract list (PLAN §12): a contract may be priced and quoted at
    // the same time, so `traded` is a flag rather than implied by `index >= first_traded`.
    // What still has to hold is that everything the cycle values has a risk row: a pricing
    // column of its own, or legs that lead back to pricing columns.
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const contract_input& ci = in.contracts[c];
        const bool is_spread = ci.leg_count > 1;
        if (is_spread && c < m.first_traded) {
            fail("spread at contract " + std::to_string(c) +
                 " cannot be a PricingMatrix column (a spread has no risk row)");
        }
        if (is_spread) {
            for (uint32_t k = 0; k < ci.leg_count; ++k) {
                if (ci.leg_begin + k >= in.legs.size() || in.legs[ci.leg_begin + k].index >= m.n_pricing) {
                    fail("spread at contract " + std::to_string(c) +
                         " has a leg that is not a pricing contract");
                }
            }
        } else if (ci.traded && c >= m.first_traded) {
            fail("contract " + std::to_string(c) +
                 " is quoted but has no legs and is not a PricingMatrix column");
        }
    }

    const int n = static_cast<int>(m.n_pricing);
    const MatrixXd p = Eigen::Map<const MatrixXd>(in.pricing_matrix.data(), n, n);

    // risk = inverse(P) transposed, indexed [contract][vector] for the hot matvec.
    // V1: a singular pricing matrix is an init failure (ALGLIB used to return
    // garbage of magnitude ~1e15 for rank-deficient input).
    Eigen::FullPivLU<MatrixXd> lu(p);
    if (!lu.isInvertible()) {
        fail("pricing matrix is singular");
    }
    const MatrixXd inverse = lu.inverse();
    m.risk.resize(static_cast<size_t>(n) * n);
    for (int c = 0; c < n; ++c) {
        for (int v = 0; v < n; ++v) {
            // legacy _riskMatrix = pricingMatrix.inverse().transpose(), i.e.
            // risk[vector][contract] = inverse(contract, vector). Stored here as
            // risk[contract][vector] so the hot path is one row-major matvec.
            m.risk[static_cast<size_t>(c) * n + v] = inverse(c, v);
        }
    }
    // ODM = (A^T W^T W A)^-1 A^T W^T W   (weighted least squares operator)
    MatrixXd a = MatrixXd::Zero(m.n_all, m.n_pricing);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const contract_input& ci = in.contracts[c];
        if (ci.leg_count > 1) {  // spread: row is the leg ratios
            for (uint32_t k = 0; k < ci.leg_count; ++k) {
                const leg_input& leg = in.legs[ci.leg_begin + k];
                a(static_cast<int>(c), static_cast<int>(leg.index)) += leg.ratio;
            }
        } else {  // outright
            a(static_cast<int>(c), static_cast<int>(c)) = 1.0;
        }
    }
    MatrixXd w2 = MatrixXd::Zero(m.n_all, m.n_all);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        // legacy forms A^T W^T W A with W diagonal, i.e. the weight enters squared
        const double w = in.contracts[c].weight;
        w2(static_cast<int>(c), static_cast<int>(c)) = w * w;
    }
    const MatrixXd at = a.transpose();
    const MatrixXd normal = at * w2 * a;
    Eigen::FullPivLU<MatrixXd> normal_lu(normal);
    if (!normal_lu.isInvertible()) {
        fail("ODM normal matrix is singular");
    }
    const MatrixXd odm = normal_lu.inverse() * at * w2;
    m.odm.assign(odm.data(), odm.data() + odm.size());
    m.pricing = in.pricing_matrix;

    // ---- per contract tables ----
    m.width_extra.resize(m.n_all);
    m.threshold.resize(m.n_all);
    m.est_level.resize(m.n_all);
    m.max_ticks_through_market.resize(m.n_all);
    m.qmult.resize(m.n_all);
    m.pmult.resize(m.n_all);
    m.tick.resize(m.n_all);
    m.lot.resize(m.n_all);
    m.traded.resize(m.n_all);
    m.used_in_pricing.resize(m.n_all);
    m.quote_external.resize(m.n_all);
    m.weighted_mid.resize(m.n_all);
    m.static_quote.resize(m.n_all);
    m.static_quote_size.resize(m.n_all);
    m.venue.resize(m.n_all);
    m.rhone_max_clip = in.rhone_max_clip;
    m.mult.resize(m.n_pricing);
    m.leg_begin.assign(m.n_all + 1, 0);

    for (uint32_t c = 0; c < m.n_all; ++c) {
        const contract_input& ci = in.contracts[c];
        if (ci.tick_size <= 0) {
            fail("tick size must be positive (contract " + std::to_string(c) + ")");
        }
        if (ci.pricing_multiplier == 0.0) {
            fail("zero pricing multiplier (contract " + std::to_string(c) + ")");
        }
        if (ci.lot_size == 0) {
            fail("zero lot size (contract " + std::to_string(c) + ")");
        }
        m.width_extra[c] = ci.width + ci.extra_width;
        m.threshold[c] = ci.price_change_threshold;
        m.est_level[c] = ci.established_level;
        m.max_ticks_through_market[c] = ci.max_ticks_through_market;
        m.qmult[c] = ci.quantity_multiplier;
        m.pmult[c] = ci.pricing_multiplier;
        m.tick[c] = make_fast_div(ci.tick_size);
        m.lot[c] = ci.lot_size;
        m.traded[c] = ci.traded ? 1 : 0;
        m.used_in_pricing[c] = (ci.weight != 0.0) ? 1 : 0;
        m.quote_external[c] = ci.quote_external ? 1 : 0;
        m.weighted_mid[c] = ci.weighted_mid ? 1 : 0;
        m.static_quote[c] = ci.static_quote ? 1 : 0;
        m.static_quote_size[c] = ci.static_quote_size;
        m.venue[c] = ci.venue;
        if (c < m.n_pricing) {
            m.mult[c] = ci.pricing_multiplier;
        }
    }

    // ---- per (contract, lean) tables ----
    m.hedge_ratio = in.hedge_ratio;
    m.quote_size = in.quote_size;
    m.factor_hedged_index = in.factor_hedged_index;
    m.hedge_unity.resize(nn);
    m.commission_scaled.resize(nn);
    m.lean_begin.assign(m.n_all + 1, 0);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const double cpm = in.contracts[c].pricing_multiplier;
        const double cqm = in.contracts[c].quantity_multiplier;
        for (uint32_t j = 0; j < m.n_all; ++j) {
            const size_t k = static_cast<size_t>(c) * m.n_all + j;
            const double lpm = in.contracts[j].pricing_multiplier;
            const double lqm = in.contracts[j].quantity_multiplier;
            // The scaled form changes the floating-point association, and the oracle
            // measured a 0.5% tick mismatch rate for it even with power-of-ten
            // multipliers. So the precomputed path is used only when it is provably
            // identical to the legacy expression: all four multipliers exactly 1.0.
            m.hedge_unity[k] = (lpm == 1.0 && cpm == 1.0 && lqm == 1.0 && cqm == 1.0) ? 1 : 0;
            // legacy :2970 — commission divided by the contract multiplier; the
            // division moves from every cycle to init.
            m.commission_scaled[k] =
                static_cast<price_t>(static_cast<double>(in.commissions[k]) / cpm);
            // "canLeanOn && canQuote" are both static -> fold into the CSR list.
            if (in.leans[k] != 0 && in.hedge_ratio[k] >= 0.0) {
                m.lean_index.push_back(j);
            }
        }
        m.lean_begin[c + 1] = static_cast<uint32_t>(m.lean_index.size());
    }
    if (m.lean_index.size() > MAX_LEANS) {
        fail("lean count exceeds cap");
    }

    // ---- per vector ----
    m.fare_move.resize(m.n_vectors);
    m.init_ltf.resize(m.n_vectors);
    m.ema_period.resize(m.n_vectors);
    m.use_ema.resize(m.n_vectors);
    m.set_to_market.resize(m.n_vectors);
    m.factor_hedged.resize(m.n_vectors);
    for (uint32_t v = 0; v < m.n_vectors; ++v) {
        const vector_input& vi = in.vectors[v];
        m.fare_move[v] = vi.max_position != 0.0 ? vi.range / vi.max_position : 0.0;
        m.init_ltf[v] = vi.initial_ltf;
        m.ema_period[v] = vi.ema_period_cycles;
        m.use_ema[v] = vi.use_ema ? 1 : 0;
        m.set_to_market[v] = vi.set_to_market ? 1 : 0;
        m.factor_hedged[v] = vi.factor_hedged ? 1 : 0;
    }

    // ---- spread legs (CSR) ----
    m.leg_begin.assign(m.n_all + 1, 0);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const contract_input& ci = in.contracts[c];
        if (ci.leg_count > 1) {
            for (uint32_t k = 0; k < ci.leg_count; ++k) {
                const leg_input& leg = in.legs[ci.leg_begin + k];
                m.leg_index.push_back(leg.index);
                m.leg_ratio.push_back(leg.ratio);
            }
        }
        m.leg_begin[c + 1] = static_cast<uint32_t>(m.leg_index.size());
    }
    if (m.leg_index.size() > MAX_LEGS) {
        fail("leg count exceeds cap");
    }
    return m;
}

}  // namespace tachyon::trade::rv
