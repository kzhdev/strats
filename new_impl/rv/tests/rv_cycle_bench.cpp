// Cycle-level benchmark for the new RV engine (PLAN.md §7.1/§7.2).
//
//   rv_cycle_bench [cycles] [runs]
//
// Reports ns and cycles per full cycle and per stage, plus a zero-allocation
// assertion for the timed region.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "config.hpp"
#include "model.hpp"
#include "state.hpp"
#include "valuation.hpp"
#include "quotes.hpp"
#include "strategy.hpp"

namespace {

using namespace tachyon::trade::rv;
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

// 4 vectors / 4 pricing / 10 contracts (config/backtest shape)
model_inputs make_inputs() {
    model_inputs in;
    in.n_pricing = 4;
    in.n_vectors = 4;
    in.n_contracts = 10;
    in.first_traded = 4;
    const size_t nn = static_cast<size_t>(in.n_contracts) * in.n_contracts;

    const double p[4][4] = {{1.0, 0.02, -0.02, 0.0},
                            {0.02, 1.0, 0.02, -0.02},
                            {-0.02, 0.02, 1.0, 0.02},
                            {0.0, -0.02, 0.02, 1.0}};
    in.pricing_matrix.resize(16);
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c) in.pricing_matrix[r * 4 + c] = p[r][c];

    in.contracts.resize(in.n_contracts);
    for (uint32_t s = 0; s < 6; ++s) {
        const uint32_t begin = static_cast<uint32_t>(in.legs.size());
        in.legs.push_back({s % 4, 1.0});
        in.legs.push_back({(s + 1) % 4, -1.0});
        in.contracts[4 + s].leg_begin = begin;
        in.contracts[4 + s].leg_count = 2;
    }
    in.hedge_ratio.assign(nn, 1.0);
    in.quote_size.assign(nn, 7.0);
    in.commissions.assign(nn, 34'000);
    in.leans.assign(nn, 1);
    in.factor_hedged_index.assign(nn, 0);
    in.rhone_max_clip.assign(in.n_contracts, 100);
    for (uint32_t c = 0; c < in.n_contracts; ++c) {
        contract_input& ci = in.contracts[c];
        ci.tick_size = to_scaled(0.25);
        ci.width = to_scaled(0.008);
        ci.established_level = 1000;
        ci.weight = c < 4 ? 1.0 : 0.5;
        ci.traded = c >= 4;  // the 4 pricing outrights are inputs, not quotes
        ci.quote_external = (c % 2) == 0;
    }
    for (uint32_t v = 0; v < 4; ++v) {
        vector_input vi;
        vi.range = 0.3 + 0.1 * v;
        vi.max_position = 1000.0;
        vi.use_ema = (v == 1);
        vi.ema_period_cycles = 1'000'000;
        vi.set_to_market = (v == 2);
        vi.factor_hedged = true;
        in.vectors.push_back(vi);
    }
    return in;
}

}  // namespace

