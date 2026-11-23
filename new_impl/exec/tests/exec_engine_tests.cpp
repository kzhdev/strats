// Phase-2/3 gate for the new executor: the pipeline end to end.
//
//   exec_engine_tests [cycles]
//
// Every case drives the public surface only (mailboxes in, gateway + endpoints out), so what is
// asserted is the behaviour the strategies and the exchange see, not an internal detail:
//
//   * "pick the last request": N requests for one key produce exactly one book update and one
//     exchange order;
//   * the internal cross happens first, at the passive price chosen by the aggressor (:1143);
//   * only the remainder reaches the exchange (getPriceAndSize + synchronizeQuote, :835-852);
//   * fills are fanned out and closed with end_of_fills per strategy (:1310-1318);
//   * the internal best is published to RV/AS only, never a subscriber's own liquidity (:1249-1272).

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "executor.hpp"
#include "ports.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;

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

constexpr price_t PX(int64_t ticks) { return 25'000'000 * ticks; }  // 0.025 at 1e-9

struct fixture {
    explicit fixture(uint32_t contracts = 2) {
        // Cold-path pre-sizing: the recording sinks must not allocate inside the timed region,
        // otherwise the zero-allocation assertion would measure the test harness, not the engine.
        gateway.quotes.reserve(8192);
        gateway.hedges.reserve(8192);
        gateway.cancels.reserve(256);
        gateway.alerts.reserve(256);
        for (recording_endpoint* endpoint : {&mm, &rv, &as}) {
            endpoint->markets.reserve(8192);
            endpoint->fills.reserve(8192);
            endpoint->ends.reserve(1024);
            endpoint->as_done.reserve(256);
            endpoint->hedge_dones.reserve(256);
            endpoint->self_matches.reserve(256);
        }

        executor_config config;
        config.contracts = contracts;
        config.queue_capacity = 256;
        exec.init(config, &gateway);
        exec.register_strategy(strategy_id::MM, &mm);
        exec.register_strategy(strategy_id::RV, &rv);
        exec.register_strategy(strategy_id::AS, &as);
    }

    void cycle(uint64_t now = 1000) {
        exec.run_cycle(now);
    }

    executor exec;
    recording_gateway gateway;
    recording_endpoint mm{strategy_id::MM};
    recording_endpoint rv{strategy_id::RV};
    recording_endpoint as{strategy_id::AS};
};

price_t quote_price(const recording_gateway& gw, side sd, size_t index = 0) {
    size_t seen = 0;
    for (const exchange_order& order : gw.quotes) {
        if (order.sd != sd) continue;
        if (seen++ == index) return order.price;
    }
    return NULL_PRICE;
}

