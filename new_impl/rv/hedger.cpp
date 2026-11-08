#include "hedger.hpp"

#include <algorithm>
#include <cmath>

namespace tachyon::trade::rv {
namespace {

inline bool is_zero(double v) noexcept { return std::fabs(v) < 1e-12; }

// legacy Round<2>
inline double round2(double v) noexcept { return std::round(v * 100.0) / 100.0; }

inline bool better_or_equal(price_t a, price_t b, side sd) noexcept {
    return sd == side::bid ? a >= b : a <= b;
}

}  // namespace

hedger::hedger(const model& m, const quote_policy& policy, const order_port& port) noexcept
    : m_(m), policy_(policy), port_(port) {
    request_index_.fill(0);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        max_clip_[c] = m.rhone_max_clip[c];
    }
}

void hedger::on_fill(uint32_t contract, side sd, qty_t qty) noexcept {
    // legacy handleFill: accumulate the signed fill into the hedge request. The position moves
    // when the hedge is sent (add_position), not here.
    append_request(contract,
                   sd == side::bid ? static_cast<double>(qty) : -static_cast<double>(qty));
}

void hedger::append_request(uint32_t contract, double size) noexcept {
    const uint32_t slot = request_index_[contract];
    if (slot != 0) {
        requests_[slot - 1].size += size;
        return;
    }
    requests_[request_count_].contract = contract;
    requests_[request_count_].size = size;
    request_index_[contract] = ++request_count_;
}

void hedger::erase_request(uint32_t index) noexcept {
    request_index_[requests_[index].contract] = 0;
    for (uint32_t i = index; i + 1 < request_count_; ++i) {
        requests_[i] = requests_[i + 1];
        request_index_[requests_[i].contract] = i + 1;
    }
    --request_count_;
}

qty_t hedger::adjusted_market_size(const cycle_state& s, uint32_t contract, side sd,
                                   bool external) const noexcept {
    const market_state& mkt = s.market(contract, sd, external);
    if (policy_.adjusted_size != nullptr) {
        return policy_.adjusted_size(policy_.adjusted_ctx, contract, sd, external, mkt.size);
    }
    return mkt.size;
}

double hedger::residual(const cycle_state& s, uint32_t contract, side sd) const noexcept {
    // legacy getResiduals (:578-607)
    const quote& q = s.quote_of(contract, sd);
    if (q.price == tachyon::NULL_PRICE) {
        return 0.0;
    }
    const uint32_t lean = q.lean;
    const int32_t vector_signed = m_.factor_hedged_index[m_.pair(contract, lean)];
    if (vector_signed == -1) {
        return 0.0;
    }
    const uint32_t v = static_cast<uint32_t>(vector_signed);
    if (!m_.factor_hedged[v]) {
        return 0.0;
    }
    double vector_risk = 0.0;
    if (m_.is_spread(contract)) {
        for (uint32_t k = m_.leg_begin[contract]; k < m_.leg_begin[contract + 1]; ++k) {
            vector_risk += m_.risk[static_cast<size_t>(m_.leg_index[k]) * m_.n_vectors + v] *
                           m_.leg_ratio[k];
        }
    } else {
        vector_risk = m_.risk[static_cast<size_t>(contract) * m_.n_vectors + v];
    }
    vector_risk = round2(vector_risk);
    if (is_zero(vector_risk)) {
        return 0.0;
    }
    return s.pos_vec[v] / vector_risk / m_.qmult[contract];
}

qty_t hedger::order_size(uint32_t lean, double hedge_ratio, qty_t min_available, double& need,
                         double size, double residuals, double quote_size) const noexcept {
    // legacy getOrderSize (:2577-2600)
    if (is_zero(hedge_ratio)) {
        need -= quote_size;  // "no quote path"
        return 0;
    }
    double max_hedge = (size + residuals) * hedge_ratio;
    double available = static_cast<double>(min_available);
    if (available > max_hedge) {
        available = max_hedge;
    }
    const double need_in_hedge_terms = need * hedge_ratio;
    qty_t qty = 0;
    if (need_in_hedge_terms >= available) {
        qty = to_valid_qty(static_cast<qty_t>(std::round(available)), m_.lot[lean]);
    } else {
        qty = to_valid_qty(static_cast<qty_t>(std::round(need_in_hedge_terms)), m_.lot[lean]);
    }
    need = (need_in_hedge_terms - static_cast<double>(qty)) / hedge_ratio;
    return qty;
}

