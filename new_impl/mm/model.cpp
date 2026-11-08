#include "model.hpp"

#include <stdexcept>

namespace tachyon::trade::mm {
namespace {

[[noreturn]] void fail(const std::string& name, uint32_t index, const std::string& what) {
    throw std::runtime_error((name.empty() ? "contract[" + std::to_string(index) + "]" : name) +
                             ": " + what);
}

// Legacy GetCyclesFromSeconds (Common.h:909-911), transcribed: seconds * 1e9 * frequency,
// truncated to uint64.
uint64_t cycles_from_seconds(double seconds, double cycles_per_second) noexcept {
    return static_cast<uint64_t>(seconds * 1000000000.0 * cycles_per_second);
}

}  // namespace

model model::build(const model_inputs& in) {
    if (in.contracts.size() > MAX_CONTRACTS) {
        throw std::runtime_error("mm: " + std::to_string(in.contracts.size()) +
                                 " contracts configured, cap is " + std::to_string(MAX_CONTRACTS) +
                                 " (limits.hpp)");
    }

    const int64_t k_unbounded = std::numeric_limits<int64_t>::max();

    model m;
    m.n_contracts = static_cast<uint32_t>(in.contracts.size());
    m.cycles_per_second = in.cycles_per_second;
    m.initial_shift = in.current_shift < SHIFT_COUNT ? in.current_shift : 0;

    for (size_t idx = 0; idx < in.contracts.size(); ++idx) {
        const contract_input& c = in.contracts[idx];
        const uint32_t ci = static_cast<uint32_t>(idx);
        const std::string& name = c.name;

        if (c.tick_size <= 0) {
            fail(name, ci, "tick size must be positive (check the secdb `tick` column)");
        }
        if (c.lot == 0) {
            fail(name, ci, "lot (secdb order_increment_qty) must be >= 1");
        }

        // Legacy initialize (:200-204): the quote size is lot-floored, and a zero is fatal.
        const qty_t quote_size = to_valid_qty(c.quote_size, c.lot);
        if (quote_size == 0) {
            fail(name, ci, "quoteSize is 0");
        }

        // Legacy ViperConfig.h:900-903: a non-positive worsen-payup percentage is rejected.
        if (c.worsen_payup_pct <= 0.0 || is_zero(c.worsen_payup_pct)) {
            fail(name, ci, "_worsenPayupCondPercent must be > 0");
        }

        // Legacy ViperConfig.h:904-907: a queue importance above 10 is silently zeroed.
        const uint32_t queue_importance = c.queue_importance > 10 ? 0 : c.queue_importance;

        // Legacy initialize (:218-225). Note the tick products are truncated int64 results of
        // a double * int64 expression, exactly as legacy assigns them (PLAN.md §D2 sites 2-3).
        const price_t hedge_offset_price =
            static_cast<price_t>(c.hedge_offset * static_cast<double>(c.tick_size));
        const price_t max_ticks_wide =
            c.max_ticks_wide < 0 ? k_unbounded : static_cast<price_t>(c.max_ticks_wide) * c.tick_size;
        const price_t max_ticks_wide_2x = c.max_ticks_wide < 0 ? k_unbounded : max_ticks_wide * 2;
        const price_t worsen_payup_amount =
            static_cast<price_t>(c.worsen_payup_pct * static_cast<double>(c.tick_size));

        m.tick_size.push_back(c.tick_size);
        m.tick.push_back(make_fast_div(c.tick_size));
        m.lot.push_back(c.lot);
        m.venue.push_back(c.venue);
        m.enabled.push_back(c.enabled ? 1 : 0);
        m.quote_external.push_back(c.quote_external ? 1 : 0);

        m.quote_size.push_back(quote_size);
        m.quote_gap.push_back(c.quote_gap);
        m.pct_tob_max_quote.push_back(c.pct_tob_max_quote);
        m.width_pct.push_back(c.width_pct);
        m.join_size.push_back(c.join_size);
        m.leave_size.push_back(c.leave_size);
        m.payup_qty.push_back(c.payup_qty);
        m.payup_orders.push_back(c.payup_orders);
        m.payup_multiple.push_back(c.payup_multiple);
        m.max_pos_internal.push_back(c.max_pos_internal);
        m.max_pos_external.push_back(c.max_pos_external);

        m.commission.push_back(to_mm_price(c.commission));
        m.min_internal_price_change.push_back(to_mm_price(c.min_internal_price_change));
        m.hedge_offset_price.push_back(hedge_offset_price);
        m.max_ticks_wide.push_back(max_ticks_wide);
        m.max_ticks_wide_2x.push_back(max_ticks_wide_2x);
        m.worsen_payup_amount.push_back(worsen_payup_amount);

        m.est_level_ema.push_back(c.est_level_ema);
        m.est_level_ema_period.push_back(cycles_from_seconds(
            static_cast<double>(c.est_level_ema_seconds), m.cycles_per_second));
        m.est_level_pct_ema.push_back(c.est_level_pct_ema);

        m.queue_importance.push_back(queue_importance);
        m.init_internal_position.push_back(c.internal_position);
        m.init_external_position.push_back(c.external_position);
    }

    return m;
}

}  // namespace tachyon::trade::mm
