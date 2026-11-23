// Phase-6 gate: the return path (feedback.hpp).
//
//   exec_feedback_tests [cycles]
//
// `strategy_endpoint` calls land on the engine thread, which is fine single-threaded and wrong with
// one thread per strategy. `strategy_link` moves them onto a per-strategy SPSC ring. This gate proves
// three things:
//
//   1. routed mode is *equivalent* to direct mode: the same request tape through two executors, one
//      with the real endpoint registered and one with `endpoint_via_link`, must produce the identical
//      event stream once the link is drained;
//   2. the enabled mirror is immediately visible to the engine -- a `disable()` posted on one side
//      stops routing on the other without waiting for a drain;
//   3. the ring reports overflow instead of corrupting when the strategy stops draining.
//
// The threaded case at the end runs the engine on one thread and the strategy's drain on another,
// which is the deployment shape the whole file exists for.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "executor.hpp"
#include "feedback.hpp"
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

constexpr price_t PX(int64_t ticks) { return 25'000'000 * ticks; }

// A recording endpoint that flattens everything it is told into one comparable string, so the direct
// and the linked executor can be diffed exactly.
class trace_endpoint final : public strategy_endpoint {
public:
    void on_internal_market(uint32_t contract, side sd, const internal_top& top) noexcept override {
        append("mkt", contract, static_cast<int>(sd), top.price, top.size);
    }
    void on_fill(const internal_fill& fill) noexcept override {
        append("fill", fill.contract, static_cast<int>(fill.sd), fill.price, fill.size);
        if (fill.missed) trace += " missed";
        trace += " k" + std::to_string(static_cast<int>(fill.kind));
    }
    void end_of_fills(uint32_t contract) noexcept override {
        trace += " eof" + std::to_string(contract);
    }
    void on_hedge_done(uint32_t contract, uint32_t hedge_id, uint32_t, side sd) noexcept override {
        trace += " hdone" + std::to_string(contract) + "/" + std::to_string(hedge_id) + "/" +
                 std::to_string(static_cast<int>(sd));
    }
    void on_as_match_done(uint32_t contract, qty_t bid_size, qty_t ask_size) noexcept override {
        trace += " asmatch" + std::to_string(contract) + "/" + std::to_string(bid_size) + "/" +
                 std::to_string(ask_size);
    }
    void on_self_match(uint32_t contract, const char*) noexcept override {
        trace += " self" + std::to_string(contract);
    }
    bool enabled() const noexcept override { return is_enabled; }
    void disable() noexcept override {
        is_enabled = false;
        trace += " disable";
    }

    void clear() { trace.clear(); }
    bool is_enabled = true;
    std::string trace;

private:
    void append(const char* what, uint32_t contract, int sd, price_t price, qty_t size) {
        trace += ' ';
        trace += what;
        trace += std::to_string(contract) + "/" + std::to_string(sd) + "/" + std::to_string(price) +
                 "/" + std::to_string(size);
    }
};

// One deterministic request tape, so the two executors see exactly the same traffic.
void run_tape(executor& exec, uint32_t cycles) {
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        const int64_t drift = cycle % 4;
        for (uint32_t c = 0; c < 3; ++c) {
            // MM quotes both sides, RV bids into it every few cycles (that crosses), AS offers wide.
            exec.submit(strategy_id::MM,
                        make_quote(strategy_id::MM, c, side::bid, PX(100 + drift), 5, PX(100 + drift),
                                   5, false));
            exec.submit(strategy_id::MM,
                        make_quote(strategy_id::MM, c, side::ask, PX(104 + drift), 5, PX(104 + drift),
                                   5, false));
            if (cycle % 3 == 0) {
                exec.submit(strategy_id::RV,
                            make_quote(strategy_id::RV, c, side::ask, PX(99 + drift), 3,
                                       PX(99 + drift), 3, false));
            }
            if (cycle % 5 == 0) {
                exec.submit(strategy_id::RV,
                            make_hedge(strategy_id::RV, c, side::bid, PX(101), PX(101), 2, true,
                                       false, static_cast<uint32_t>(cycle), 0));
            }
            exec.submit(strategy_id::AS,
                        make_quote(strategy_id::AS, c, side::ask, PX(110 + drift), 2, PX(110 + drift),
                                   2, false));
        }
        exec.run_cycle(cycle);
    }
}


}  // namespace

