// Phase-5 gate: the multi-threaded deployment shape.
//
//   exec_threaded_tests [requests-per-strategy] [contracts]
//
// One thread per strategy (the producers) and one executor thread (the consumer), with
// `sync_mode::spsc` mailboxes -- the configuration a live deployment uses when the three strategies
// run on their own threads. It asserts what such a deployment depends on:
//
//   * nothing is lost: the requests drained equal the requests pushed, per strategy;
//   * nothing is reordered: because every request overwrites its (strategy, kind, side) slot in the
//     book, the final book state must be *exactly* the last request each producer pushed for that
//     key -- a lost release/acquire pairing shows up here as a stale price or size;
//   * the engine's invariants survive concurrency: fills delivered == fills produced, and no crossed
//     residual pair rests on the "exchange";
//   * the ring applies back-pressure: a full mailbox makes the producer retry rather than corrupt.
//
// The price bands are chosen so no two orders can cross (bids below asks), which keeps the expected
// final state exact and independent of scheduling.
//
// ThreadSanitizer: this machine has no TSan runtime (the link fails on libtsan), so the recipe is
// documented rather than run here (PLAN.md §10). On a machine that has it:
//   cmake -S new_impl -B build_tsan -DCMAKE_CXX_FLAGS=-fsanitize=thread -DEXEC_QUEUE_SPSC=ON
//   cmake --build build_tsan --target exec_threaded_tests && ./build_tsan/exec_threaded_tests

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
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

constexpr price_t TICK = 25'000'000;  // 0.025 at 1e-9

// Thread-safe recorders: everything the engine emits is counted with relaxed atomics, which is all a
// smoke test needs (the *ordering* proof is the book state below).
class atomic_gateway final : public exchange_gateway {
public:
    void send_quote(const exchange_order& order) noexcept override {
        ++quotes;
        if (order.sd == side::bid) {
            last_bid.store(order.size > 0 ? order.price : NULL_PRICE, std::memory_order_relaxed);
        } else {
            last_ask.store(order.size > 0 ? order.price : NULL_PRICE, std::memory_order_relaxed);
        }
        const price_t bid = last_bid.load(std::memory_order_relaxed);
        const price_t ask = last_ask.load(std::memory_order_relaxed);
        if (bid != NULL_PRICE && ask != NULL_PRICE && bid >= ask) ++crossed;
    }
    void send_hedge(const exchange_order&) noexcept override { ++hedges; }
    void cancel(uint32_t, strategy_id, bool) noexcept override {}
    void alert(uint32_t, const char*, int32_t) noexcept override { ++alerts; }

    std::atomic<uint64_t> quotes{0};
    std::atomic<uint64_t> hedges{0};
    std::atomic<uint64_t> alerts{0};
    std::atomic<uint64_t> crossed{0};
    std::atomic<price_t> last_bid{NULL_PRICE};
    std::atomic<price_t> last_ask{NULL_PRICE};
};

}  // namespace

namespace {

class atomic_endpoint final : public strategy_endpoint {
public:
    void on_internal_market(uint32_t, side, const internal_top& top) noexcept override {
        ++markets;
        if (top.price != NULL_PRICE && top.size <= 0) ++dead_publications;
    }
    void on_fill(const internal_fill& fill) noexcept override {
        ++fills;
        filled_size.fetch_add(fill.size, std::memory_order_relaxed);
    }
    void end_of_fills(uint32_t) noexcept override { ++batches; }
    void on_as_match_done(uint32_t, qty_t, qty_t) noexcept override {}
    void on_hedge_done(uint32_t, uint32_t, uint32_t, side) noexcept override { ++hedges_done; }
    void on_self_match(uint32_t, const char*) noexcept override { ++self_matches; }
    bool enabled() const noexcept override { return true; }
    void disable() noexcept override { ++disables; }

    std::atomic<uint64_t> markets{0};
    std::atomic<uint64_t> fills{0};
    std::atomic<uint64_t> batches{0};
    std::atomic<uint64_t> hedges_done{0};
    std::atomic<uint64_t> self_matches{0};
    std::atomic<uint64_t> disables{0};
    std::atomic<uint64_t> dead_publications{0};
    std::atomic<qty_t> filled_size{0};
};

// What one producer pushed last for one (contract, side) slot of its own strategy.
struct last_pushed {
    price_t price = NULL_PRICE;
    qty_t size = 0;
    bool any = false;
};

}  // namespace

