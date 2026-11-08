#pragma once

// Reference oracle: a deliberately naive transcription of the legacy formulas
// (src/RelativeValue.h), written with plain loops and the legacy operation order.
// The differential test compares the optimised engine against this, so bugs in
// CSR leans / precomputed coefficients / magic divide are caught rather than
// blessed. Never optimise this file.

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "model.hpp"
#include "types.hpp"

namespace tachyon::trade::rv::oracle {

using row = std::vector<double>;

// Gauss-Jordan inverse with row swaps (independent of Eigen on purpose).
inline std::vector<row> inverse(std::vector<row> a) {
    const size_t n = a.size();
    std::vector<row> inv(n, row(n, 0.0));
    for (size_t i = 0; i < n; ++i) inv[i][i] = 1.0;
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        for (size_t r = col + 1; r < n; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
        if (a[pivot][col] == 0.0) throw std::runtime_error("oracle: singular");
        std::swap(a[col], a[pivot]);
        std::swap(inv[col], inv[pivot]);
        const double d = a[col][col];
        for (size_t c = 0; c < n; ++c) { a[col][c] /= d; inv[col][c] /= d; }
        for (size_t r = 0; r < n; ++r) {
            if (r == col || a[r][col] == 0.0) continue;
            const double f = a[r][col];
            for (size_t c = 0; c < n; ++c) { a[r][c] -= f * a[col][c]; inv[r][c] -= f * inv[col][c]; }
        }
    }
    return inv;
}

// Legacy RoundToTickSize (Common.h:1196), including the negative-remainder branch.
inline price_t round_to_tick_legacy(price_t price, price_t tick, side sd) noexcept {
    const price_t remainder = price % tick;
    if (!remainder) return price;
    const price_t truncated = price - remainder;
    if (sd == side::bid) return remainder > 0 ? truncated : truncated - tick;
    return remainder > 0 ? truncated + tick : truncated;
}

struct oracle_model {
    const model_inputs& in;
    std::vector<row> odm;      // [n_pricing][n_all]
    std::vector<row> risk;     // [n_vectors][n_pricing]  legacy [vector][contract]