namespace {

// ---------------------------------------------------------------------------
// 1. routed mode == direct mode
// ---------------------------------------------------------------------------
void test_routed_equals_direct(uint32_t cycles) {
    std::printf("feedback: routed mode is equivalent to direct mode\n");

    executor_config config;
    config.contracts = 3;
    config.queue_capacity = 1024;

    // direct: the endpoints are registered on the executor and called on its thread.
    recording_gateway direct_gateway;
    trace_endpoint direct_mm;
    trace_endpoint direct_rv;
    trace_endpoint direct_as;
    executor direct;
    direct.init(config, &direct_gateway);
    direct.register_strategy(strategy_id::MM, &direct_mm);
    direct.register_strategy(strategy_id::RV, &direct_rv);
    direct.register_strategy(strategy_id::AS, &direct_as);
    run_tape(direct, cycles);

    // routed: `endpoint_via_link` is registered instead, and the events are replayed afterwards.
    recording_gateway routed_gateway;
    trace_endpoint routed_mm;
    trace_endpoint routed_rv;
    trace_endpoint routed_as;
    strategy_link<> link_mm;
    strategy_link<> link_rv;
    strategy_link<> link_as;
    link_mm.init(4096);
    link_rv.init(4096);
    link_as.init(4096);
    endpoint_via_link<> routed_endpoint_mm(link_mm);
    endpoint_via_link<> routed_endpoint_rv(link_rv);
    endpoint_via_link<> routed_endpoint_as(link_as);
    executor routed;
    routed.init(config, &routed_gateway);
    routed.register_strategy(strategy_id::MM, &routed_endpoint_mm);
    routed.register_strategy(strategy_id::RV, &routed_endpoint_rv);
    routed.register_strategy(strategy_id::AS, &routed_endpoint_as);
    run_tape(routed, cycles);
    link_dispatch dispatch_mm(routed_mm);
    link_dispatch dispatch_rv(routed_rv);
    link_dispatch dispatch_as(routed_as);
    link_mm.drain(dispatch_mm);
    link_rv.drain(dispatch_rv);
    link_as.drain(dispatch_as);

    check(!direct_mm.trace.empty(), "the tape produced events at all");
    check(direct_mm.trace == routed_mm.trace, "MM event stream identical (direct vs routed)");
    check(direct_rv.trace == routed_rv.trace, "RV event stream identical (direct vs routed)");
    check(direct_as.trace == routed_as.trace, "AS event stream identical (direct vs routed)");
    check_eq(static_cast<long long>(routed_endpoint_mm.overflowed() +
                                    routed_endpoint_rv.overflowed() +
                                    routed_endpoint_as.overflowed()),
             0, "nothing overflowed the feedback rings");
    check_eq(static_cast<long long>(direct_gateway.quotes.size()),
             static_cast<long long>(routed_gateway.quotes.size()),
             "the exchange saw the same residual quotes either way");
    std::printf("  %-30s %8zu events (MM)\n", "stream length", direct_mm.trace.size());
}

// ---------------------------------------------------------------------------
// 2. the enabled mirror
// ---------------------------------------------------------------------------
void test_enabled_mirror() {
    std::printf("feedback: the enabled mirror is immediate\n");

    strategy_link<> link;
    link.init(64);
    endpoint_via_link<> routed(link);
    trace_endpoint real;
    check(routed.enabled(), "a fresh link is enabled");

    routed.disable();
    check(!link.enabled(), "disable() flips the mirror at once, before any drain");
    link_dispatch dispatch(real);
    link.drain(dispatch);
    check(!real.is_enabled, "and the strategy sees the disable when it drains");

    link.set_enabled(true);  // the strategy re-enables itself on its own thread
    check(routed.enabled(), "the strategy can re-enable through the mirror");
}

// ---------------------------------------------------------------------------
// 3. overflow is reported, not hidden
// ---------------------------------------------------------------------------
void test_overflow_is_reported() {
    std::printf("feedback: a stalled drain overflows visibly\n");

    strategy_link<> link;
    link.init(4);
    endpoint_via_link<> routed(link);
    trace_endpoint real;
    link_dispatch dispatch(real);

    internal_fill fill;
    fill.contract = 0;
    fill.price = PX(100);
    fill.size = 1;
    for (int i = 0; i < 10; ++i) routed.on_fill(fill);
    check_eq(static_cast<long long>(routed.overflowed()), 6, "the events that did not fit are counted");

    const uint32_t drained = link.drain(dispatch);
    check_eq(static_cast<long long>(drained), 4, "the ring kept exactly its capacity");
}

// ---------------------------------------------------------------------------
// 4. the threaded shape: engine on one thread, drain on another
// ---------------------------------------------------------------------------
void test_threaded_drain(uint32_t cycles) {
    std::printf("feedback: engine thread posts, strategy thread drains\n");

    executor_config config;
    config.contracts = 2;
    config.queue_capacity = 512;

    recording_gateway gateway;
    strategy_link<sync_mode::spsc> link;
    link.init(8192);
    endpoint_via_link<sync_mode::spsc> routed(link);
    trace_endpoint real;
    link_dispatch dispatch(real);

    basic_executor<sync_mode::spsc> exec;
    exec.init(config, &gateway);
    // RV is the subscriber here: the MM is never published an internal market (asserted in
    // InternalMatchingEngine.h:1276), so it would post nothing to drain.
    exec.register_strategy(strategy_id::RV, &routed);

    std::atomic<bool> engine_done{false};
    std::atomic<uint64_t> drained{0};
    std::thread strategy([&] {
        for (;;) {
            drained += link.drain(dispatch);
            if (engine_done.load(std::memory_order_acquire) && link.dropped() == 0) {
                // one last drain after the flag, then stop
                drained += link.drain(dispatch);
                break;
            }
        }
    });

    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        const int64_t drift = cycle % 4;
        for (uint32_t c = 0; c < 2; ++c) {
            exec.submit(strategy_id::MM, make_quote(strategy_id::MM, c, side::bid, PX(100 + drift), 5,
                                                    PX(100 + drift), 5, false));
            exec.submit(strategy_id::MM, make_quote(strategy_id::MM, c, side::ask, PX(103 + drift), 5,
                                                    PX(103 + drift), 5, false));
            exec.submit(strategy_id::RV, make_quote(strategy_id::RV, c, side::bid, PX(99 + drift), 2,
                                                    PX(99 + drift), 2, false));
        }
        exec.run_cycle(cycle);
    }
    engine_done.store(true, std::memory_order_release);
    strategy.join();

    uint64_t produced = 0;
    for (uint32_t c = 0; c < 2; ++c) produced += exec.engine(c).stats().publishes;
    check_eq(static_cast<long long>(routed.overflowed()), 0, "nothing overflowed while draining");
    check(drained.load() > 0, "the strategy thread received events");
    check_eq(static_cast<long long>(drained.load()), static_cast<long long>(produced),
             "every posted event was drained (publishes are the only source here)");
}

}  // namespace

void* operator new(size_t size) {
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    const uint32_t cycles = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 500;

    test_routed_equals_direct(cycles);
    test_enabled_mirror();
    test_overflow_is_reported();
    test_threaded_drain(cycles);

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-6 checks passed\n");
    return EXIT_SUCCESS;
}
