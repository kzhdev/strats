#include "valuation.hpp"

#include <Eigen/Dense>

#include <cmath>

namespace tachyon::trade::rv {
namespace {

using MapV = Eigen::Map<Eigen::VectorXd>;
using MapCV = Eigen::Map<const Eigen::VectorXd>;
using MapM = Eigen::Map<const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>;

// The six pipeline stages. They are file-local so they inline into recompute() (a member with
// external linkage is kept out of line by GCC, which costs ~4% of the cycle in aggregate --
// measured with callgrind, PLAN §7.3). The stage split stays visible here, which is the point:
// one named kernel per legacy stage.

// stage 1: pricing contracts from midpoints (weighted least squares)
inline void pricing_contracts(const model& m, cycle_state& s) {
    const uint32_t np = m.n_pricing;
    const MapM odm(m.odm.data(), np, m.n_all);
    const MapCV mid(s.mid.data(), m.n_all);
    MapV pricing(s.pricing.data(), np);
    pricing.noalias() = odm * mid;
}

// stage 2: factor prices
inline void factor_prices(const model& m, cycle_state& s) {
    const uint32_t np = m.n_pricing;
    const uint32_t nv = m.n_vectors;
    const Eigen::Map<const Eigen::ArrayXd> mm(m.mult.data(), np);
    const Eigen::Map<const Eigen::ArrayXd> pricing(s.pricing.data(), np);
    Eigen::Map<Eigen::ArrayXd> scaled(s.scratch.data(), np);
    scaled = pricing * mm;  // element-wise: pricing multiplier applies per pricing contract
    const MapM p(m.pricing.data(), nv, np);
    MapV factor(s.factor.data(), nv);
    factor.noalias() = p * scaled.matrix();
}

// stage 3: long-term fair (EMA), including its isfinite guard
inline void long_term_fairs(const model& m, cycle_state& s, uint64_t now_cycles) {
    const uint64_t elapsed = now_cycles - s.last_ema_time;
    for (uint32_t v = 0; v < m.n_vectors; ++v) {
        if (m.use_ema[v]) {
            const uint64_t period = m.ema_period[v];
            if (period > 0) {
                const double alpha = static_cast<double>(elapsed) / static_cast<double>(period);
                const double mu = std::exp(-alpha);
                const double nu = (1.0 - mu) / alpha;
                const double last = s.ltf[v];
                const double next =
                    mu * last + (nu - mu) * s.prior_factor[v] + (1.0 - nu) * s.factor[v];
                if (std::isfinite(next)) {
                    s.ltf[v] = next;
                }
            } else if (std::isfinite(s.factor[v])) {
                s.ltf[v] = s.factor[v];
            }
        }
        s.prior_factor[v] = s.factor[v];
    }
    s.last_ema_time = now_cycles;
}

// stage 4: per-vector short-term fair
inline void vector_fairs(const model& m, cycle_state& s) {
    for (uint32_t v = 0; v < m.n_vectors; ++v) {
        s.stf_vec[v] = m.set_to_market[v] ? s.factor[v]
                                          : s.ltf[v] - s.pos_vec[v] * m.fare_move[v];
    }
}

// stage 5: pricing-contract short-term fair
inline void contract_fairs(const model& m, cycle_state& s) {
    const uint32_t np = m.n_pricing;
    const uint32_t nv = m.n_vectors;
    const MapM risk(m.risk.data(), np, nv);  // [contract][vector]
    const MapCV stf_vec(s.stf_vec.data(), nv);
    MapV out(s.pricing_stf.data(), np);
    out.noalias() = risk * stf_vec;
    for (uint32_t c = 0; c < np; ++c) {
        // Saturated: a config-scale ltf can exceed int64 at 1e9 (see PARITY_NOTES F6).
        s.stf[c] = to_rhone_price_saturated(s.pricing_stf[c] / m.mult[c]);
    }
}

// stage 6: spreads from legs (per-leg truncation, as legacy)
inline void spread_fairs(const model& m, cycle_state& s) {
    for (uint32_t c = m.first_traded; c < m.n_all; ++c) {
        if (m.leg_begin[c + 1] - m.leg_begin[c] <= 1) {
            continue;
        }
        // 128-bit accumulator: two saturated legs can exceed int64 when summed, and a leg
        // with ratio > 1 can overflow on its own.
        __int128 sum = 0;
        for (uint32_t k = m.leg_begin[c]; k < m.leg_begin[c + 1]; ++k) {
            sum += static_cast<__int128>(
                saturate_price(static_cast<double>(s.stf[m.leg_index[k]]) * m.leg_ratio[k]));
        }
        const __int128 limit = static_cast<__int128>(PRICE_SATURATION);
        if (sum > limit) {
            sum = limit;
        } else if (sum < -limit) {
            sum = -limit;
        }
        s.stf[c] = static_cast<price_t>(sum);
    }
}

}  // namespace

double valuation::mid(const cycle_state& s, uint32_t contract) const noexcept {
    const market_state& bid = s.market(contract, side::bid, true);  // legacy uses external markets
    const market_state& ask = s.market(contract, side::ask, true);
    if (m_.weighted_mid[contract]) {
        const qty_t est = static_cast<qty_t>(m_.est_level[contract]);
        const qty_t ask_size = ask.size < est ? ask.size : est;
        const qty_t bid_size = bid.size < est ? bid.size : est;
        const double denom = static_cast<double>(bid_size + ask_size);
        if (denom == 0.0) {
            return 0.0;
        }
        return (to_dollars(ask.price) * static_cast<double>(bid_size) +
                to_dollars(bid.price) * static_cast<double>(ask_size)) /
               denom;
    }
    return (to_dollars(bid.price) + to_dollars(ask.price)) / 2.0;
}

void valuation::refresh_dirty_mids(cycle_state& s) const noexcept {
    for (uint32_t i = 0; i < s.changed_count(); ++i) {
        const uint32_t c = s.changed_at(i);
        if (m_.used_in_pricing[c]) {
            s.mid[c] = mid(s, c);
        }
    }
}

void valuation::recompute(cycle_state& s, uint64_t now_cycles) const noexcept {
    pricing_contracts(m_, s);
    factor_prices(m_, s);
    long_term_fairs(m_, s, now_cycles);
    vector_fairs(m_, s);
    contract_fairs(m_, s);
    spread_fairs(m_, s);
}

}  // namespace tachyon::trade::rv
