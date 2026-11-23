// Phase-4 gate: the strategy adapters.
//
//   exec_adapter_tests
//
// Each adapter is the seam between a strategy engine and the executor: the strategy emits through
// the port it already published through (mm::request_sink, rv/as host_ports) and the adapter turns
// that into a push into the strategy's own mailbox. The cases below drive each port directly with
// the real strategy types, so a field that is dropped, swapped or routed to the wrong mailbox is
// caught here rather than in a replay.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <adapters/strategy_adapters.hpp>

#include "executor.hpp"
#include "ports.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;

// The strategy namespaces are aliased so the port types can be named without dragging their
// enumerators into scope (each has its own `side`, deliberately).
namespace mm = tachyon::trade::mm;
namespace rv = tachyon::trade::rv;
namespace as = tachyon::trade::as;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void check_eq(long long got, long long want, const std::string& what) {
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s (got %lld, want %lld)\n", what.c_str(), got, want);
    }
}

constexpr price_t PX(int64_t ticks) { return 25'000'000 * ticks; }

struct bench {
    bench() {
        executor_config config;
        config.contracts = 4;
        config.queue_capacity = 64;
        exec.init(config, &gateway);
        exec.register_strategy(strategy_id::MM, &mm);
        exec.register_strategy(strategy_id::RV, &rv);
        exec.register_strategy(strategy_id::AS, &as);
        mm_sink = new mm_request_sink<executor>(exec);
    }
    ~bench() { delete mm_sink; }

    // Drain the mailbox and keep the last request (the adapters push one per call).
    order_request peek(strategy_id strategy) {
        order_request request{};
        exec.mailbox(strategy).drain([&](const order_request& r) { request = r; });
        return request;
    }

    executor exec;
    recording_gateway gateway;
    recording_endpoint mm{strategy_id::MM};
    recording_endpoint rv{strategy_id::RV};
    recording_endpoint as{strategy_id::AS};
    mm_request_sink<executor>* mm_sink = nullptr;
};

}  // namespace

namespace {

void test_mm_quote_and_hedge() {
    std::printf("adapters: market maker\n");

    bench b;
    mm::order_intent quote;
    quote.type = mm::order_intent::kind::quote;
    quote.contract = 2;
    quote.sd = mm::side::ask;
    quote.internal_price = PX(101);
    quote.internal_size = 7;
    quote.external_price = PX(102);
    quote.external_size = 5;
    quote.persist = true;
    b.mm_sink->send_quote(quote);

    order_request request = b.peek(strategy_id::MM);
    check(request.strategy_id_value() == strategy_id::MM, "routed to the MM mailbox");
    check_eq(request.contract, 2, "contract carried over");
    check(request.side_value() == side::ask, "side mapped (mm::side -> exec::side)");
    check(request.kind_value() == request_kind::quote, "quote kind");
    check_eq(request.internal_price, PX(101), "internal price");
    check_eq(request.internal_size, 7, "internal size");
    check_eq(request.external_price, PX(102), "external price");
    check_eq(request.external_size, 5, "external size");
    check(request.persist(), "persist flag");

    mm::order_intent hedge;
    hedge.type = mm::order_intent::kind::hedge;
    hedge.contract = 3;
    hedge.sd = mm::side::bid;
    hedge.internal_price = PX(99);
    hedge.external_price = PX(98);
    hedge.size = 4;
    hedge.in_payup = true;
    b.mm_sink->send_hedge(hedge);

    request = b.peek(strategy_id::MM);
    check(request.kind_value() == request_kind::hedge, "hedge kind");
    check_eq(request.hedge_size, 4, "hedge size");
    check(request.pay_up(), "pay-up flag");
    check(request.external(), "MM hedges always go to the exchange");
    check_eq(request.contract, 3, "hedge contract");
}

void test_rv_ports() {
    std::printf("adapters: relative value\n");

    bench b;
    rv_adapter<executor> adapter(b.exec);
    const rv::host_ports ports = adapter.ports();
    check(ports.send_quote != nullptr && ports.send_hedge != nullptr, "ports are wired");

    rv::quote_request quote;
    quote.contract = 1;
    quote.side_ = rv::side::bid;
    quote.price = PX(100);
    quote.size = 6;
    quote.external_price = PX(101);
    quote.external_size = 4;
    quote.quote_external = true;
    ports.send_quote(ports.ctx, quote);

    order_request request = b.peek(strategy_id::RV);
    check(request.strategy_id_value() == strategy_id::RV, "routed to the RV mailbox");
    check_eq(request.internal_price, PX(100), "price");
    check_eq(request.internal_size, 6, "size");
    check_eq(request.external_price, PX(101), "external price");
    check(request.persist(), "quoteExternally becomes persist");

    rv::hedge_order hedge;
    hedge.contract = 1;
    hedge.side_ = rv::side::ask;
    hedge.price = PX(99);
    hedge.size = 3;
    hedge.external = true;
    hedge.hedge_id = 11;
    hedge.market_index = 2;
    ports.send_hedge(ports.ctx, hedge);

    request = b.peek(strategy_id::RV);
    check(request.kind_value() == request_kind::hedge, "hedge kind");
    check_eq(request.hedge_size, 3, "hedge size");
    check_eq(request.hedge_id, 11, "hedge id");
    check_eq(request.market_id, 2, "market id");
    check(request.external(), "external hedge");
    check(request.side_value() == side::ask, "side mapped");
}

}  // namespace

