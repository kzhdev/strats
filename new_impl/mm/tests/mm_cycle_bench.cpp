// Cycle-level benchmark for the new MarketMaker engine (PLAN.md §7).
//
//   mm_cycle_bench [cycles] [runs] [contracts]
//
// Reports ns and cycles per cycle and per stage, plus a zero-allocation assertion for the timed
// region (the hard invariant). Latency budgets are advisory: they assume an unloaded
// production-class CPU.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "book.hpp"
#include "host.hpp"
#include "model.hpp"
#include "quoting.hpp"
#include "state.hpp"
#include "strategy.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade;
using namespace tachyon::trade::mm;
using Clock = std::chrono::steady_clock;

volatile double g_sink = 0.0;

struct alloc_probe {
    static inline uint64_t allocations = 0;
    static inline bool counting = false;
};

double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

constexpr price_t TICK = 25'000'000;  // 0.025

contract_input make_input(uint32_t index) {
    contract_input c;
    c.name = "BENCH";
    c.tick_size = TICK;
    c.lot = 1;
    c.enabled = true;
    c.quote_external = true;
    c.quote_size = 8;
    c.quote_gap = 5;
    c.pct_tob_max_quote = 0.2;
    c.width_pct = {3.0, 2.0, 0.1, 0.1};
    c.hedge_offset = 1.0;
    c.join_size = 0;
    c.leave_size = 2;
    c.payup_qty = 1;
    c.payup_orders = 1;
    c.payup_multiple = 0.01;
    c.max_pos_internal = 1500;
    c.max_pos_external = 350;
    c.commission = 0.00001;
    c.min_internal_price_change = 0.0001;
    c.est_level_ema = 3.0;
    c.est_level_ema_seconds = 5000;
    c.est_level_pct_ema = 0.8;
    c.max_ticks_wide = 20;
    c.worsen_payup_pct = 0.65477777777;
    c.queue_importance = 3;  // the production value: exercises the S-curve every cycle
    (void)index;
    return c;
}

model make_model(uint32_t contracts) {
    model_inputs in;
    for (uint32_t i = 0; i < contracts; ++i) in.contracts.push_back(make_input(i));
    in.cycles_per_second = 1.0e6;
    in.current_shift = 2;
    return model::build(in);
}

void noop_quote(void*, const order_intent&) {}
void noop_hedge(void*, const order_intent&) {}
void noop_notify(void*, uint32_t) {}
void noop_cancel(void*, uint32_t, bool) {}
void noop_alert(void*, uint32_t, const char*) {}
void noop_fill(void*, uint32_t, const order_intent*, const fill_outcome&) {}

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
    const uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
    const int runs = argc > 2 ? std::atoi(argv[2]) : 7;
    const uint32_t contracts = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 8;

    const model m = make_model(contracts);
    cycle_state st(m);
    host_ports ports;
    ports.send_quote = &noop_quote;
    ports.send_hedge = &noop_hedge;
    ports.notify = &noop_notify;
    ports.cancel_orders = &noop_cancel;
    ports.on_alert = &noop_alert;
    ports.on_fill_report = &noop_fill;
    host h(m, st, ports);
    h.set_strategy_enabled(true);

    // Prime every contract with a two-tick-wide book and let it quote once.
    level bid{};
    level ask{};
    uint64_t batch = 1;
    for (uint32_t c = 0; c < contracts; ++c) {
        st.contract(c).contract_can_trade = true;
        bid = level{TICK * 2000, 1000, 5, 0};
        ask = level{TICK * 2002, 1000, 5, 0};
        h.on_market_data(c, side::bid, &bid, 1, 1, 1, batch);
        h.on_market_data(c, side::ask, &ask, 1, 1, 1, batch++);
    }
    h.run_cycle(1000);

    // ---- full cycle through the host (the production path) ----
    std::vector<double> full_ns;
    uint64_t allocations = 0;
    for (int r = 0; r < runs; ++r) {
        alloc_probe::allocations = 0;
        alloc_probe::counting = true;
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < cycles; ++i) {
            const uint32_t c = static_cast<uint32_t>(i % contracts);
            const int drift = static_cast<int>(i % 7) - 3;
            bid = level{TICK * (2000 + drift), 900 + static_cast<qty_t>(i % 200), 5, 0};
            ask = level{TICK * (2002 + drift), 900 + static_cast<qty_t>(i % 200), 5, 0};
            h.on_market_data(c, side::bid, &bid, 1, 1, 1, batch);
            h.on_market_data(c, side::ask, &ask, 1, 1, 1, batch++);
            h.run_cycle(1000 * (i + 2));
            g_sink = g_sink + static_cast<double>(st.contract(c).mid_point);
        }
        alloc_probe::counting = false;
        const auto t1 = Clock::now();
        allocations += alloc_probe::allocations;
        full_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / cycles);
    }

    // ---- stage isolation: the pricing block only ----
    quote_engine eng(m);
    std::vector<double> pricing_ns;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < cycles; ++i) {
            const uint32_t c = static_cast<uint32_t>(i % contracts);
            contract_state& s = st.contract(c);
            const int drift = static_cast<int>(i % 7) - 3;
            level b{TICK * (2000 + drift), 900 + static_cast<qty_t>(i % 200), 5, 0};
            level a{TICK * (2002 + drift), 900 + static_cast<qty_t>(i % 200), 5, 0};
            s.side_book(side::bid).apply_snapshot(&b, 1);
            s.side_book(side::ask).apply_snapshot(&a, 1);
            eng.refresh_market(s, c, 1000 * (i + 2));
            g_sink = g_sink + static_cast<double>(s.mid_point);
        }
        const auto t1 = Clock::now();
        pricing_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / cycles);
    }

    // ---- stage isolation: quotes + hedges + cross checks, no host ----
    intent_log log;
    std::vector<double> cycle_ns;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < cycles; ++i) {
            const uint32_t c = static_cast<uint32_t>(i % contracts);
            log.clear();
            h.engine().on_md_change(st.contract(c), st.queue(c), c, true, true, log);
            g_sink = g_sink + static_cast<double>(log.size());
        }
        const auto t1 = Clock::now();
        cycle_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / cycles);
    }

    const double full = median(full_ns);
    const double pricing = median(pricing_ns);
    const double cyc = median(cycle_ns);
    const double ghz = 3.0;

    std::printf("MM engine: %u contracts, %llu cycles x %d runs\n", contracts,
                (unsigned long long)cycles, runs);
    std::printf("  %-24s %10.1f ns  %8.0f cycles\n", "full host cycle", full, full * ghz);
    std::printf("  %-24s %10.1f ns  %8.0f cycles\n", "pricing block only", pricing, pricing * ghz);
    std::printf("  %-24s %10.1f ns  %8.0f cycles\n", "quote+hedge+cross only", cyc, cyc * ghz);
    std::printf("  %-24s %10llu per cycle\n", "allocations",
                (unsigned long long)(allocations / (cycles * static_cast<uint64_t>(runs))));
    std::printf("  %-24s %10llu\n", "intents emitted",
                (unsigned long long)h.stats().intents_emitted);

    bool ok = true;
    if (allocations != 0) {
        std::printf("FAIL: %llu allocations in the timed region\n", (unsigned long long)allocations);
        ok = false;
    }
    std::printf("  budget (advisory): full cycle <= 1000 ns -> %s\n",
                full <= 1000.0 ? "PASS" : "miss on this machine");
    std::printf("\n(sink=%g)\n", (double)g_sink);
    return ok ? 0 : 1;
}