qty_t quote_size(const recording_gateway& gw, side sd, size_t index = 0) {
    size_t seen = 0;
    for (const exchange_order& order : gw.quotes) {
        if (order.sd != sd) continue;
        if (seen++ == index) return order.size;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// "Implied engine drains each queue and picks the last request."
// ---------------------------------------------------------------------------
void test_last_request_wins() {
    std::printf("engine: the last request per key wins\n");

    fixture f;
    for (int i = 0; i < 5; ++i) {
        f.exec.submit(strategy_id::MM,
                      make_quote(strategy_id::MM, 0, side::bid, PX(100 + i), 10 + i, PX(100 + i),
                                 10 + i, false));
    }
    f.cycle();

    check_eq(f.exec.engine(0).stats().staged, 5, "all five requests were staged");
    check_eq(f.exec.engine(0).stats().flushes, 1, "one flush for the contract");
    check_eq(f.exec.engine(0).book().find(strategy_id::MM, request_kind::quote, side::bid)->price,
             PX(104), "the book holds the last price");
    check_eq(f.exec.engine(0).book().find(strategy_id::MM, request_kind::quote, side::bid)->size, 14,
             "the book holds the last size");
    // One round of emissions per side, carrying the last price.
    check_eq(f.gateway.quotes.size(), 2, "one quote per side, not one per request");
    check_eq(quote_price(f.gateway, side::bid), PX(104), "the exchange sees the last price");
    check_eq(quote_size(f.gateway, side::bid), 14, "and the last size");
}

// A FIFO stream (AS quote, RV/AS hedge) is not collapsed: each request is applied in order.
void test_fifo_streams_are_not_collapsed() {
    std::printf("engine: FIFO streams are applied one by one\n");

    fixture f;
    for (int i = 0; i < 3; ++i) {
        f.exec.submit(strategy_id::AS,
                      make_quote(strategy_id::AS, 0, side::ask, PX(300 + i), 5, PX(300 + i), 5, false));
    }
    f.cycle();

    check_eq(f.exec.engine(0).stats().staged, 0, "no AS quote was staged");
    check_eq(f.exec.engine(0).stats().applied_now, 3, "all three were applied immediately");
    check_eq(f.exec.engine(0).book().find(strategy_id::AS, request_kind::quote, side::ask)->price,
             PX(302), "the book ends on the last AS price");
}


// ---------------------------------------------------------------------------
// Cross first, exchange second.
// ---------------------------------------------------------------------------
void test_internal_cross_then_residual() {
    std::printf("engine: cross internally, send the remainder out\n");

    fixture f;
    // MM bids 100 for 5; RV offers 99 for 3. RV arrived last, so it is the aggressor and the
    // passive price (the bid) is the fill price (:1143).
    f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::bid, PX(100), 5, PX(100), 5, false));
    f.exec.submit(strategy_id::RV, make_quote(strategy_id::RV, 0, side::ask, PX(99), 3, PX(99), 3, false));
    f.cycle();

    check_eq(f.mm.fills.size(), 1, "MM got one fill");
    check_eq(f.mm.fills[0].price, PX(100), "MM filled at its own (passive) bid price");
    check_eq(f.mm.fills[0].size, 3, "MM fill size");
    check(f.mm.fills[0].sd == side::bid, "MM fill is on the bid side");
    check_eq(f.rv.fills.size(), 1, "RV got one fill");
    check_eq(f.rv.fills[0].price, PX(100), "RV (aggressor) also filled at the passive price");
    check_eq(f.as.fills.size(), 0, "AS was not involved");
    check_eq(f.mm.ends.size(), 1, "end_of_fills closes the MM batch");
    check_eq(f.rv.ends.size(), 1, "end_of_fills closes the RV batch");

    check_eq(f.exec.engine(0).book().find(strategy_id::MM, request_kind::quote, side::bid)->size, 2,
             "MM has 2 left internally");
    check_eq(f.gateway.quotes.size(), 2, "one residual quote per side");
    check_eq(quote_price(f.gateway, side::bid), PX(100), "the residual bid keeps the price");
    check_eq(quote_size(f.gateway, side::bid), 2, "the residual bid keeps only the remainder");
    check(quote_price(f.gateway, side::ask) == NULL_PRICE, "the ask side leaves the market");
}

void test_self_match_aborts_the_cross() {
    std::printf("engine: a non-AS self match aborts\n");

    fixture f;
    // Both orders are MM: the internal book would self-cross.
    f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::bid, PX(100), 5, PX(100), 5, false));
    f.exec.submit(strategy_id::MM, make_hedge(strategy_id::MM, 0, side::ask, PX(99), PX(99), 5, true, false));
    f.cycle();

    check_eq(f.exec.engine(0).stats().self_matches, 1, "the self match is counted");
    check_eq(f.gateway.alerts.size(), 1, "one critical alert");
    check_eq(f.gateway.alerts[0].code, implied_engine::ALERT_SELF_MATCH, "self-match code");
    check_eq(f.mm.disables, 0, "a self match disables external quoting, not the strategy");
    check_eq(f.mm.self_matches.size(), 1, "the strategy was told to stop quoting externally");
    check_eq(f.mm.fills.size(), 0, "nothing was filled");
}

