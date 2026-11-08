#pragma once

// Integration seam (PLAN.md §8): the object the legacy host (MarketMaker / MarketDataConsumer /
// Executor) talks to, holding the engine, the state and the host, and translating emitted intents
// into the operations Viper's transport needs.
//
// It is deliberately framework-free: no Viper, boost or protobuf header is included, so it builds
// and is tested here, and the legacy side only has to write the shim that converts its message
// types into these calls and `request_sink` into OrderRequestManager. The legacy enum mapping is
// documented in PLAN.md §8.

#include <cstdint>
#include <memory>

#include "config.hpp"
#include "host.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "positions.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// A market-data snapshot the way the feed path produces it: the levels plus the feed's 1-based
// change numbers and the batch id (see MarketDataConsumer.h's copyBooks).
struct market_data_view {
    const level* levels = nullptr;
    uint32_t count = 0;
    uint32_t price_num = 0;
    uint32_t qty_num = 0;
    uint64_t batch_id = 0;
};

// Where emitted requests go. The legacy shim implements this over OrderRequestManager; a test
// implements it over a recorder. Every method is on the cold path, so virtual dispatch is fine.
class request_sink {
public:
    virtual ~request_sink() = default;
    // Legacy sendQuoteRequest(internalId, id, side, intPrice, intSize, extPrice, extSize, persist,
    //                         leanOnRV, execToken)
    virtual void send_quote(const order_intent& intent) = 0;
    // Legacy sendHedgeRequest(internalId, id, side, intPrice, extPrice, size, isExternal, inPayUp,
    //                         execToken)
    virtual void send_hedge(const order_intent& intent) = 0;
    // Legacy notifyOrderRequest.
    virtual void notify(uint32_t contract) = 0;
    // Legacy sendCancelRequest(strategy, internalId, disableUseGTC).
    virtual void cancel_orders(uint32_t contract, bool disable_gtc) = 0;
    // Legacy sendErrorMessage(..., ErrorType::Critical).
    virtual void alert(uint32_t contract, const char* reason) = 0;
    // Legacy sendFillToFront.
    virtual void report_fill(uint32_t contract, const fill_outcome& outcome) = 0;
};

// The composition root the legacy host drives: one instance per strategy resource (all contracts).
class adapter {
public:
    // Parses the config and builds the engine. Throws std::runtime_error on a malformed config,
    // exactly like model::build does.
    adapter(const mm_model_cfg& cfg, const secdb_table& secdb, double cycles_per_second,
            uint32_t initial_shift, request_sink& sink);

    // ---- market data -------------------------------------------------------
    bool on_market_data(uint32_t contract, side sd, const market_data_view& view) noexcept {
        return host_->on_market_data(contract, sd, view.levels, view.count, view.price_num,
                                     view.qty_num, view.batch_id);
    }
    bool on_internal_book(uint32_t contract, side sd, const level* levels, uint32_t count,
                          uint64_t batch_id) noexcept {
        return host_->on_internal_book_update(contract, sd, levels, count, batch_id);
    }

    // ---- order / queue-position / fill events ------------------------------
    void on_order_update(uint32_t contract, uint32_t order_id, price_t price, qty_t qty, qty_t qp,
                         side sd) noexcept {
        host_->on_order_update(contract, order_id, price, qty, qp, sd);
    }
    void on_queue_position_update(uint32_t contract, uint32_t order_id, qty_t qp) noexcept {
        host_->on_queue_position_update(contract, order_id, qp);
    }
    fill_outcome on_fill(uint32_t contract, const fill& f) noexcept {
        return host_->on_fill(contract, f);
    }
    void end_of_fills(uint32_t contract) noexcept { host_->end_of_fills(contract); }

    // ---- cycle ------------------------------------------------------------
    void run_cycle(uint64_t now_cycles) noexcept { host_->run_cycle(now_cycles); }

    // ---- gates the legacy side owns ---------------------------------------
    void enable(bool on) noexcept { host_->set_strategy_enabled(on); }
    void set_external_quoting_allowed(bool on) noexcept {
        host_->set_external_quoting_allowed(on);
    }
    void set_venue_enabled(uint32_t venue, bool on) noexcept { host_->set_venue_enabled(venue, on); }
    void set_can_send_quotes(bool on) noexcept { host_->set_can_send_quotes(on); }
    void set_contract_can_trade(uint32_t contract, bool on) noexcept {
        if (contract < model_->n_contracts) state_->contract(contract).contract_can_trade = on;
    }
    void set_contract_enabled(uint32_t contract, bool on) noexcept {
        if (contract < model_->n_contracts) state_->contract(contract).enabled = on;
    }
    // Legacy onShiftUpdate: only the active width percentage changes (:1540-1543).
    void set_shift(uint32_t shift) noexcept {
        for (uint32_t c = 0; c < model_->n_contracts; ++c) {
            state_->contract(c).width_pct = model_->width_pct_of(c, shift);
        }
        current_shift_ = shift;
    }

    // ---- inspection -------------------------------------------------------
    const model& get_model() const noexcept { return *model_; }
    cycle_state& get_state() noexcept { return *state_; }
    const host& get_host() const noexcept { return *host_; }
    const host_stats& stats() const noexcept { return host_->stats(); }
    uint32_t current_shift() const noexcept { return current_shift_; }
    bool ready() const noexcept { return host_ != nullptr; }

private:
    static void trampoline_quote(void* ctx, const order_intent& it) noexcept;
    static void trampoline_hedge(void* ctx, const order_intent& it) noexcept;
    static void trampoline_notify(void* ctx, uint32_t contract) noexcept;
    static void trampoline_cancel(void* ctx, uint32_t contract, bool disable_gtc) noexcept;
    static void trampoline_alert(void* ctx, uint32_t contract, const char* reason) noexcept;
    static void trampoline_fill(void* ctx, uint32_t contract, const order_intent*,
                                const fill_outcome& outcome) noexcept;

    // Declared before the host so the host is destroyed first.
    std::unique_ptr<model> model_;
    std::unique_ptr<cycle_state> state_;
    std::unique_ptr<host> host_;
    request_sink* sink_ = nullptr;
    uint32_t current_shift_ = 0;
};

}  // namespace tachyon::trade::mm
