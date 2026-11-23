#pragma once

// The three strategy adapters: the only place that knows both a strategy engine and the executor.
//
// Each adapter implements the port the strategy already publishes through -- mm::request_sink for
// the market maker, the function-pointer host_ports for RV and AS -- and turns it into a push into
// that strategy's own mailbox. Nothing else changes: the strategies keep emitting the same intents
// in the same order, and the executor decides what happens to them.
//
// Field mapping (legacy order request -> exec::order_request):
//
//   mm::order_intent.contract        -> contract
//   mm::order_intent.sd              -> side
//   quote: internal_price/size       -> internal price/size
//          external_price/size       -> external leg
//          persist                   -> flag_persist (legacy passes quoteExternally)
//          lean_on_rv                -> flag_lean_on_rv
//   hedge: size                      -> hedge_size (legacy _sizeToQuote)
//          in_payup                  -> flag_pay_up
//          external (implicit: true) -> flag_external (MM hedges are LMT and always go out)
//
//   rv::quote_request / as::quote_request carry the same fields under their own names, and their
//   hedge_order.price becomes both the internal and the external limit: the strategy engines have
//   already resolved the lean, and external limit refinement belongs to the OM layer.
//
// `notify`/`notify_quote` are accepted and ignored: the legacy notification existed only to push a
// message into the executor's ring so the executor would look at the queue. The executor drains
// every mailbox once per cycle on its own schedule, so the notification has no work left to do.

// The strategy-side headers are included here, not by the exec core: the executor itself stays
// free of any strategy dependency.
#include <as/host.hpp>
#include <mm/adapter.hpp>
#include <rv/host.hpp>

#include "order_request.hpp"
#include "types.hpp"

namespace tachyon::trade::exec {

// Each module keeps its own side enum (mm::side, rv::side, as::side); all of them use bid = 0,
// ask = 1. The static assertions pin that contract, so a change in any strategy is a compile error
// here rather than a silent mirror-image order.
template <class strategy_side>
constexpr side to_side(strategy_side value) noexcept {
    static_assert(sizeof(strategy_side) == sizeof(uint8_t), "side must stay one byte");
    static_assert(static_cast<int>(strategy_side::bid) == 0 &&
                      static_cast<int>(strategy_side::ask) == 1,
                  "strategy side values must match exec::side");
    return static_cast<side>(value);
}

// The reverse direction, for the inbound bridges (`strategy_bridges.hpp`).
template <class strategy_side>
constexpr strategy_side from_side(side value) noexcept {
    static_assert(static_cast<int>(strategy_side::bid) == 0 &&
                      static_cast<int>(strategy_side::ask) == 1,
                  "strategy side values must match exec::side");
    return static_cast<strategy_side>(value);
}

// Every adapter is a template on the executor type so that both mailbox modes -- and a test double
// -- work without a virtual call on the push path.
template <class Executor>
class mm_request_sink final : public mm::request_sink {
public:
    explicit mm_request_sink(Executor& executor) noexcept : executor_(executor) {}

    void send_quote(const mm::order_intent& intent) noexcept override {
        executor_.submit(strategy_id::MM,
                         make_quote(strategy_id::MM, intent.contract, to_side(intent.sd),
                                    intent.internal_price, intent.internal_size,
                                    intent.external_price, intent.external_size, intent.persist,
                                    false, 0));
    }

    void send_hedge(const mm::order_intent& intent) noexcept override {
        // The MM hedge is an LMT order: it rests internally *and* goes to the exchange
        // (legacy modifyOrder + _pendingHedge, InternalMatchingEngine.h:2017-2019, :569-607).
        executor_.submit(strategy_id::MM,
                         make_hedge(strategy_id::MM, intent.contract, to_side(intent.sd),
                                    intent.internal_price, intent.external_price, intent.size,
                                    /*external=*/true, intent.in_payup));
    }

    void notify(uint32_t /*contract*/) noexcept override {}

    void cancel_orders(uint32_t contract, bool disable_gtc) noexcept override {
        if (contract == INVALID_CONTRACT) {
            executor_.cancel_strategy(strategy_id::MM, disable_gtc);
        } else {
            executor_.cancel_contract(contract, strategy_id::MM, disable_gtc);
        }
    }