// ---------------------------------------------------------------------------
// FAK hedges: match what the book holds, send the rest out.
// ---------------------------------------------------------------------------
void test_fak_hedge_matches_then_sends_remainder() {
    std::printf("engine: FAK hedge fills internally then sends the remainder\n");

    fixture f;
    f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::ask, PX(99), 3, PX(99), 3, false));
    f.cycle();
    f.gateway.clear();
    f.mm.clear();
    f.rv.clear();

    // RV buys 5 at 100: 3 fill internally at 99, 2 go to the exchange.
    f.exec.submit(strategy_id::RV,
                  make_hedge(strategy_id::RV, 0, side::bid, PX(100), PX(100), 5, true, false, 77, 0));
    f.cycle();

    check_eq(f.rv.fills.size(), 1, "the aggressor gets one volume-weighted fill");
    check_eq(f.rv.fills[0].price, PX(99), "at the resting price");
    check_eq(f.rv.fills[0].size, 3, "for what the book held");
    check_eq(f.mm.fills.size(), 1, "the resting MM order was filled");
    check_eq(f.mm.fills[0].size, 3, "in full");
    check_eq(f.gateway.hedges.size(), 1, "exactly one hedge order goes out");
    check_eq(f.gateway.hedges[0].size, 2, "carrying only the unfilled remainder");
    check_eq(f.gateway.hedges[0].price, PX(100), "at the hedge limit price");
    check_eq(f.gateway.hedges[0].hedge_id, 77, "with the hedge id threaded through");
    check_eq(f.exec.engine(0).book().has_matchable(side::ask), 0, "the ask side is consumed");
}

void test_internal_only_hedge_miss_is_reported_back() {
    std::printf("engine: an internal-only hedge that misses is handed back\n");

    fixture f;
    f.exec.submit(strategy_id::RV,
                  make_hedge(strategy_id::RV, 0, side::bid, PX(100), PX(100), 5, false, false, 5, 0));
    f.cycle();

    check_eq(f.rv.fills.size(), 1, "the miss is reported to RV");
    check(f.rv.fills[0].missed, "flagged as a MissedHedge correction");
    check_eq(f.rv.fills[0].size, 5, "for the whole size");
    check(f.rv.fills[0].sd == side::ask, "on the other side, so RV can unwind");
    check_eq(f.rv.hedge_dones.size(), 1, "and the hedge is closed");
    check_eq(f.rv.hedge_dones[0].hedge_id, 5, "with its id");
    check_eq(f.gateway.hedges.size(), 0, "nothing is sent to the exchange");
}

// ---------------------------------------------------------------------------
// Gates
// ---------------------------------------------------------------------------
void test_stale_mm_request_is_dropped() {
    std::printf("engine: stale MM requests are dropped\n");

    fixture f;
    f.exec.set_last_mm_execution(0, 100);
    f.exec.submit(strategy_id::MM,
                  make_quote(strategy_id::MM, 0, side::bid, PX(100), 5, PX(100), 5, false, false, 0));
    f.cycle();

    check_eq(f.exec.engine(0).stats().stale_dropped, 1, "the stale request was dropped");
    check_eq(f.gateway.quotes.size(), 0, "and never reached the exchange");

    // The exception (:1981-1984): a hedge with no external price still has to be applied, because
    // it is the request that removes the IME's order.
    f.exec.submit(strategy_id::MM, make_hedge(strategy_id::MM, 0, side::bid, PX(90), NULL_PRICE, 1,
                                              false, false, 0, 0, 0));
    f.cycle();
    check_eq(f.exec.engine(0).stats().stale_dropped, 1, "the removing hedge was not dropped");
}

void test_disabled_strategy_request_is_dropped() {
    std::printf("engine: a disabled strategy's requests are dropped\n");

    fixture f;
    f.rv.disable();
    f.exec.submit(strategy_id::RV,
                  make_quote(strategy_id::RV, 0, side::bid, PX(100), 5, PX(100), 5, false));
    f.cycle();

    check_eq(f.exec.engine(0).stats().disabled_dropped, 1, "dropped");
    check_eq(f.gateway.quotes.size(), 0, "no emission");
}


