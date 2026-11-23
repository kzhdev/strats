#pragma once

#include <cstdint>
#include <vector>

#include "types.hpp"

// The two seams of the executor. Everything else in `exec` is concrete and allocation-free; these
// are the only polymorphic types, and they are crossed once per emitted order / per fill batch,
// never once per request (PLAN.md D-EX9).
namespace tachyon::trade::exec {

// Where the residual of the internal book goes: the exchange, or the OM layer that fronts it.
//
// The quote leg is one call per (contract, side) whose aggregated external top changed -- the
// legacy synchronizeQuote (:543-567) minus its banner/throttle gating, which is deferred. The
// hedge leg is one call per hedge request (legacy sendHedge :569-607, sendExternalFAKHedge
// :1884-1890). A `size == 0` quote is a cancel-with-price (legacy quotes size 0 to keep the
// price), and `price == NULL_PRICE` with size 0 is "leave the market".
class exchange_gateway {
public:
    virtual ~exchange_gateway() = default;

    virtual void send_quote(const exchange_order& order) noexcept = 0;
    virtual void send_hedge(const exchange_order& order) noexcept = 0;
    // Legacy sendCancelRequest: take one strategy (or one contract of it) out of the market.
    virtual void cancel(uint32_t contract, strategy_id strategy, bool disable_gtc) noexcept = 0;
    // Legacy sendErrorMessage(..., ErrorType::Critical) for the fault paths.
    virtual void alert(uint32_t contract, const char* reason, int32_t code) noexcept = 0;
};

// What the engine tells a strategy: the internal market it may trade against, and the fills it got.
class strategy_endpoint {
public:
    virtual ~strategy_endpoint() = default;

    // Legacy sendMarketUpdateToStrategy (:1297-1298): the best internal price the subscriber may
    // see, with the subscriber's own liquidity and lean-on-RV orders excluded.
    virtual void on_internal_market(uint32_t contract, side sd, const internal_top& top) noexcept = 0;
    // Legacy onInternalFill (:562-584) / onExternalFill (:586-596).
    virtual void on_fill(const internal_fill& fill) noexcept = 0;
    // Legacy onEndOfFills (:613-...): one per contract per batch. The MM state machine needs it.
    virtual void end_of_fills(uint32_t contract) noexcept = 0;
    // Legacy `ASInternalMatchingDone` (:1225-1228): how much AS matched per side as aggressor.
    virtual void on_as_match_done(uint32_t contract, qty_t bid_size, qty_t ask_size) noexcept {}
    // Legacy onInternalHedgeDone (:1898): a hedge completed internally in the given book.
    virtual void on_hedge_done(uint32_t contract, uint32_t hedge_id, uint32_t market_id,
                               side sd) noexcept {}
    // Legacy disableContractExternalQuoteOnAllStrategy (:1928): the internal book self-crossed,
    // so this contract may not quote externally until it is cleared. Not a strategy disable.
    virtual void on_self_match(uint32_t contract, const char* reason) noexcept {}
    // The engine must not route work to a disabled strategy (legacy :1996).
    virtual bool enabled() const noexcept = 0;
    virtual void disable() noexcept = 0;
};

// ---------------------------------------------------------------------------
// Recording implementations. Tests and the replay driver use these; they make the emitted
// sequence observable without any transport, which is how the "last request wins" and "residual
// goes out" invariants are asserted.
// ---------------------------------------------------------------------------
class recording_gateway final : public exchange_gateway {
public:
    struct alert_event {
        uint32_t contract;
        int32_t code;
    };

    void send_quote(const exchange_order& order) noexcept override { quotes.push_back(order); }
    void send_hedge(const exchange_order& order) noexcept override { hedges.push_back(order); }
    void cancel(uint32_t contract, strategy_id strategy, bool disable_gtc) noexcept override {
        cancels.push_back(cancel_event{contract, strategy, disable_gtc});
    }
    void alert(uint32_t contract, const char* reason, int32_t code) noexcept override {
        alerts.push_back(alert_event{contract, code});
        last_reason = reason ? reason : "";
    }

    struct cancel_event {
        uint32_t contract;
        strategy_id strategy;
        bool disable_gtc;
    };

    void clear() noexcept {
        quotes.clear();
        hedges.clear();
        cancels.clear();
        alerts.clear();
    }

    std::vector<exchange_order> quotes;
    std::vector<exchange_order> hedges;
    std::vector<cancel_event> cancels;
    std::vector<alert_event> alerts;
    const char* last_reason = "";
};

class recording_endpoint final : public strategy_endpoint {
public:
    explicit recording_endpoint(strategy_id id, bool is_enabled = true) noexcept
        : strategy(id), is_enabled(is_enabled) {}

    void on_internal_market(uint32_t contract, side sd, const internal_top& top) noexcept override {
        markets.push_back(market_event{contract, sd, top});
    }
    void on_fill(const internal_fill& fill) noexcept override { fills.push_back(fill); }
    void end_of_fills(uint32_t contract) noexcept override { ends.push_back(contract); }
    void on_as_match_done(uint32_t contract, qty_t bid_size, qty_t ask_size) noexcept override {
        as_done.push_back(as_done_event{contract, bid_size, ask_size});
    }
    void on_hedge_done(uint32_t contract, uint32_t hedge_id, uint32_t market_id,
                       side sd) noexcept override {
        hedge_dones.push_back(hedge_done_event{contract, hedge_id, market_id, sd});
    }
    void on_self_match(uint32_t contract, const char* reason) noexcept override {
        self_matches.push_back(contract);
        last_reason = reason ? reason : "";
    }
    bool enabled() const noexcept override { return is_enabled; }
    void disable() noexcept override {
        is_enabled = false;
        ++disables;
    }

    struct market_event {
        uint32_t contract;
        side sd;
        internal_top top;
    };
    struct as_done_event {
        uint32_t contract;
        qty_t bid_size;
        qty_t ask_size;
    };
    struct hedge_done_event {
        uint32_t contract;
        uint32_t hedge_id;
        uint32_t market_id;
        side sd;
    };

    void clear() noexcept {
        markets.clear();
        fills.clear();
        ends.clear();
        as_done.clear();
        hedge_dones.clear();
        self_matches.clear();
        disables = 0;
    }

    strategy_id strategy = strategy_id::MM;
    bool is_enabled = true;
    uint32_t disables = 0;
    const char* last_reason = "";
    std::vector<market_event> markets;
    std::vector<internal_fill> fills;
    std::vector<uint32_t> ends;
    std::vector<as_done_event> as_done;
    std::vector<hedge_done_event> hedge_dones;
    std::vector<uint32_t> self_matches;
};

}  // namespace tachyon::trade::exec
