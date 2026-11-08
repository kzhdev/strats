// Cycle-level benchmark for the new AS engine (PLAN §7.1/§7.2).
//
//   as_cycle_bench [cycles] [runs]
//
// Reports ns and cycles per stage plus the zero-allocation assertion, and measures the A/B
// decisions the plan owes an answer on:
//
//   * the incremental path update vs a forced full recompute (PLAN D4 / §4.3),
//   * the ranking: insertion sort vs the legacy std::sort (PLAN D5; both are bit-identical, which
//     the P2 gate proves) -- the arm is chosen at compile time,
//   * the host cycle with the ledger attached, i.e. the deployment shape.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "config.hpp"
#include "host.hpp"
#include "model.hpp"
#include "paths.hpp"
#include "policy.hpp"
#include "quotes.hpp"
#include "state.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::as;
using Clock = std::chrono::steady_clock;

volatile int64_t g_sink = 0;

struct alloc_probe {
    static inline uint64_t allocations = 0;
    static inline bool counting = false;
};

price_t px(double dollars) { return to_scaled(dollars); }

double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename Fn>
double time_ns(uint64_t iterations, Fn&& fn) {
    const auto begin = Clock::now();
    for (uint64_t i = 0; i < iterations; ++i) fn(i);
    const auto end = Clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count() /
           static_cast<double>(iterations);
}

model real_model() {
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    return model::build(build_model_inputs(cfg));
}

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

struct bench_ports {
    uint64_t quotes = 0;
    uint64_t hedges = 0;
};