// ---------------------------------------------------------------------------
// Publication and cancellation
// ---------------------------------------------------------------------------
void test_publish_rules() {
    std::printf("engine: internal market publication\n");

    fixture f;
    // MM offers 101 (no cross: 100 < 101), RV bids 100.
    f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::ask, PX(101), 7, PX(101), 7, false));
    f.exec.submit(strategy_id::RV, make_quote(strategy_id::RV, 0, side::bid, PX(100), 4, PX(100), 4, false));
    f.cycle();

    check_eq(f.mm.markets.size(), 0, "the MM is never an internal-market subscriber (:1276)");

    bool rv_saw_mm_ask = false;
    bool rv_saw_own_bid = false;
    for (const auto& event : f.rv.markets) {
        if (event.sd == side::ask && event.top.price == PX(101) && event.top.size == 7 &&
            event.top.source == liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE) {
            rv_saw_mm_ask = true;
        }
        if (event.sd == side::bid && event.top.price == PX(100)) rv_saw_own_bid = true;
    }
    check(rv_saw_mm_ask, "RV sees the MM ask together with its liquidity source");
    check(!rv_saw_own_bid, "RV never sees its own bid");

    bool as_saw_rv_bid = false;
    for (const auto& event : f.as.markets) {
        if (event.sd == side::bid && event.top.price == PX(100) &&
            event.top.source == liquidity_source::LIQUIDITY_SOURCE_RV_QUOTE) {
            as_saw_rv_bid = true;
        }
    }
    check(as_saw_rv_bid, "AS sees the RV bid");
}

void test_cancel_strategy() {
    std::printf("engine: cancel a strategy\n");

    fixture f;
    f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::bid, PX(100), 5, PX(100), 5, false));
    f.cycle();
    f.gateway.clear();

    f.exec.cancel_contract(0, strategy_id::MM, true);
    check_eq(f.gateway.cancels.size(), 1, "one cancel per contract");
    check(f.gateway.cancels[0].disable_gtc, "disableGTC is threaded through");
    check(f.gateway.cancels[0].strategy == strategy_id::MM, "for the right strategy");
    check_eq(f.exec.engine(0).book().has_matchable(side::bid), 0, "the internal book is empty");
    check(quote_price(f.gateway, side::bid) == NULL_PRICE, "the bid leaves the market");
}


// ---------------------------------------------------------------------------
// Contract routing and determinism
// ---------------------------------------------------------------------------
void test_contract_routing() {
    std::printf("executor: contracts are routed independently\n");

    fixture f;
    f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::bid, PX(100), 5, PX(100), 5, false));
    f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 1, side::bid, PX(200), 6, PX(200), 6, false));
    f.cycle();

    check_eq(f.exec.engine(0).book().find(strategy_id::MM, request_kind::quote, side::bid)->price,
             PX(100), "contract 0 keeps its price");
    check_eq(f.exec.engine(1).book().find(strategy_id::MM, request_kind::quote, side::bid)->price,
             PX(200), "contract 1 keeps its price");
    check_eq(f.exec.stats().contracts_flushed, 2, "both contracts were flushed");

    // A request for a contract the executor does not know is counted, not fatal.
    order_request stray = make_quote(strategy_id::MM, 99, side::bid, PX(100), 1, PX(100), 1, false);
    f.exec.submit(strategy_id::MM, stray);
    f.cycle();
    check_eq(f.exec.stats().dropped_bad_contract, 1, "the stray contract was dropped");
}