namespace {

void test_as_routes_the_hedge_contract() {
    std::printf("adapters: arbie spreader\n");

    bench b;
    as_adapter<executor> adapter(b.exec);
    const as::host_ports ports = adapter.ports();
    check(ports.send_quote != nullptr && ports.send_hedge != nullptr, "ports are wired");

    as::quote_request quote;
    quote.contract = 0;
    quote.sd = as::side::ask;
    quote.internal_price = PX(105);
    quote.internal_size = 9;
    quote.external_price = PX(106);
    quote.external_size = 9;
    quote.quote_external = false;
    quote.lean_on_rv = true;
    ports.send_quote(ports.ctx, quote);

    order_request request = b.peek(strategy_id::AS);
    check(request.strategy_id_value() == strategy_id::AS, "routed to the AS mailbox");
    check(request.lean_on_rv(), "leanOnRV carried (it changes what the engine publishes)");
    check(!request.persist(), "quoteExternally false");
    check_eq(request.internal_size, 9, "size");

    // AS hedges in a *different* contract than the one that filled: the request must be routed by
    // the hedge contract, or the residual would be sent for the wrong instrument.
    as::hedge_order hedge;
    hedge.contract = 0;
    hedge.hedge_contract = 3;
    hedge.sd = as::side::bid;
    hedge.internal_price = PX(200);
    hedge.external_price = PX(201);
    hedge.size = 5;
    hedge.external = true;
    hedge.hedge_id = 42;
    hedge.market_index = 1;
    ports.send_hedge(ports.ctx, hedge);

    request = b.peek(strategy_id::AS);
    check_eq(request.contract, 3, "the hedge is routed by hedge_contract");
    check_eq(request.hedge_size, 5, "hedge size");
    check_eq(request.external_price, PX(201), "hedge limit");
    check_eq(request.hedge_id, 42, "hedge id");
}

void test_cancel_and_alert_plumbing() {
    std::printf("adapters: cancel and alert plumbing\n");

    bench b;
    b.mm_sink->cancel_orders(INVALID_CONTRACT, true);
    check_eq(b.gateway.cancels.size(), 4, "a strategy-wide cancel reaches every contract");
    check(b.gateway.cancels[0].disable_gtc, "disableGTC is carried");

    b.gateway.clear();
    b.mm_sink->cancel_orders(2, false);
    check_eq(b.gateway.cancels.size(), 1, "a per-contract cancel reaches one contract");
    check_eq(b.gateway.cancels[0].contract, 2, "the right contract");
    check(!b.gateway.cancels[0].disable_gtc, "disableGTC false");

    b.mm_sink->alert(1, "test alert");
    check_eq(b.gateway.alerts.size(), 1, "the alert reached the gateway");
    check_eq(b.gateway.alerts[0].contract, 1, "with its contract");

    rv_adapter<executor> rv_ports_adapter(b.exec);
    const rv::host_ports rv_ports = rv_ports_adapter.ports();
    rv_ports.on_alert(rv_ports.ctx, "rv alert");
    check_eq(b.gateway.alerts.size(), 2, "the RV alert reached the gateway");

    as_adapter<executor> as_ports_adapter(b.exec);
    const as::host_ports as_ports = as_ports_adapter.ports();
    as_ports.cancel_all(as_ports.ctx);
    check_eq(b.gateway.cancels.size(), 1 + 4, "cancel_all cancels AS in every contract");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_mm_quote_and_hedge();
    test_rv_ports();
    test_as_routes_the_hedge_contract();
    test_cancel_and_alert_plumbing();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-4 checks passed\n");
    return EXIT_SUCCESS;
}
