#pragma once

#include <cstdint>

#include "hedger.hpp"
#include "model.hpp"
#include "quotes.hpp"
#include "state.hpp"
#include "valuation.hpp"

namespace tachyon::trade::rv {

// The RV engine: one object that owns the three collaborators and the cycle that drives them.
//
//   valuation   midpoints -> short-term fair values
//   quote_engine  fair values -> quotes (and the maxTicksThroughMarket gate)
//   hedger      fills -> hedge requests -> orders, and the position book
//
// The strategy itself is only the façade: it receives the inputs, keeps the enabled flag and
// the cycle counter, and calls the collaborators in the legacy order
// (RelativeValue::runCycle, :387). Nothing here allocates or dispatches virtually.
class strategy {
public:
    strategy(const model& m, cycle_state& s, const quote_policy& policy,
             const order_port& port) noexcept
        : m_(m),
          s_(s),
          policy_(policy),
          port_(port),
          valuation_(m),
          quotes_(m, policy),
          hedger_(m, policy, port) {}

    // ---- inputs (all on the strategy thread, D8) ----
    void on_market_data(uint32_t contract, side sd, bool external, price_t price,
                        qty_t size) noexcept;
    void on_fill(uint32_t contract, side sd, qty_t qty) noexcept;
    void set_max_clip(uint32_t contract, price_t clip) noexcept {
        hedger_.set_max_clip(contract, clip);
    }
    // Strategy-level disable (legacy disableRelativeValue, called from the quote validation
    // failure path, RelativeValue.h:2872-2879). While disabled the cycle is a no-op.
    void set_enabled(bool on) noexcept { enabled_ = on; }

    // ---- cycle ----
    void run_cycle(uint64_t now_cycles) noexcept;

    // ---- inspection ----
    bool enabled() const noexcept { return enabled_; }
    uint32_t hedge_request_count() const noexcept { return hedger_.request_count(); }
    double position_of(uint32_t contract) const noexcept { return s_.position[contract]; }
    uint32_t hedge_id() const noexcept { return hedger_.hedge_id(); }

    // The quote engine, for the host's validation gate (it owns the maxTicksThroughMarket
    // arithmetic) and for tests that inspect quotes without running a cycle.
    const quote_engine& quotes() const noexcept { return quotes_; }
    cycle_state& state() noexcept { return s_; }

private:
    void disable() noexcept { enabled_ = false; }

    const model& m_;
    cycle_state& s_;
    quote_policy policy_;
    order_port port_;

    valuation valuation_;
    quote_engine quotes_;
    hedger hedger_;

    bool enabled_ = true;
};

}  // namespace tachyon::trade::rv
