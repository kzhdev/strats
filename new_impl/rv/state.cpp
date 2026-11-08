#include "state.hpp"

namespace tachyon::trade::rv {

void cycle_state::init(const model& m) noexcept {
    for (uint32_t c = 0; c < m.n_all; ++c) {
        for (uint32_t sd = 0; sd < SIDE_COUNT; ++sd) {
            for (uint32_t ext = 0; ext < 2; ++ext) {
                market(c, static_cast<side>(sd), ext != 0) = market_state{};
            }
        }
    }
    for (uint32_t v = 0; v < m.n_vectors; ++v) {
        ltf[v] = m.init_ltf[v];
        prior_factor[v] = 0.0;
        factor[v] = 0.0;
        pos_vec[v] = 0.0;
    }
    mid.fill(0.0);
    stf.fill(0);
    pricing.fill(0.0);
    stf_vec.fill(0.0);
    pricing_stf.fill(0.0);
    scratch.fill(0.0);
    position.fill(0.0);
    for (uint32_t i = 0; i < m.n_all * SIDE_COUNT; ++i) {
        quotes[i] = quote{};
    }
    clear_dirty();
    last_ema_time = 0;
}

const market_state& cycle_state::best_market(uint32_t contract, side sd) const noexcept {
    const market_state& internal = market(contract, sd, false);
    const market_state& external = market(contract, sd, true);
    // legacy BetterMarket<side>: NULL_PRICE loses, otherwise higher bid / lower ask wins
    if (internal.price == tachyon::NULL_PRICE) {
        return external;
    }
    if (external.price == tachyon::NULL_PRICE) {
        return internal;
    }
    if (sd == side::bid) {
        return internal.price >= external.price ? internal : external;
    }
    return internal.price <= external.price ? internal : external;
}

bool cycle_state::best_is_external(uint32_t contract, side sd) const noexcept {
    return &best_market(contract, sd) == &market(contract, sd, true);
}

}  // namespace tachyon::trade::rv