int main(int argc, char** argv) {
    const uint64_t requests = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
    const uint32_t contracts = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 8;

    atomic_gateway gateway;
    atomic_endpoint endpoints[3] = {atomic_endpoint{}, atomic_endpoint{}, atomic_endpoint{}};

    executor_config config;
    config.contracts = contracts;
    config.queue_capacity = 1024;  // small enough that the producers meet real back-pressure

    basic_executor<sync_mode::spsc> exec;
    exec.init(config, &gateway);
    for (uint32_t s = 0; s < 3; ++s) {
        exec.register_strategy(static_cast<strategy_id>(s), &endpoints[s]);
    }

    std::printf(
        "threaded: %u contracts, %llu requests per strategy, 3 producer threads + 1 consumer\n",
        contracts, (unsigned long long)requests);

    // Each producer owns one strategy, one mailbox and one price band, so the expected final book
    // state is exact and independent of scheduling.
    std::vector<std::vector<last_pushed>> expected(3);
    for (uint32_t s = 0; s < 3; ++s) {
        expected[s].assign(static_cast<size_t>(contracts) * SIDE_COUNT, last_pushed{});
    }
    std::atomic<uint64_t> pushed[3] = {{0}, {0}, {0}};
    std::atomic<uint64_t> retries[3] = {{0}, {0}, {0}};
    std::atomic<bool> producers_done{false};
    std::atomic<uint32_t> running{3};

    std::vector<std::thread> producers;
    for (uint32_t s = 0; s < 3; ++s) {
        producers.emplace_back([&, s] {
            const auto strategy = static_cast<strategy_id>(s);
            // Bids below every ask: nothing can cross, so the book is a pure last-writer-wins map.
            const price_t bid_band = TICK * (100 + 100 * static_cast<int64_t>(s));
            const price_t ask_band = TICK * (1000 + 100 * static_cast<int64_t>(s));
            for (uint64_t i = 0; i < requests; ++i) {
                const uint32_t contract = static_cast<uint32_t>(i % contracts);
                // Derived from a different divisor on purpose: `i % SIDE_COUNT` would correlate the
                // side with the contract index and never touch half the slots.
                const auto sd = static_cast<side>((i / contracts) % SIDE_COUNT);
                const price_t price =
                    (sd == side::bid ? bid_band : ask_band) + TICK * static_cast<int64_t>(i % 20);
                const qty_t size = 1 + static_cast<qty_t>(i % 5);

                const order_request request =
                    make_quote(strategy, contract, sd, price, size, price, size, false);
                // Retry on a full mailbox: the ring must apply back-pressure, never corrupt.
                while (!exec.submit(strategy, request)) {
                    ++retries[s];
                    std::this_thread::yield();
                }
                ++pushed[s];
                // The producer is the only writer of its own slot, so this needs no synchronisation.
                last_pushed& slot = expected[s][contract * SIDE_COUNT + to_index(sd)];
                slot.price = price;
                slot.size = size;
                slot.any = true;
            }
            if (--running == 0) producers_done.store(true, std::memory_order_release);
        });
    }

    // The consumer: drain until every producer is finished and the mailboxes are empty.
    uint64_t cycles = 0;
    for (;;) {
        exec.run_cycle(cycles++);
        if (producers_done.load(std::memory_order_acquire)) {
            bool empty = true;
            for (uint32_t s = 0; s < 3; ++s) {
                empty &= exec.mailbox(static_cast<strategy_id>(s)).empty();
            }
            if (empty) break;
        }
    }
    for (auto& t : producers) t.join();
    // ---- assertions ------------------------------------------------------
    const executor_stats& st = exec.stats();
    const uint64_t total = requests * 3;
    check_eq(static_cast<long long>(st.requests_drained), static_cast<long long>(total),
             "every pushed request was drained");
    check_eq(static_cast<long long>(st.dropped_bad_contract + st.dropped_bad_strategy), 0,
             "no request was routed to a bad contract or strategy");
    for (uint32_t s = 0; s < 3; ++s) {
        check_eq(static_cast<long long>(pushed[s].load()), static_cast<long long>(requests),
                 "the producer pushed everything it intended to");
    }

    // The ordering proof: the book must hold exactly what each producer pushed last for its slot.
    uint64_t checked_slots = 0;
    for (uint32_t s = 0; s < 3; ++s) {
        const auto strategy = static_cast<strategy_id>(s);
        for (uint32_t c = 0; c < contracts; ++c) {
            for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
                const last_pushed& want = expected[s][c * SIDE_COUNT + si];
                if (!want.any) continue;
                const auto* order = exec.engine(c).book().find(strategy, request_kind::quote,
                                                               static_cast<side>(si));
                ++checked_slots;
                if (order->price != want.price || order->size != want.size) {
                    ++g_failures;
                    std::printf(
                        "  FAIL  slot mismatch strategy %u contract %u side %u: book %lld@%lld "
                        "expected %lld@%lld\n",
                        s, c, si, (long long)order->size, (long long)order->price,
                        (long long)want.size, (long long)want.price);
                }
            }
        }
    }
    check_eq(static_cast<long long>(checked_slots),
             static_cast<long long>(contracts) * SIDE_COUNT * 3, "every slot was checked");

    uint64_t engine_fills = 0;
    uint64_t endpoint_fills = 0;
    for (uint32_t c = 0; c < contracts; ++c) engine_fills += exec.engine(c).stats().fills;
    for (uint32_t s = 0; s < 3; ++s) endpoint_fills += endpoints[s].fills.load();
    check_eq(static_cast<long long>(endpoint_fills), static_cast<long long>(engine_fills),
             "fills delivered to the strategies match the fills produced");
    check_eq(static_cast<long long>(gateway.crossed.load()), 0,
             "no crossed residual rested on the exchange");
    for (uint32_t s = 0; s < 3; ++s) {
        check_eq(static_cast<long long>(endpoints[s].dead_publications.load()), 0,
                 "no price was published with no size");
    }

    std::printf("  %-30s %10llu\n", "requests drained", (unsigned long long)st.requests_drained);
    std::printf("  %-30s %10llu\n", "consumer cycles", (unsigned long long)cycles);
    std::printf("  %-30s %10llu %llu %llu\n", "back-pressure retries",
                (unsigned long long)retries[0].load(), (unsigned long long)retries[1].load(),
                (unsigned long long)retries[2].load());
    std::printf("  %-30s %10llu\n", "internal fills", (unsigned long long)engine_fills);
    std::printf("  %-30s %10llu\n", "residual quotes", (unsigned long long)gateway.quotes.load());
    std::printf("  %-30s %10llu\n", "alerts", (unsigned long long)gateway.alerts.load());

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-5 checks passed\n");
    return EXIT_SUCCESS;
}
