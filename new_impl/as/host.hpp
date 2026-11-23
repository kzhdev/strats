#pragma once

// Host for the new AS engine: the composition root that owns the model, the runtime state, the
// ledger bridge and the four collaborators, and exposes the event surface the legacy host calls.
// Everything here is thread-confined to the strategy thread and allocation-free after init.
//
// Responsibilities (each maps to a named legacy function):
//   on_market_data                      -- onMDUpdate / processInternalMarketUpdate
//   on_fill                             -- handleFill (+ onManualFill)
//   on_stf                              -- handleSTFUpdate -> calculateShortTermFairOffset
//   on_hedge_ack / on_hedge_done        -- handleHedgeMarketDataAdjustmentAck / InternalHedgeDone
//   run_cycle                           -- runCycle -> updateQuotes -> updateQuote
//   disable                             -- disable() + handleDisabled() (maxTicks violation)

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <common/liquidity_ledger.hpp>

#include "hedger.hpp"
#include "ledger_bridge.hpp"
#include "model.hpp"
#include "paths.hpp"
#include "policy.hpp"
#include "quotes.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::as {

// What the host emits for one side of one contract (legacy ReadyOrderRequests._quote[side]).
struct quote_request {
    uint32_t contract = INVALID_INDEX;
    side sd = side::bid;
    price_t internal_price = NULL_PRICE;
    qty_t internal_size = 0;
    price_t external_price = NULL_PRICE;
    qty_t external_size = 0;
    bool quote_external = false;  // canQuoteExternal for this contract
    bool lean_on_rv = false;      // legacy sendQuoteRequest's `pBestPath->_leanOnRV`
};

struct hedge_order {
    uint32_t contract = INVALID_INDEX;        // the contract that filled
    uint32_t hedge_contract = INVALID_INDEX;  // the lean to trade
    side sd = side::bid;
    price_t internal_price = NULL_PRICE;
    price_t external_price = NULL_PRICE;
    qty_t size = 0;
    bool external = false;
    uint32_t hedge_id = 0;
    uint32_t market_index = 0;   // legacy marketId (book index)
    market_key target = 0;       // the same book as a ledger key, for the ack/done messages
    uint32_t path_id = 0;
};

// Outbound sinks. Every one is optional, so the host runs with no order path at all.
struct host_ports {
    void (*send_quote)(void* ctx, const quote_request&) = nullptr;
    void (*notify_quote)(void* ctx, uint32_t contract) = nullptr;
    void (*send_hedge)(void* ctx, const hedge_order&) = nullptr;
    // The fill AS is covering, re-announced to the front (legacy sendFillToFront).
    void (*send_fill)(void* ctx, uint32_t contract, side sd, qty_t qty, price_t price) = nullptr;
    // A switch-path fill handed to RV (legacy ASFillsToRV).
    void (*send_switch_fill)(void* ctx, const hedge_plan&) = nullptr;
    // A fill AS could not hedge, forwarded to the market maker (FillSource::ASUnableToHedge).
    void (*forward_fill)(void* ctx, uint32_t contract, side sd, qty_t qty) = nullptr;
    void (*on_alert)(void* ctx, const char* reason) = nullptr;
    void (*cancel_all)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

struct host_stats {
    uint64_t cycles = 0;
    uint64_t quotes_emitted = 0;
    uint64_t quote_notifications = 0;
    uint64_t quotes_blocked = 0;  // a cycle arrived while quotes could not be sent
    uint64_t hedges_sent = 0;
    uint64_t hedge_legs = 0;
    uint64_t switch_transfers = 0;
    uint64_t reservations_taken = 0;
    uint64_t unhedged_fills = 0;
    uint64_t validation_failures = 0;  // maxTicksThroughMarket violations
    bool disabled = false;
};

// Shared with rv: both track hedge ids handed out by the executor, from the same id space.
using tachyon::trade::MAX_TRACKED_HEDGES;

class host {
public:
    // `ledger` may be nullptr: the engine then runs on raw displayed sizes (parity mode).
    host(const model& m, cycle_state& s, liquidity_ledger* ledger, const host_ports& ports) noexcept;
    ~host();

    host(const host&) = delete;
    host& operator=(const host&) = delete;

    // ---- wiring (init time) ----------------------------------------------------
    void wire_market(uint32_t contract, side sd, bool external, market_key key,
                     liquidity_source source) noexcept;
    void set_external_quoting_enabled(bool on) noexcept { external_quoting_allowed_ = on; }
    void set_venue_enabled(uint32_t venue, bool on) noexcept;
    // False while a transaction is open: emission is deferred and the pending change retained.
    void set_can_send_quotes(bool on) noexcept { can_send_quotes_ = on; }
    // Re-enable after a disable (legacy operator re-enable), which re-quotes everything.
    void enable(bool on) noexcept;

    // ---- inputs (strategy thread) ----------------------------------------------
    void on_market_data(uint32_t contract, side sd, bool external, price_t price,
                        qty_t size) noexcept;
    void on_fill(uint32_t contract, side sd, qty_t qty, bool is_internal) noexcept;
    void on_stf(uint32_t contract, price_t fair) noexcept;
    // The transport knows which book the hedge went to, exactly as the legacy ack message does.
    void on_hedge_ack(uint32_t hedge_id, market_key target, ord_id_t order_id, qty_t actual) noexcept;
    void on_hedge_done(uint32_t hedge_id, market_key target) noexcept;

    // ---- cycle -----------------------------------------------------------------
    void run_cycle(uint64_t now_cycles) noexcept;

    // ---- inspection ------------------------------------------------------------
    const host_stats& stats() const noexcept { return stats_; }
    const cycle_state& state() const noexcept { return s_; }
    const model& get_model() const noexcept { return m_; }
    path_engine& paths() noexcept { return paths_; }
    quote_engine& quotes() noexcept { return quotes_; }
    hedger& hedge_engine() noexcept { return hedger_; }
    ledger_bridge* bridge() noexcept { return bridge_; }
    bool enabled() const noexcept { return enabled_ && !external_quoting_disabled_; }
    bool external_quoting_enabled() const noexcept {
        return external_quoting_allowed_ && !external_quoting_disabled_;
    }

private:
    void cycle_contract(uint32_t contract) noexcept;
    void emit_side(uint32_t contract, side sd, bool lean_on_rv) noexcept;
    void reserve_hedge_legs(const hedge_plan& plan) noexcept;
    void disable(const char* reason) noexcept;
    bool can_quote_external(uint32_t contract) const noexcept;

    static qty_t book_size_at(void* ctx, uint32_t contract, side sd, bool external) noexcept;
    static quote_policy build_policy(ledger_bridge* bridge, cycle_state& s) noexcept;

    const model& m_;
    cycle_state& s_;
    std::unique_ptr<ledger_bridge> bridge_storage_;  // declared before the raw pointer it owns
    ledger_bridge* bridge_ = nullptr;
    host_ports ports_{};
    quote_policy policy_{};
    path_engine paths_;
    quote_engine quotes_;
    hedger hedger_;

    std::array<uint8_t, 32> venue_enabled_{};
    host_stats stats_{};

    bool enabled_ = true;
    bool external_quoting_allowed_ = true;
    bool external_quoting_disabled_ = false;  // legacy disableQuoteExternal
    bool can_send_quotes_ = true;
};

}  // namespace tachyon::trade::as
