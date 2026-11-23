// Cycle-level benchmark for the new executor (PLAN.md §4).
//
//   exec_bench [cycles] [runs] [contracts] [requests-per-contract]
//
// Reports ns per stage plus a zero-allocation assertion for the timed region (the hard
// invariant). Latency budgets in the output are advisory: they assume an unloaded
// production-class CPU, and the point of the numbers is the ratio between the stages, i.e. where
// the cycle's time actually goes.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "executor.hpp"
#include "ports.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;
using Clock = std::chrono::steady_clock;

// A sink the compiler cannot see through, so the measured work is never optimised away. Written
// through a helper because `volatile +=` is deprecated in C++20.
volatile double g_sink = 0.0;

inline void sink_add(double value) noexcept {
    g_sink = g_sink + value;
}

struct alloc_probe {
    static inline uint64_t allocations = 0;
    static inline bool counting = false;
};

double median(std::vector<double>& values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

constexpr price_t PX(int64_t ticks) { return 25'000'000 * ticks; }

// Silent ports: the benchmark measures the engine, so nothing here may allocate or format.
class null_gateway final : public exchange_gateway {
public:
    void send_quote(const exchange_order& order) noexcept override {
        ++quotes;
        sink_add(static_cast<double>(order.size));
    }
    void send_hedge(const exchange_order& order) noexcept override {
        ++hedges;
        sink_add(static_cast<double>(order.size));
    }
    void cancel(uint32_t, strategy_id, bool) noexcept override {}
    void alert(uint32_t, const char*, int32_t) noexcept override { ++alerts; }
    uint64_t quotes = 0;
    uint64_t hedges = 0;
    uint64_t alerts = 0;
};

class null_endpoint final : public strategy_endpoint {
public:
    void on_internal_market(uint32_t, side, const internal_top& top) noexcept override {
        sink_add(static_cast<double>(top.size));
    }
    void on_fill(const internal_fill& fill) noexcept override {
        sink_add(static_cast<double>(fill.size));
    }
    void end_of_fills(uint32_t) noexcept override {}
    void on_as_match_done(uint32_t, qty_t, qty_t) noexcept override {}
    void on_hedge_done(uint32_t, uint32_t, uint32_t, side) noexcept override {}
    void on_self_match(uint32_t, const char*) noexcept override {}
    bool enabled() const noexcept override { return true; }
    void disable() noexcept override {}
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

int main(int argc, char** argv) {
    const uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
    const int runs = argc > 2 ? std::atoi(argv[2]) : 5;
    const uint32_t contracts = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 8;
    const uint32_t per_contract = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 4;

    null_gateway gateway;
    null_endpoint mm_endpoint;
    null_endpoint rv_endpoint;
    null_endpoint as_endpoint;

    executor_config config;
    config.contracts = contracts;
    config.queue_capacity = 4096;

    executor exec;
    exec.init(config, &gateway);
    exec.register_strategy(strategy_id::MM, &mm_endpoint);
    exec.register_strategy(strategy_id::RV, &rv_endpoint);
    exec.register_strategy(strategy_id::AS, &as_endpoint);

    // ---- stage 1: push cost -------------------------------------------------
    std::vector<double> push_ns;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < cycles; ++i) {
            const uint32_t c = static_cast<uint32_t>(i % contracts);
            order_request* slot = exec.mailbox(strategy_id::MM).reserve();
            if (slot != nullptr) {
                *slot = make_quote(strategy_id::MM, c, side::bid, PX(100), 5, PX(100), 5, false);
                exec.mailbox(strategy_id::MM).publish();
            }
            sink_add(static_cast<double>(exec.mailbox(strategy_id::MM).size()));
        }
        const auto t1 = Clock::now();
        push_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / cycles);
        exec.run_cycle(1);  // drain so the next run starts empty
    }

    // ---- stage 2: full cycle (drain + route + flush + fan out), per request --
    std::vector<double> cycle_ns;
    for (int r = 0; r < runs; ++r) {
        for (uint64_t i = 0; i < per_contract * contracts; ++i) {
            const uint32_t c = static_cast<uint32_t>(i % contracts);
            exec.submit(strategy_id::MM,
                        make_quote(strategy_id::MM, c, side::bid, PX(100), 5, PX(100), 5, false));
            exec.submit(strategy_id::MM,
                        make_quote(strategy_id::MM, c, side::ask, PX(104), 5, PX(104), 5, false));
            exec.submit(strategy_id::RV,
                        make_quote(strategy_id::RV, c, side::bid, PX(98), 3, PX(98), 3, false));
            exec.submit(strategy_id::AS,
                        make_quote(strategy_id::AS, c, side::ask, PX(106), 2, PX(106), 2, false));
        }
        const auto t0 = Clock::now();
        exec.run_cycle(static_cast<uint64_t>(1000 + r));
        const auto t1 = Clock::now();
        // Four requests per (i) per contract are pushed above, so divide by the real count.
        const double requests = static_cast<double>(4 * per_contract * contracts);
        cycle_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / requests);
    }

    // ---- stage 3: a crossing cycle (the expensive path) ---------------------
    std::vector<double> cross_ns;
    for (int r = 0; r < runs; ++r) {
        order_request* slot = exec.mailbox(strategy_id::MM).reserve();
        if (slot != nullptr) {
            *slot = make_quote(strategy_id::MM, 0, side::bid, PX(100), 5, PX(100), 5, false);
            exec.mailbox(strategy_id::MM).publish();
        }
        slot = exec.mailbox(strategy_id::RV).reserve();
        if (slot != nullptr) {
            *slot = make_quote(strategy_id::RV, 0, side::ask, PX(99), 3, PX(99), 3, false);
            exec.mailbox(strategy_id::RV).publish();
        }
        const auto t0 = Clock::now();
        exec.run_cycle(static_cast<uint64_t>(2000 + r));
        const auto t1 = Clock::now();
        cross_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    // ---- zero-allocation check --------------------------------------------
    alloc_probe::allocations = 0;
    alloc_probe::counting = true;
    for (uint64_t i = 0; i < cycles; ++i) {
        const uint32_t c = static_cast<uint32_t>(i % contracts);
        exec.submit(strategy_id::MM,
                    make_quote(strategy_id::MM, c, side::bid, PX(100), 5, PX(100), 5, false));
        exec.submit(strategy_id::AS,
                    make_quote(strategy_id::AS, c, side::ask, PX(105), 2, PX(105), 2, false));
        exec.run_cycle(i);
    }
    alloc_probe::counting = false;
    const uint64_t allocations = alloc_probe::allocations;

    const double push = median(push_ns);
    const double cycle = median(cycle_ns);
    const double crossing = median(cross_ns);
    const double ghz = 3.0;

    std::printf("exec engine: %u contracts, %llu cycles x %d runs, %u requests/contract/cycle\n",
                contracts, (unsigned long long)cycles, runs, per_contract);
    std::printf("  %-30s %10.1f ns  %8.0f cycles\n", "mailbox push", push, push * ghz);
    std::printf("  %-30s %10.1f ns  %8.0f cycles\n", "cycle per request", cycle, cycle * ghz);
    std::printf("  %-30s %10.1f ns  %8.0f cycles\n", "crossing cycle (2 requests)", crossing,
                crossing * ghz);
    std::printf("  %-30s %10llu\n", "allocations in cycles", (unsigned long long)allocations);
    std::printf("  %-30s %10llu quotes, %llu hedges\n", "emitted",
                (unsigned long long)gateway.quotes, (unsigned long long)gateway.hedges);
    std::printf("  %-30s %10llu\n", "requests drained",
                (unsigned long long)exec.stats().requests_drained);

    bool ok = true;
    if (allocations != 0) {
        std::printf("FAIL: %llu allocations inside the cycle\n", (unsigned long long)allocations);
        ok = false;
    }
    // Advisory budgets, calibrated on the machine the numbers in PLAN.md §4 were measured on.
    std::printf("  budget (advisory): push <= 10 ns -> %s, cycle per request <= 150 ns -> %s\n",
                push <= 10.0 ? "PASS" : "miss on this machine",
                cycle <= 150.0 ? "PASS" : "miss on this machine");
    std::printf("\n(sink=%g)\n", (double)g_sink);
    return ok ? 0 : 1;
}