void* operator new(size_t size) {
    if (alloc_probe::counting) {
        ++alloc_probe::allocations;
    }
    void* p = std::malloc(size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }


int main(int argc, char** argv) {
    const uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
    const int runs = argc > 2 ? std::atoi(argv[2]) : 7;

    const model_inputs inputs = make_inputs();
    const model m = model::build(inputs);
    cycle_state s{};
    s.init(m);
    quote_policy policy{};
    order_port port{};
    strategy strat(m, s, policy, port);
    // The two isolated benchmarks drive the collaborators directly (no strategy overhead).
    const valuation fairs(m);
    const quote_engine quote_calc(m, policy);

    std::vector<price_t> bid(m.n_pricing), ask(m.n_pricing);
    for (uint32_t c = 0; c < m.n_pricing; ++c) {
        bid[c] = to_scaled(4.0) + static_cast<price_t>(c) * to_scaled(0.25);
        ask[c] = bid[c] + to_scaled(0.25);
        strat.on_market_data(c, side::bid, true, bid[c], 25);
        strat.on_market_data(c, side::ask, true, ask[c], 25);
    }
    strat.run_cycle(1000);

    std::vector<double> full_ns;
    uint64_t allocations = 0;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        alloc_probe::allocations = 0;
        alloc_probe::counting = true;
        for (uint64_t i = 0; i < cycles; ++i) {
            const price_t drift = static_cast<price_t>((i % 5) - 2) * to_scaled(0.25);
            const uint32_t c = static_cast<uint32_t>(i % m.n_pricing);
            strat.on_market_data(c, side::bid, true, bid[c] + drift, 25);
            strat.run_cycle(1000 * (i + 2));
            g_sink = g_sink + static_cast<double>(s.stf[0]);
        }
        alloc_probe::counting = false;
        const auto t1 = Clock::now();
        allocations = alloc_probe::allocations;
        full_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / cycles);
    }

    std::vector<double> pipeline_ns;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < cycles; ++i) {
            fairs.recompute(s, 1000 * (i + 2));
            g_sink = g_sink + s.stf[0];
        }
        const auto t1 = Clock::now();
        pipeline_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / cycles);
    }

    std::vector<double> quote_ns;
    for (int r = 0; r < runs; ++r) {
        const auto t0 = Clock::now();
        for (uint64_t i = 0; i < cycles; ++i) {
            quote_calc.build_all(s);
            g_sink = g_sink + static_cast<double>(s.quote_of(4, side::bid).price);
        }
        const auto t1 = Clock::now();
        quote_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / cycles);
    }

    const double full = median(full_ns);
    const double pipeline = median(pipeline_ns);
    const double quotes = median(quote_ns);
    const double ghz = 3.0;  // ns -> cycles at a nominal 3 GHz

    // ---- model init: parse -> derive -> build (real config, PLAN §7.1) ----
    std::vector<double> init_ms;
    bool init_ok = false;
    {
        const secdb_table secdb = load_secdb("config/backtest/secdb.cme.csv");
        for (int r = 0; r < std::max(1, runs / 2); ++r) {
            const auto t0 = Clock::now();
            const rv_model_cfg cfg = rv_model_cfg::load("config/backtest/model.csv");
            const model_inputs inputs =
                build_model_inputs(cfg, secdb, cfg.width_multiplier(), 1.0);
            const model live = model::build(inputs);
            const auto t1 = Clock::now();
            init_ok = live.n_all == 10;
            init_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    }

    std::printf("RV engine: %u vectors / %u pricing / %u contracts, %llu cycles x %d runs\n",
                m.n_vectors, m.n_pricing, m.n_all, (unsigned long long)cycles, runs);
    std::printf("  %-20s %10.1f ns  %8.0f cycles\n", "full cycle", full, full * ghz);
    std::printf("  %-20s %10.1f ns  %8.0f cycles\n", "  pricing pipeline", pipeline, pipeline * ghz);
    std::printf("  %-20s %10.1f ns  %8.0f cycles\n", "  quote stage", quotes, quotes * ghz);
    std::printf("  %-20s %10llu per cycle\n", "allocations",
                (unsigned long long)(allocations / (cycles * runs)));
    std::printf("  %-20s %10u\n", "lean-sides/cycle", m.lean_count() * 2 * (m.n_all - m.first_traded));
    std::printf("  %-20s %10.3f ms  (%s)\n", "model init (real cfg)", median(init_ms),
                init_ok ? "config -> model" : "FAILED");
    std::printf("\n(sink=%g)\n", (double)g_sink);

    bool ok = true;
    if (allocations != 0) {  // hard invariant
        std::printf("FAIL: %llu allocations in the cycle\n", (unsigned long long)allocations);
        ok = false;
    }
    // Budgets are advisory: they assume an unloaded production-class CPU. This box
    // measured 6-19x inflation on unrelated microbenchmarks, so only the allocation
    // invariant is a hard failure here.
    std::printf("  budget (advisory, PLAN 7.2): full cycle <= 1000 ns -> %s\n",
                full <= 1000.0 ? "PASS" : "miss on this machine");
    return ok ? 0 : 1;
}