    explicit oracle_model(const model_inputs& inputs) : in(inputs) {
        const uint32_t np = in.n_pricing, nv = in.n_vectors, na = in.n_contracts;
        std::vector<row> p(nv, row(np, 0.0));
        for (uint32_t r = 0; r < nv; ++r)
            for (uint32_t c = 0; c < np; ++c) p[r][c] = in.pricing_matrix[r * np + c];
        const std::vector<row> pinv = inverse(p);
        risk.assign(nv, row(np, 0.0));
        for (uint32_t r = 0; r < nv; ++r)
            for (uint32_t c = 0; c < np; ++c) risk[r][c] = pinv[c][r];

        std::vector<row> a(na, row(np, 0.0));
        for (uint32_t c = 0; c < na; ++c) {
            if (in.contracts[c].leg_count > 1) {
                for (uint32_t k = 0; k < in.contracts[c].leg_count; ++k) {
                    const leg_input& lg = in.legs[in.contracts[c].leg_begin + k];
                    a[c][lg.index] += lg.ratio;
                }
            } else {
                a[c][c] = 1.0;
            }
        }
        std::vector<row> normal(np, row(np, 0.0)), rhs(np, row(na, 0.0));
        for (uint32_t i = 0; i < np; ++i) {
            for (uint32_t j = 0; j < np; ++j) {
                double s = 0.0;
                // legacy: At * Wt * W * A  =>  the weight enters squared
                for (uint32_t c = 0; c < na; ++c) {
                    const double w2 = in.contracts[c].weight * in.contracts[c].weight;
                    s += a[c][i] * w2 * a[c][j];
                }
                normal[i][j] = s;
            }
            for (uint32_t c = 0; c < na; ++c) {
                const double w2 = in.contracts[c].weight * in.contracts[c].weight;
                rhs[i][c] = a[c][i] * w2;
            }
        }
        const std::vector<row> ninv = inverse(normal);
        odm.assign(np, row(na, 0.0));
        for (uint32_t i = 0; i < np; ++i)
            for (uint32_t c = 0; c < na; ++c) {
                double s = 0.0;
                for (uint32_t k = 0; k < np; ++k) s += ninv[i][k] * rhs[k][c];
                odm[i][c] = s;
            }
    }
};

// The legacy per-cycle fair-value chain, transcribed literally.
//   mids: [n_all] in dollars; positions: [n_vectors]
// Returns stf in 1e-8 ticks; also advances ltf/prior (caller owns EMA state).
inline void fairs(const oracle_model& om,
                  const std::vector<double>& mid,
                  const std::vector<double>& pos_vec,
                  std::vector<double>& ltf,
                  std::vector<double>& prior_factor,
                  uint64_t elapsed_cycles,
                  std::vector<price_t>& stf_out) {
    const model_inputs& in = om.in;
    const uint32_t np = in.n_pricing, nv = in.n_vectors, na = in.n_contracts;

    // pricing[p] = sum_c ODM[p][c] * mid[c]
    std::vector<double> pricing(np, 0.0);
    for (uint32_t p = 0; p < np; ++p) {
        double s = 0.0;
        for (uint32_t c = 0; c < na; ++c) s += om.odm[p][c] * mid[c];
        pricing[p] = s;
    }

    // factor[m] = sum_p P[m][p] * pricing[p] * mult[p]
    std::vector<double> factor(nv, 0.0);
    for (uint32_t m = 0; m < nv; ++m) {
        double s = 0.0;
        for (uint32_t p = 0; p < np; ++p) {
            s += in.pricing_matrix[m * np + p] * pricing[p] * in.contracts[p].pricing_multiplier;
        }
        factor[m] = s;
    }

    // LTF / EMA (legacy calculateLongTermFairsInVector)
    for (uint32_t m = 0; m < nv; ++m) {
        if (in.vectors[m].use_ema) {
            const uint64_t period = in.vectors[m].ema_period_cycles;
            if (period > 0) {
                const double alpha = static_cast<double>(elapsed_cycles) / static_cast<double>(period);
                const double mu = std::exp(-alpha);
                const double nu = (1.0 - mu) / alpha;
                const double next = mu * ltf[m] + (nu - mu) * prior_factor[m] + (1.0 - nu) * factor[m];
                if (std::isfinite(next)) ltf[m] = next;
            } else if (std::isfinite(factor[m])) {
                ltf[m] = factor[m];
            }
        }
        prior_factor[m] = factor[m];
    }

    // short-term fair per vector
    std::vector<double> stf_vec(nv, 0.0);
    for (uint32_t m = 0; m < nv; ++m) {
        stf_vec[m] = in.vectors[m].set_to_market
                         ? factor[m]
                         : ltf[m] - pos_vec[m] * (in.vectors[m].max_position != 0.0
                                                      ? in.vectors[m].range / in.vectors[m].max_position
                                                      : 0.0);
    }

    // pricing contracts: stf[c] = ToRhonePrice( sum_m stf_vec[m]*risk[m][c] / mult[c] )
    stf_out.assign(na, 0);
    for (uint32_t c = 0; c < np; ++c) {
        double sum = 0.0;
        for (uint32_t m = 0; m < nv; ++m) {
            sum += stf_vec[m] * om.risk[m][c] / in.contracts[c].pricing_multiplier;
        }
        stf_out[c] = to_rhone_price(sum);
    }
    // spreads: per-leg truncation then integer accumulate (legacy :2717-2735)
    for (uint32_t c = in.first_traded; c < na; ++c) {
        if (in.contracts[c].leg_count > 1) {
            price_t sum = 0;
            for (uint32_t k = 0; k < in.contracts[c].leg_count; ++k) {
                const leg_input& lg = in.legs[in.contracts[c].leg_begin + k];
                sum += static_cast<price_t>(static_cast<double>(stf_out[lg.index]) * lg.ratio);
            }
            stf_out[c] = sum;
        }
    }
}

// One lean, with the legacy operation order for the cost scaling (four divides,
// not the precomputed hedge_scale).
inline price_t lean_price_legacy(const oracle_model& om, const std::vector<price_t>& stf,
                                 const std::vector<price_t>& mkt_price,
                                 const std::vector<qty_t>& /*mkt_size*/,
                                 uint32_t contract, side sd, uint32_t lean, bool external) {
    const model_inputs& in = om.in;
    const size_t k = static_cast<size_t>(contract) * in.n_contracts + lean;
    const double ratio = in.hedge_ratio[k];
    const price_t lp = mkt_price[market_index(lean, sd, external)];
    if (!((lp != tachyon::NULL_PRICE && ratio != 0.0) || (ratio == 0.0 && external))) {
        return tachyon::NULL_PRICE;
    }
    const price_t cost = sd == side::bid ? stf[lean] - lp : lp - stf[lean];
    price_t hedging = static_cast<price_t>(static_cast<double>(cost) * ratio *
                                           in.contracts[lean].pricing_multiplier /
                                           in.contracts[contract].pricing_multiplier *
                                           in.contracts[lean].quantity_multiplier /
                                           in.contracts[contract].quantity_multiplier);
    const price_t width = in.contracts[contract].width + in.contracts[contract].extra_width;
    if (hedging < 0) {
        const price_t max_cost = static_cast<price_t>(0.95 * static_cast<double>(width));
        if (-hedging > max_cost) hedging = -max_cost;
    }
    const price_t commission =
        static_cast<price_t>(static_cast<double>(in.commissions[k]) /
                             in.contracts[contract].pricing_multiplier);
    const price_t fair = stf[contract];
    return sd == side::bid ? fair - width - hedging - commission
                           : fair + width + hedging + commission;
}

}  // namespace tachyon::trade::rv::oracle