    void alert(uint32_t contract, const char* reason) noexcept override {
        executor_.alert(contract, reason, 0);
    }

    // The GUI fill report belongs to the front-end adapter; the executor's fill path is
    // strategy_endpoint::on_fill, which the engine drives itself.
    void report_fill(uint32_t /*contract*/, const mm::fill_outcome& /*outcome*/) noexcept override {}

private:
    Executor& executor_;
};


template <class Executor>
class rv_adapter {
public:
    explicit rv_adapter(Executor& executor) noexcept : executor_(executor) {}

    rv::host_ports ports() noexcept {
        rv::host_ports ports;
        ports.send_quote = &rv_adapter::send_quote;
        ports.send_hedge = &rv_adapter::send_hedge;
        ports.notify_quote = &rv_adapter::notify_quote;
        ports.on_alert = &rv_adapter::alert;
        ports.ctx = this;
        return ports;
    }

private:
    static void send_quote(void* ctx, const rv::quote_request& request) noexcept {
        auto& self = *static_cast<rv_adapter*>(ctx);
        self.executor_.submit(strategy_id::RV,
                              make_quote(strategy_id::RV, request.contract, to_side(request.side_),
                                         request.price, request.size, request.external_price,
                                         request.external_size, request.quote_external, false, 0));
    }

    static void send_hedge(void* ctx, const rv::hedge_order& order) noexcept {
        auto& self = *static_cast<rv_adapter*>(ctx);
        // RV hedges are FAK: the executor matches them internally and sends only the remainder out
        // (legacy handleFAKHedgeOrder, :2028-2043).
        self.executor_.submit(strategy_id::RV,
                              make_hedge(strategy_id::RV, order.contract, to_side(order.side_), order.price,
                                         order.price, order.size, order.external, false,
                                         order.hedge_id, order.market_index));
    }

    static void notify_quote(void*, uint32_t) noexcept {}
    static void alert(void* ctx, const char* reason) noexcept {
        static_cast<rv_adapter*>(ctx)->executor_.alert(INVALID_CONTRACT, reason, 0);
    }

    Executor& executor_;
};

template <class Executor>
class as_adapter {
public:
    explicit as_adapter(Executor& executor) noexcept : executor_(executor) {}

    as::host_ports ports() noexcept {
        as::host_ports ports;
        ports.send_quote = &as_adapter::send_quote;
        ports.send_hedge = &as_adapter::send_hedge;
        ports.notify_quote = &as_adapter::notify_quote;
        ports.on_alert = &as_adapter::alert;
        ports.cancel_all = &as_adapter::cancel_all;
        // send_fill / send_switch_fill / forward_fill stay with the strategy wiring: they move
        // fills between strategies, not orders into the book.
        ports.ctx = this;
        return ports;
    }

private:
    static void send_quote(void* ctx, const as::quote_request& request) noexcept {
        auto& self = *static_cast<as_adapter*>(ctx);
        self.executor_.submit(strategy_id::AS,
                              make_quote(strategy_id::AS, request.contract, to_side(request.sd),
                                         request.internal_price, request.internal_size,
                                         request.external_price, request.external_size,
                                         request.quote_external, request.lean_on_rv, 0));
    }

    static void send_hedge(void* ctx, const as::hedge_order& order) noexcept {
        auto& self = *static_cast<as_adapter*>(ctx);
        // The hedge is traded in `hedge_contract`, which is not necessarily the contract that
        // filled, so the request must be routed by the hedge contract.
        self.executor_.submit(strategy_id::AS,
                              make_hedge(strategy_id::AS, order.hedge_contract, to_side(order.sd),
                                         order.internal_price, order.external_price, order.size,
                                         order.external, false, order.hedge_id, order.market_index));
    }

    static void notify_quote(void*, uint32_t) noexcept {}
    static void alert(void* ctx, const char* reason) noexcept {
        static_cast<as_adapter*>(ctx)->executor_.alert(INVALID_CONTRACT, reason, 0);
    }
    static void cancel_all(void* ctx) noexcept {
        static_cast<as_adapter*>(ctx)->executor_.cancel_strategy(strategy_id::AS, true);
    }

    Executor& executor_;
};

}  // namespace tachyon::trade::exec
