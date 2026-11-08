#include "state.hpp"

namespace tachyon::trade::mm {

void cycle_state::init(const model& m) {
    n_contracts_ = m.n_contracts;
    queues_.assign(m.n_contracts, order_queue{});
    changed_count_ = 0;
    dirty_.fill(0);
    now_cycles = 0;

    for (uint32_t c = 0; c < m.n_contracts; ++c) {
        contract_state& s = contracts_[c];

        s.side_book(side::bid).init(side::bid);
        s.side_book(side::ask).init(side::ask);
        for (quote_state& q : s.quotes) {
            q = quote_state{};
        }

        // Legacy initialize (:192-236) values that are state rather than model constants.
        s.width_pct = m.width_pct_of(c, m.initial_shift);
        s.ema_est_level = m.est_level_ema[c];
        s.established_level = 1;
        s.last_tob_qty = 0.0;
        s.last_ema_time = 0;
        s.mid_point = 0;
        s.width = 0;

        s.internal_pos = m.init_internal_position[c];
        s.external_pos = m.init_external_position[c];

        s.enabled = m.enabled[c] != 0;
        s.quote_external = m.quote_external[c] != 0;
        s.quote_external_before_event = false;
        s.max_ticks_wide_breached = false;
        s.contract_can_trade = false;
        s.contract_can_cancel = false;
        s.gateway_active = true;
        s.order_execution_token = 0;
        s.last_md_bid = 0;
    }
}

}  // namespace tachyon::trade::mm
