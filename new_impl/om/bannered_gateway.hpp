#pragma once

#include <cstdint>
#include <vector>

#include "banners.hpp"
#include "exec/ports.hpp"

// The OM seam: an `exchange_gateway` that decides whether an intent may leave, and forwards only what
// the legacy external order management would have sent.
//
// Why a decorator and not a rewrite of the engine's emission: the engine's job ends at "this is the
// residual" (exec/PLAN.md D-EX6), and the gates need state the engine must not own -- the external
// book top for max-ticks-off-top-level, and the ban windows, which are venue-behaviour knobs. Sitting
// on the gateway keeps that state out of the engine, keeps the engine allocation-free, and makes the
// whole layer testable through the existing `recording_gateway`.

namespace tachyon::trade::om {

class bannered_gateway final : public exec::exchange_gateway {
public:
    // Clock injection (om/PLAN.md D-OM2): the gate needs `now` for the ban windows, and a test needs
    // to advance time exactly. Production passes `clockUtil::GetCycles`, so the ban periods keep the
    // same units as the legacy `Banner::_expiryTime` comparisons.
    using now_fn = uint64_t (*)(void* ctx) noexcept;

    bannered_gateway(exec::exchange_gateway& next, now_fn now, void* ctx) noexcept
        : next_(next), now_(now), ctx_(ctx) {}

    struct stats {
        uint64_t quotes_forwarded = 0;
        uint64_t quotes_suppressed = 0;  // banned, and the intent was an improvement
        uint64_t quotes_degraded = 0;    // outside max ticks off top level, forwarded with size 0
        uint64_t hedges_forwarded = 0;
        uint64_t hedges_suppressed = 0;
    };

    // Per-contract configuration. `contracts` must be known before the first intent; the executor
    // already fixes that up front, so a plain resize at wiring time is enough.
    void configure(uint32_t contracts) {
        state_.clear();
        state_.resize(contracts);
        config_.assign(contracts, gate_config{});
        top_[0].assign(contracts, market_top{});
        top_[1].assign(contracts, market_top{});
    }

    void set_config(uint32_t contract, const gate_config& cfg) noexcept { config_[contract] = cfg; }

    // Fed by the market-data path. The legacy check reads `_marketBooks[BID].l[0].pr` (:624).
    void set_market_top(uint32_t contract, side sd, market_top top) noexcept {
        top_[detail::idx(sd)][contract] = top;
    }

    const gate_state& state(uint32_t contract) const noexcept { return state_[contract]; }
    const stats& counters() const noexcept { return stats_; }

    void send_quote(const exec::exchange_order& order) noexcept override {
        const uint32_t c = order.contract;
        const gate_result r = gate_quote(state_[c], config_[c], order.sd, order.price, order.size,
                                        top_[detail::idx(order.sd)][c], now_(ctx_));
        if (!r.send) {
            ++stats_.quotes_suppressed;
            return;
        }
        // Record what the venue will hold: legacy `_om[Side].quote()` leaves the price in place with
        // its resting size, including for a size-0 quote (which keeps the price).
        state_[c].resting_quote_price[detail::idx(order.sd)] = r.price;
        state_[c].resting_quote_size[detail::idx(order.sd)] = r.size;

        exec::exchange_order out = order;
        out.size = r.size;
        ++stats_.quotes_forwarded;
        if (r.degraded) ++stats_.quotes_degraded;
        next_.send_quote(out);
    }

    void send_hedge(const exec::exchange_order& order) noexcept override {
        const uint32_t c = order.contract;
        const gate_result r = gate_hedge(state_[c], config_[c], order.sd, order.price, order.size,
                                        order.pay_up, now_(ctx_));
        if (!r.send) {
            ++stats_.hedges_suppressed;
            return;
        }
        state_[c].resting_hedge_price[detail::idx(order.sd)] = r.price;
        ++stats_.hedges_forwarded;
        next_.send_hedge(order);
    }

    // Taking a contract out of the market drops the resting quote (legacy getQuoteOutOfMarket sets
    // `_om[Side]._quote = nullptr`, so the next quote is a "first quote in market" again).
    void cancel(uint32_t contract, exec::strategy_id strategy, bool disable_gtc) noexcept override {
        if (contract < state_.size()) {
            for (uint32_t i = 0; i < 2; ++i) {
                state_[contract].resting_quote_price[i] = NULL_PRICE;
                state_[contract].resting_quote_size[i] = 0;
            }
        }
        next_.cancel(contract, strategy, disable_gtc);
    }

    void alert(uint32_t contract, const char* reason, int32_t code) noexcept override {
        next_.alert(contract, reason, code);
    }

private:
    exec::exchange_gateway& next_;
    now_fn now_;
    void* ctx_;
    std::vector<gate_state> state_;
    std::vector<gate_config> config_;
    std::vector<market_top> top_[2];
    stats stats_{};
};

}  // namespace tachyon::trade::om