void test_determinism() {
    std::printf("executor: the same tape gives the same emissions\n");

    auto tape = [](fixture& f) {
        f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::bid, PX(100), 5, PX(100), 5, false));
        f.exec.submit(strategy_id::MM, make_quote(strategy_id::MM, 0, side::ask, PX(103), 5, PX(103), 5, false));
        f.cycle(10);
        f.exec.submit(strategy_id::RV, make_quote(strategy_id::RV, 0, side::ask, PX(99), 2, PX(99), 2, false));
        f.cycle(20);
        f.exec.submit(strategy_id::AS, make_quote(strategy_id::AS, 0, side::bid, PX(102), 4, PX(102), 4, false));
        f.cycle(30);
    };

    fixture a;
    fixture b;
    tape(a);
    tape(b);

    bool same = a.gateway.quotes.size() == b.gateway.quotes.size();
    for (size_t i = 0; same && i < a.gateway.quotes.size(); ++i) {
        same = a.gateway.quotes[i].price == b.gateway.quotes[i].price &&
               a.gateway.quotes[i].size == b.gateway.quotes[i].size &&
               a.gateway.quotes[i].sd == b.gateway.quotes[i].sd;
    }
    check(same, "the emitted sequence is identical");
    check_eq(a.mm.fills.size(), b.mm.fills.size(), "same MM fills");
    check_eq(a.as.fills.size(), b.as.fills.size(), "same AS fills");
}

}  // namespace

// ---------------------------------------------------------------------------
// The cycle must not allocate (PLAN.md §4). The probe is global, so it also catches an
// allocation inside the gateway or an endpoint.
// ---------------------------------------------------------------------------
namespace {
struct alloc_probe {
    static inline uint64_t allocations = 0;
    static inline bool counting = false;
};
}  // namespace

void* operator new(size_t size) {
    if (alloc_probe::counting) ++alloc_probe::allocations;
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {
void test_no_allocation_in_cycle() {
    std::printf("executor: the cycle performs no allocation\n");

    fixture f;
    // Warm up every path once so lazily created state is not counted.
    for (int i = 0; i < 4; ++i) {
        f.exec.submit(strategy_id::MM,
                      make_quote(strategy_id::MM, static_cast<uint32_t>(i % 2), side::bid, PX(100),
                                 5, PX(100), 5, false));
        f.exec.submit(strategy_id::RV,
                      make_hedge(strategy_id::RV, static_cast<uint32_t>(i % 2), side::bid, PX(101),
                                 PX(101), 2, true, false, static_cast<uint32_t>(i), 0));
        f.cycle(static_cast<uint64_t>(i));
    }

    alloc_probe::allocations = 0;
    alloc_probe::counting = true;
    for (int i = 0; i < 1000; ++i) {
        for (uint32_t c = 0; c < 2; ++c) {
            f.exec.submit(strategy_id::MM,
                          make_quote(strategy_id::MM, c, side::bid, PX(100 + (i % 3)), 5,
                                     PX(100 + (i % 3)), 5, false));
            f.exec.submit(strategy_id::AS,
                          make_quote(strategy_id::AS, c, side::ask, PX(120 + (i % 3)), 3,
                                     PX(120 + (i % 3)), 3, false));
            f.exec.submit(strategy_id::RV,
                          make_hedge(strategy_id::RV, c, side::bid, PX(101), PX(101), 2, true,
                                     false, static_cast<uint32_t>(i), 0));
        }
        f.cycle(static_cast<uint64_t>(1000 + i));
    }
    alloc_probe::counting = false;

    check_eq(static_cast<long long>(alloc_probe::allocations), 0, "no allocation inside run_cycle");
    check(f.exec.stats().requests_drained > 0, "the cycles actually did work");
}
}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_last_request_wins();
    test_fifo_streams_are_not_collapsed();
    test_internal_cross_then_residual();
    test_self_match_aborts_the_cross();
    test_fak_hedge_matches_then_sends_remainder();
    test_internal_only_hedge_miss_is_reported_back();
    test_stale_mm_request_is_dropped();
    test_disabled_strategy_request_is_dropped();
    test_publish_rules();
    test_cancel_strategy();
    test_contract_routing();
    test_determinism();
    test_no_allocation_in_cycle();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-2/3 checks passed\n");
    return EXIT_SUCCESS;
}
