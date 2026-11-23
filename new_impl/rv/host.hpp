#pragma once

// Host for the new RV strategy: the composition root that owns the model, the cycle state,
// the liquidity ledger bridge and the strategy object, and exposes the event API the legacy
// host calls (PLAN.md §4 "input surface"). Everything here is thread-confined to the strategy
// thread (D8) and allocation-free after init.
//
// Responsibilities (each maps to a named legacy function):
//   on_market_data / on_fill / on_hedge_ack / on_timer   -- RelativeValue.h handlers
//   validate (maxTicksThroughMarket)                     -- :2795-2824 + :2871-2879
//   emit_quotes                                          -- sendQuoteOrder, :2914-2950
//   hedge emission -> ledger reservation                  -- addFAKAdjustment, :2512

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "ledger_bridge.hpp"
#include "model.hpp"
#include "quotes.hpp"
#include "state.hpp"
#include "strategy.hpp"
#include "types.hpp"
#include "valuation.hpp"

namespace tachyon::trade::rv {

// Outbound sinks. Every one is optional so the host can run with no order path at all.
struct host_ports {
    void (*send_hedge)(void* ctx, const hedge_order& order) = nullptr;
    void (*send_quote)(void* ctx, const quote_request& request) = nullptr;
    void (*notify_quote)(void* ctx, uint32_t contract) = nullptr;
    // Legacy sendErrorMessage(Critical) on the disable paths.
    void (*on_alert)(void* ctx, const char* reason) = nullptr;
    void* ctx = nullptr;
};

struct host_stats {
    uint64_t cycles = 0;
    uint64_t quotes_emitted = 0;
    uint64_t quote_notifications = 0;
    uint64_t quotes_blocked = 0;        // cycle ran while quotes were un-sendable
    uint64_t hedges_sent = 0;
    uint64_t reservations_taken = 0;
    uint64_t validation_failures = 0;   // maxTicksThroughMarket violations
    uint64_t quote_removals = 0;        // emitted NULL/0 quotes (market gone or disabled)
    bool disabled = false;
};

class host {
public:
    // `ledger` may be nullptr: the strategy then runs with raw displayed sizes (parity mode).
    host(const model& m, cycle_state& s, liquidity_ledger* ledger,
         const host_ports& ports) noexcept;
    ~host();

    host(const host&) = delete;
    host& operator=(const host&) = delete;

    // ---- wiring (init time) ----
    // Rhone clip limit for a contract (legacy SetRhoneLimits): a hedge request larger than
    // this disables the strategy (S12). The model carries 0 as "not yet supplied", so the
    // host must set it or the first fill disables quoting.
    void set_max_clip(uint32_t contract, price_t clip) noexcept;
    // Map one displayed book to its ledger key and the source currently supplying it.
    // Required before market data flows if a ledger was supplied.
    void wire_market(uint32_t contract, side sd, bool external, market_key key,
                     liquidity_source source) noexcept;
    // canQuoteExternal: contract flag & strategy-wide gate & per-venue system gate.
    void set_external_quoting_enabled(bool on) noexcept { external_quoting_allowed_ = on; }
    void set_venue_enabled(uint32_t venue, bool on) noexcept;
    // canSendQuotes (false while an internal-fill transaction is open): emission is deferred,
    // the pending change is retained, exactly like legacy.
    void set_can_send_quotes(bool on) noexcept { can_send_quotes_ = on; }
    // Re-enable after a disable (legacy operator re-enable path).
    void enable(bool on) noexcept;

    // ---- inputs (strategy thread) ----
    void on_market_data(uint32_t contract, side sd, bool external, price_t price,
                        qty_t size) noexcept;
    void on_fill(uint32_t contract, side sd, qty_t qty) noexcept;
    void on_hedge_ack(uint32_t hedge_id, ord_id_t order_id, qty_t actual) noexcept;
    void on_hedge_done(uint32_t hedge_id) noexcept;
    void on_timer(uint64_t now_cycles) noexcept;

    // ---- inspection ----
    const host_stats& stats() const noexcept { return stats_; }
    const cycle_state& state() const noexcept { return s_; }
    const model& get_model() const noexcept { return m_; }
    strategy& engine() noexcept { return *strategy_; }
    bool ready() const noexcept { return strategy_ != nullptr; }
    ledger_bridge* bridge() noexcept { return bridge_; }
    bool external_quoting_enabled() const noexcept {
        return external_quoting_allowed_ && !disable_external_quoting_;
    }

private:
    static bool validate_static(void* ctx, uint32_t contract, side sd, price_t price,
                                uint32_t lean) noexcept;
    static void send_hedge_static(void* ctx, const hedge_order& order) noexcept;
    static void send_quote_static(void* ctx, uint32_t contract) noexcept;

    bool validate(uint32_t contract, side sd, price_t price, uint32_t lean) noexcept;
    void emit_hedge(const hedge_order& order) noexcept;
    void emit_quotes() noexcept;
    void emit_all_removals() noexcept;
    void disable(const char* reason) noexcept;
    bool can_quote_external(uint32_t contract) const noexcept;

    void remember_hedge(uint32_t hedge_id, uint32_t market_index) noexcept;
    bool lookup_hedge(uint32_t hedge_id, market_key& key) noexcept;

    struct hedge_slot {
        uint32_t hedge_id = 0;
        market_key key = 0;
        bool used = false;
    };
    static constexpr uint32_t MAX_TRACKED_HEDGES = tachyon::trade::MAX_TRACKED_HEDGES;

    const model& m_;
    cycle_state& s_;
    ledger_bridge* bridge_ = nullptr;
    std::unique_ptr<ledger_bridge> bridge_storage_;
    host_ports ports_{};
    quote_policy policy_{};
    order_port order_port_{};
    // Constructed in the body: the engine keeps the policy/port by value, so they must be
    // fully populated (including the host's validation gate) before the strategy exists.
    std::unique_ptr<strategy> strategy_;

    std::array<hedge_slot, MAX_TRACKED_HEDGES> hedges_{};
    std::array<uint8_t, 32> venue_enabled_{};
    host_stats stats_{};

    bool external_quoting_allowed_ = true;
    bool disable_external_quoting_ = false;
    bool can_send_quotes_ = true;
};

}  // namespace tachyon::trade::rv