void port_quote(void* ctx, const quote_request&) { ++static_cast<bench_ports*>(ctx)->quotes; }
void port_notify(void*, uint32_t) {}
void port_hedge(void* ctx, const hedge_order&) { ++static_cast<bench_ports*>(ctx)->hedges; }
void port_fill(void*, uint32_t, side, qty_t, price_t) {}
void port_switch(void*, const hedge_plan&) {}
void port_forward(void*, uint32_t, side, qty_t) {}
void port_alert(void*, const char*) {}
void port_cancel(void*) {}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
    const int runs = argc > 2 ? std::atoi(argv[2]) : 5;

    const model m = real_model();
    cycle_state s(m);
    const quote_policy policy{};
    path_engine paths(m, policy);
    quote_engine quotes(m, paths, policy);

    // a coherent starting book: every contract two-sided around a plausible fair value
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const price_t base = m.tick[c] * 2400;
        s.update_internal(c, side::bid, base - m.tick[c], 10);
        s.update_external(policy, c, side::bid, base - m.tick[c], 12);
        s.refresh_best(m, policy, c, side::bid, true);
        s.update_internal(c, side::ask, base + m.tick[c], 10);
        s.update_external(policy, c, side::ask, base + m.tick[c], 12);
        s.refresh_best(m, policy, c, side::ask, true);
        if (m.is_quoting(c)) paths.update_paths(s, c, true);
    }
    const uint32_t quoted = 6;  // QMQ2-QMV2, the enabled contract
    const uint32_t bid_slot = model::path_slot(6, side::bid);

    std::vector<double> full_ns(static_cast<size_t>(runs));
    std::vector<double> path_ns(static_cast<size_t>(runs));
    std::vector<double> quote_ns(static_cast<size_t>(runs));
    std::vector<double> sort_ns(static_cast<size_t>(runs));
    std::vector<double> incremental_ns(static_cast<size_t>(runs));
    std::vector<double> forced_ns(static_cast<size_t>(runs));

    for (int run = 0; run < runs; ++run) {
        const size_t r = static_cast<size_t>(run);

        full_ns[r] = time_ns(cycles, [&](uint64_t i) {
            const price_t drift = m.tick[quoted] * static_cast<price_t>(i & 1);
            s.update_external(policy, quoted, side::bid, px(60.0) - m.tick[quoted] + drift, 10);
            s.refresh_best(m, policy, quoted, side::bid, true);
            s.mark_dirty(quoted);
            paths.update_paths(s, quoted, false);
            quote_gates gates;
            gates.enabled = true;
            gates.can_quote_external = true;
            const quote_result res = quotes.build(s, quoted, false, gates);
            g_sink = res.changed() ? 1 : 0;
            s.clear_dirty();
        });

        path_ns[r] = time_ns(cycles, [&](uint64_t) {
            paths.update_paths(s, quoted, false);
            g_sink = s.path(bid_slot).price;
        });

        {
            quote_gates gates;
            gates.enabled = true;
            gates.can_quote_external = true;
            quote_ns[r] = time_ns(cycles, [&](uint64_t i) {
                s.set_force_update(quoted, side::bid, (i & 1) != 0);
                const quote_result res = quotes.build(s, quoted, false, gates);
                g_sink = res.changed() ? 1 : 0;
            });
        }

        {
            uint32_t* order = s.order_data(m, quoted, side::bid);
            const uint32_t count = s.order_size(m, quoted, side::bid);
            sort_ns[r] = time_ns(cycles, [&](uint64_t i) {
                order[0] = (i & 1) ? model::path_slot(6, side::bid)
                                   : model::path_slot(4, side::bid);
                paths.sort_side<side::bid>(s, quoted, order, count);
                g_sink = static_cast<int64_t>(order[0]);
            });
        }

        {
            price_t price = px(60.0);
            incremental_ns[r] = time_ns(cycles, [&](uint64_t i) {
                price += (i & 1) ? m.tick[quoted] : -m.tick[quoted];
                const price_t old_best = s.best_price_adjusted(m, quoted, side::bid);
                s.update_internal(quoted, side::bid, price, 12);
                s.refresh_best(m, policy, quoted, side::bid, false);
                paths.on_best_market_update(s, quoted, side::bid, old_best);
                g_sink = s.path(bid_slot).price;
            });
            forced_ns[r] = time_ns(cycles, [&](uint64_t) {
                paths.update_paths(s, quoted, true);
                g_sink = s.path(bid_slot).price;
            });
        }
    }


    const double full = median(full_ns);
    const double path = median(path_ns);
    const double quote = median(quote_ns);
    const double sort = median(sort_ns);
    const double incremental = median(incremental_ns);
    const double forced = median(forced_ns);
    const double ghz = 3.0;

    uint64_t lean_sides = 0;
    for (uint32_t c = 0; c < m.n_all; ++c) {
        if (!m.is_quoting(c)) continue;
        for (uint32_t d = 0; d < m.n_path_defs; ++d) {
            if (m.path_contract[d] == c) lean_sides += m.lean_count(d);
        }
    }
    lean_sides *= SIDE_COUNT;

    std::printf("AS engine: %u contracts / %u pricing / %u path defs, %llu cycles x %d runs\n",
                m.n_all, m.n_pricing, m.n_path_defs, (unsigned long long)cycles, runs);
    std::printf("  full cycle (md + paths + quotes)   %8.1f ns  %6.0f cycles\n", full, full * ghz);
    std::printf("  path stage, nothing flagged        %8.1f ns  %6.0f cycles\n", path, path * ghz);
    std::printf("  quote stage (both sides)           %8.1f ns  %6.0f cycles\n", quote, quote * ghz);
    std::printf("  ranking (one sort call)            %8.1f ns  %6.0f cycles  [%s]\n", sort,
                sort * ghz, AS_INSERTION_SORT ? "insertion" : "std::sort");
    std::printf("A/B: incremental delta vs forced full recompute\n");
    std::printf("  incremental (one lean moves)       %8.1f ns  %6.0f cycles\n", incremental,
                incremental * ghz);
    std::printf("  forced full recompute              %8.1f ns  %6.0f cycles\n", forced,
                forced * ghz);
    std::printf("  ratio                              %8.2fx\n", forced / incremental);
    std::printf("  lean-sides/cycle                   %8llu  (%.2f ns each)\n",
                (unsigned long long)lean_sides,
                lean_sides ? quote / static_cast<double>(lean_sides) : 0.0);

    // ---- zero-allocation assertion -------------------------------------------
    alloc_probe::allocations = 0;
    alloc_probe::counting = true;
    for (uint64_t i = 0; i < cycles; ++i) {
        s.update_external(policy, quoted, side::bid, px(60.0) + m.tick[quoted] * (i & 1), 10);
        s.refresh_best(m, policy, quoted, side::bid, true);
        s.mark_dirty(quoted);
        paths.update_paths(s, quoted, false);
        quote_gates gates;
        gates.enabled = true;
        gates.can_quote_external = true;
        quotes.build(s, quoted, false, gates);
        s.clear_dirty();
    }
    alloc_probe::counting = false;
    std::printf("  allocations                        %8llu over %llu cycles\n",
                (unsigned long long)alloc_probe::allocations, (unsigned long long)cycles);
    if (alloc_probe::allocations != 0) {
        std::printf("\nFAIL: the cycle is not allocation-free\n");
        return 1;
    }

    // ---- the host cycle with the ledger attached (the deployment shape) ------
    {
        tachyon::trade::liquidity_ledger ledger;
        bench_ports ports;
        host_ports hp;
        hp.send_quote = &port_quote;
        hp.notify_quote = &port_notify;
        hp.send_hedge = &port_hedge;
        hp.send_fill = &port_fill;
        hp.send_switch_fill = &port_switch;
        hp.forward_fill = &port_forward;
        hp.on_alert = &port_alert;
        hp.cancel_all = &port_cancel;
        hp.ctx = &ports;

        cycle_state hs(m);
        host h(m, hs, &ledger, hp);
        for (uint32_t c = 0; c < m.n_all; ++c) {
            for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
                const side sd = static_cast<side>(si);
                h.wire_market(c, sd, true,
                              tachyon::trade::make_market_key(
                                  c, static_cast<tachyon::trade::order_side>(si),
                                  tachyon::trade::EXTERNAL),
                              tachyon::trade::liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
                h.wire_market(c, sd, false,
                              tachyon::trade::make_market_key(
                                  c, static_cast<tachyon::trade::order_side>(si),
                                  tachyon::trade::INTERNAL),
                              tachyon::trade::liquidity_source::LIQUIDITY_SOURCE_RV_QUOTE);
            }
        }
        std::vector<double> host_ns(static_cast<size_t>(runs));
        for (int run = 0; run < runs; ++run) {
            host_ns[static_cast<size_t>(run)] = time_ns(cycles, [&](uint64_t i) {
                const price_t drift = m.tick[quoted] * static_cast<price_t>(i & 1);
                h.on_market_data(quoted, side::bid, true, px(60.0) - m.tick[quoted] + drift, 10);
                h.run_cycle(i);
            });
        }
        const double host = median(host_ns);
        std::printf("  host cycle (md + cycle, ledger)    %8.1f ns  %6.0f cycles\n", host,
                    host * ghz);
        std::printf("  quotes emitted / hedged            %8llu / %llu\n",
                    (unsigned long long)ports.quotes, (unsigned long long)ports.hedges);
    }

    return 0;
}