double hedger::send_hedges(cycle_state& s, uint32_t contract, double size, side sd,
                           bool& remove_request, double residual_abs) noexcept {
    // legacy sendHedges (:2466-2545)
    double need = size;
    price_t rounded_price = tachyon::NULL_PRICE;
    bool sent = false;

    for (uint32_t k = m_.lean_begin[contract]; k < m_.lean_begin[contract + 1]; ++k) {
        const uint32_t lean = m_.lean_index[k];
        const bool external = s.best_is_external(lean, sd);
        const price_t lg_price = lean_calculator::price(m_, s, contract, sd, lean, external);
        if (lg_price == tachyon::NULL_PRICE) {
            continue;
        }
        const size_t idx = m_.pair(contract, lean);
        const double hedge_ratio = m_.hedge_ratio[idx];
        const price_t price = round_to_tick(lg_price, m_.tick[lean], sd);
        if (rounded_price == tachyon::NULL_PRICE) {
            rounded_price = price;
        }
        if (price != rounded_price) {  // left the club (S7)
            if (!sent) {
                remove_request = true;
            }
            break;
        }
        const market_state& hedge_market = s.market(lean, sd, external);
        const qty_t lean_qty =
            lean_calculator::size(m_, s, contract, sd, lean, external, hedge_market.size, policy_);
        const qty_t qty = order_size(lean, hedge_ratio, adjusted_market_size(s, lean, sd, external),
                                     need, static_cast<double>(lean_qty), residual_abs,
                                     m_.quote_size[idx]);
        if (qty > 0) {
            const side other_sd = other(sd);
            const price_t lean_quote_price = s.quote_of(lean, sd).price;
            const price_t hedge_price = hedge_market.price;
            // S14: never cross our own quote, never trade 2+ ticks worse
            const bool crossing = lean_quote_price != tachyon::NULL_PRICE &&
                                  better_or_equal(hedge_price, lean_quote_price, other_sd);
            bool ticks_worse = false;
            if (lean_quote_price != tachyon::NULL_PRICE &&
                !better_or_equal(hedge_price, lean_quote_price, sd)) {
                const price_t delta = std::abs(hedge_price - lean_quote_price);
                // legacy IsTicksWorse scales delta by the same multiplier ratios; the
                // unity fast path is only used where it is bit-identical.
                const price_t perspective =
                    m_.hedge_unity[idx]
                        ? static_cast<price_t>(static_cast<double>(delta) * hedge_ratio)
                        : static_cast<price_t>(static_cast<double>(delta) * hedge_ratio *
                                               m_.pmult[lean] / m_.pmult[contract] *
                                               m_.qmult[lean] / m_.qmult[contract]);
                ticks_worse = perspective >= 2 * m_.tick[contract].divisor;
            }
            if (crossing || hedge_price == tachyon::NULL_PRICE || ticks_worse) {
                remove_request = true;
                if (++skip_club_count_ >= 50) {  // fail-safe; run() reports it to the strategy
                    disable_requested_ = true;
                    break;
                }
                continue;
            }
            hedge_order order;
            order.contract = lean;
            order.side_ = other_sd;
            order.price = hedge_price;
            order.size = qty;
            order.external = external;
            order.hedge_id = ++hedge_id_;
            order.market_index = market_index(lean, sd, external);
            if (port_.send_hedge != nullptr) {
                port_.send_hedge(port_.ctx, order);
            }
            add_position(s, lean, other_sd == side::bid ? static_cast<double>(qty)
                                                        : -static_cast<double>(qty));
            sent = true;
        }
        if (need < 0.0 || is_zero(need)) {
            remove_request = true;
            break;
        }
    }
    if (!sent) {
        remove_request = true;
    }
    return need;
}
void hedger::add_position(cycle_state& s, uint32_t contract, double size) noexcept {
    s.position[contract] += size;
    // legacy addPositionToVector (:2614-2637)
    if (m_.is_spread(contract)) {
        for (uint32_t k = m_.leg_begin[contract]; k < m_.leg_begin[contract + 1]; ++k) {
            const uint32_t leg = m_.leg_index[k];
            const double leg_size = size * m_.leg_ratio[k];
            for (uint32_t v = 0; v < m_.n_vectors; ++v) {
                s.pos_vec[v] += m_.risk[static_cast<size_t>(leg) * m_.n_vectors + v] * leg_size *
                                m_.qmult[contract];
            }
        }
    } else {
        // Outright exposure. Contracts without legs beyond the pricing set cannot be mapped
        // through the risk matrix (legacy read past it); skip defensively.
        if (contract >= m_.n_pricing) {
            return;
        }
        for (uint32_t v = 0; v < m_.n_vectors; ++v) {
            s.pos_vec[v] += m_.risk[static_cast<size_t>(contract) * m_.n_vectors + v] * size *
                            m_.qmult[contract];
        }
    }
}

bool hedger::run(cycle_state& s) noexcept {
    if (request_count_ == 0) {
        return false;
    }
    // At most one hedge contract per cycle (S10). FIFO: legacy's unordered_map order was
    // unspecified (D-V4).
    const uint32_t index = 0;
    const uint32_t contract = requests_[index].contract;
    const double request_size = requests_[index].size;
    const side request_side = request_size > 0 ? side::bid : side::ask;
    const double res = residual(s, contract, request_side);
    const bool same_side =
        ((request_size >= 0) & (res >= 0)) | ((request_size <= 0) & (res <= 0));
    const double size = request_size + res;
    const double abs_size = std::fabs(size);
    const side hedge_side = size > 0 ? side::bid : side::ask;

    bool remove_request = false;
    double left = abs_size;
    if (abs_size <= static_cast<double>(max_clip_[contract])) {
        left = send_hedges(s, contract, abs_size, hedge_side, remove_request, std::fabs(res));
    } else {  // S12: the request is larger than the rhone clip
        disable_requested_ = true;
        remove_request = true;
    }

    if (remove_request) {
        add_position(s, contract, request_size);
        erase_request(index);
    } else {
        double remaining = 0.0;
        double hedged = 0.0;
        if (same_side) {
            remaining = std::max(0.0, left - std::fabs(res));
            hedged = std::fabs(request_size) - remaining;
        } else {
            remaining = left;
            hedged = std::fabs(request_size) - remaining;
        }
        add_position(s, contract, request_side == side::bid ? hedged : -hedged);
        if (is_zero(remaining)) {
            erase_request(index);
        } else {
            requests_[index].size = request_side == side::bid ? remaining : -remaining;
        }
    }
    return disable_requested_;
}

}  // namespace tachyon::trade::rv
