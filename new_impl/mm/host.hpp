#pragma once

#include <array>
#include <cstdint>

#include "book.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "positions.hpp"
#include "state.hpp"
#include "strategy.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// Outbound sinks. Every one is optional (nullptr => the host still runs, decisions unchanged), so
// the engine can be driven from a test or a replay with no transport at all.
struct host_ports {
    // One call per quote intent, in order (legacy modifyQuoteOrder -> sendQuoteRequest).
    void (*send_quote)(void* ctx, const order_intent& intent) = nullptr;
    // One call per hedge intent (legacy modifyHedgeOrder -> sendHedgeRequest).
    void (*send_hedge)(void* ctx, const order_intent& intent) = nullptr;
    // One call per contract per cycle that produced requests (legacy notifyOrderRequest).
    void (*notify)(void* ctx, uint32_t contract) = nullptr;
    // Legacy sendCancelRequest (strategy-wide, or per contract with disableGTC).
    void (*cancel_orders)(void* ctx, uint32_t contract, bool disable_gtc) = nullptr;
    // Critical alerts: the disable paths and the untaken-overfill path.
    void (*on_alert)(void* ctx, uint32_t contract, const char* reason) = nullptr;
    // A fill as it was allocated, for the fills grid (legacy sendFillToFront).
    void (*on_fill_report)(void* ctx, uint32_t contract, const order_intent* /*unused*/,
                           const fill_outcome& outcome) = nullptr;
    void* ctx = nullptr;
};

struct host_stats {
    uint64_t cycles = 0;
    uint64_t md_updates = 0;
    uint64_t stale_md_rejected = 0;
    uint64_t intents_emitted = 0;
    uint64_t notifications = 0;
    uint64_t fills = 0;
    uint64_t overfills_taken = 0;
    uint64_t untaken_overfills = 0;
    uint64_t max_ticks_wide_latches = 0;
    uint64_t disables = 0;
    uint64_t quote_removals = 0;
    bool disabled = false;
};

// Composition root: it owns nothing the engine does not need, and it is the only place that knows
// about transport, alerts and the "should we be quoting at all" gates.
//
// Responsibilities, each mapping to a legacy function:
//   on_market_data            copyBooks + stale-bid rejection (MarketMaker_MD.cpp:7-19)
//   on_internal_book_update   MarketMaker_MessageHandler.cpp:79-92 (`>=` bid rule, both indices reset)
//   run_cycle                 onMarketDataChange's outer loop (:36-41) + the maxTicksWide block (:452-482)
//   on_fill / end_of_fills    the fill fan-out (:40-43) and applyUpdates (:1194-1210)
//   enable/disable            start (:1255-1277) and disable (:1279-1295)
class host {
public:
    host(const model& m, cycle_state& s, const host_ports& ports) noexcept
        : m_(m), s_(s), ports_(ports), strat_(m), book_(m) {}

    // ---- wiring (init time) ----
    void set_strategy_enabled(bool on) noexcept {
        strategy_enabled_ = on;
        if (!on) stats_.disabled = true;
    }
    // canQuoteExternally (:367-369): contract flag & strategy gate & venue gate.
    void set_external_quoting_allowed(bool on) noexcept { external_quoting_allowed_ = on; }
    void set_venue_enabled(uint32_t venue, bool on) noexcept {
        if (venue < venue_enabled_.size()) venue_enabled_[venue] = on ? 1 : 0;
    }
    // canSendQuotes: while false, emission is deferred but the pending intents are retained.
    void set_can_send_quotes(bool on) noexcept { can_send_quotes_ = on; }

    // ---- inputs ----
    // A market-data snapshot for one side (levels + the feed's 1-based change numbers + batch id).
    bool on_market_data(uint32_t contract, side sd, const level* levels, uint32_t count,
                        uint32_t price_num, uint32_t qty_num, uint64_t batch_id) noexcept;
    bool on_internal_book_update(uint32_t contract, side sd, const level* levels, uint32_t count,
                                 uint64_t batch_id) noexcept;
    fill_outcome on_fill(uint32_t contract, const fill& f) noexcept;
    void end_of_fills(uint32_t contract) noexcept;

    // Legacy onOrderUpdate (:1562-1609): keep the resting-order book in step with the exchange.
    void on_order_update(uint32_t contract, uint32_t order_id, price_t price, qty_t qty, qty_t qp,
                         side sd) noexcept;
    // Legacy onQueuePositionUpdate (:1611-1642): record the position, and re-quote when the order
    // that moved is the one at the back of our own price level.
    void on_queue_position_update(uint32_t contract, uint32_t order_id, qty_t qp) noexcept;

    void run_cycle(uint64_t now_cycles) noexcept;

    // ---- inspection ----
    const host_stats& stats() const noexcept { return stats_; }
    const cycle_state& state() const noexcept { return s_; }
    const strategy& engine() const noexcept { return strat_; }
    bool contract_quotes_external(uint32_t c) const noexcept { return s_.contract(c).quote_external; }
    bool pending_intents(uint32_t c) const noexcept { return pending_[c]; }
    const intent_log& last_intents(uint32_t c) const noexcept { return last_log_[c]; }

private:
    // Legacy hasMarketData (:375-394): losing the book disables the contract, external quoting and
    // GTC. Returns false when the caller must bail out.
    bool has_market_data(uint32_t c) noexcept;
    // canQuoteExternally for one contract (:367-369).
    bool can_quote_externally(const contract_state& s, uint32_t c) const noexcept;
    void disable_contract(contract_state& s, uint32_t c, bool disable_gtc) noexcept;
    void disable_external_quoting(contract_state& s, uint32_t c) noexcept;
    // Emits a reset quote for one side (legacy modifyQuoteOrder with a cleared Quote).
    void emit_removal(contract_state& s, uint32_t c, side sd, intent_log& log) noexcept;
    void emit(intent_log& log) noexcept;
    void alert(uint32_t c, const char* reason) noexcept;

    const model& m_;
    cycle_state& s_;
    host_ports ports_{};
    strategy strat_;
    position_book book_;

    // Deferred emission: intents produced while quotes cannot be sent are retained and flushed on
    // the cycle that re-enables sending.
    intent_log last_log_[MAX_CONTRACTS];
    bool pending_[MAX_CONTRACTS] = {};
    std::array<uint8_t, 32> venue_enabled_{};

    host_stats stats_{};
    bool strategy_enabled_ = false;
    bool external_quoting_allowed_ = true;
    bool can_send_quotes_ = true;
};

}  // namespace tachyon::trade::mm
