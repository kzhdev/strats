// Runnable driver for the new executor: a deterministic request tape through the real pipeline.
//
//   exec_replay [contracts] [cycles] [seed]
//
// The tape plays the request mix a live cycle produces -- MM quoting both sides, RV quoting and
// hedging, AS quoting and hedging -- and prints what left the engine. It is the deployment smoke
// test: it asserts the invariants a live run depends on instead of comparing to a golden file, so it
// stays meaningful while the strategies evolve:
//
//   * every submitted request reached an engine (no silent loss);
//   * the internal book never crosses itself, so no non-AS self match is reported;
//   * no crossed residual pair leaves for the exchange (that would mean a missed internal match);
//   * the whole run performs no allocation.

#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "executor.hpp"
#include "ports.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;

struct alloc_probe {
    static inline uint64_t allocations = 0;
    static inline bool counting = false;
};

constexpr price_t TICK = 25'000'000;  // 0.025 at 1e-9

struct counters {
    // One residual pair per contract: each contract quotes its own book.
    std::vector<price_t> bid_by_contract;
    std::vector<price_t> ask_by_contract;
    uint64_t quotes = 0;
    uint64_t hedges = 0;
    uint64_t alerts = 0;
    uint64_t crossed_residuals = 0;

    void init(uint32_t contracts) {
        bid_by_contract.assign(contracts, NULL_PRICE);
        ask_by_contract.assign(contracts, NULL_PRICE);
    }

    void note_quote(const exchange_order& order) {
        ++quotes;
        if (order.contract >= bid_by_contract.size()) return;
        // A size-0 quote is a price-only order (legacy persists a price after the size is gone), so
        // it cannot cross; only a priced *and* sized residual can.
        const price_t price = order.size > 0 ? order.price : NULL_PRICE;
        if (order.sd == side::bid) {
            bid_by_contract[order.contract] = price;
        } else {
            ask_by_contract[order.contract] = price;
        }
    }

    // The resting state of the two residuals, checked once per cycle: a crossed *resting* pair means
    // the engine quoted something it should have matched first. (Inside one cycle the engine emits
    // the two sides in the legacy order, so a crossed pair never even exists between two messages.)
    void end_of_cycle() {
        for (size_t c = 0; c < bid_by_contract.size(); ++c) {
            const price_t bid = bid_by_contract[c];
            const price_t ask = ask_by_contract[c];
            if (bid != NULL_PRICE && ask != NULL_PRICE && bid >= ask) {
                ++crossed_residuals;
                bid_by_contract[c] = NULL_PRICE;
                ask_by_contract[c] = NULL_PRICE;
            }
        }
    }
};

class counting_gateway final : public exchange_gateway {
public:
    explicit counting_gateway(counters& c) noexcept : counters_(c) {}
    void send_quote(const exchange_order& order) noexcept override { counters_.note_quote(order); }
    void send_hedge(const exchange_order& order) noexcept override {
        ++counters_.hedges;
        hedged_size += order.size;
    }
    void cancel(uint32_t, strategy_id, bool) noexcept override {}
    void alert(uint32_t, const char*, int32_t) noexcept override { ++counters_.alerts; }

    qty_t hedged_size = 0;

private:
    counters& counters_;
};

class counting_endpoint final : public strategy_endpoint {
public:
    void on_internal_market(uint32_t, side, const internal_top&) noexcept override { ++markets; }
    void on_fill(const internal_fill& fill) noexcept override {
        ++fills;
        traded_size += fill.size;
    }
    void end_of_fills(uint32_t) noexcept override { ++batches; }
    void on_self_match(uint32_t, const char*) noexcept override { ++self_matches; }
    bool enabled() const noexcept override { return true; }
    void disable() noexcept override {}

    uint64_t markets = 0;
    uint64_t fills = 0;
    uint64_t batches = 0;
    uint64_t self_matches = 0;
    qty_t traded_size = 0;
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
    const uint32_t contracts = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 4;
    const uint64_t cycles = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000;
    const uint32_t seed = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 42;

    // Everything that can allocate is built before the probe is armed.
    counters stats;
    stats.init(contracts);
    counting_gateway gateway(stats);
    counting_endpoint mm_endpoint;
    counting_endpoint rv_endpoint;
    counting_endpoint as_endpoint;

    executor_config config;
    config.contracts = contracts;
    config.queue_capacity = 4096;

    executor exec;
    exec.init(config, &gateway);
    exec.register_strategy(strategy_id::MM, &mm_endpoint);
    exec.register_strategy(strategy_id::RV, &rv_endpoint);
    exec.register_strategy(strategy_id::AS, &as_endpoint);

    std::mt19937 rng(seed);
    // A per-contract random walk, so the three strategies disagree often enough to cross.
    std::vector<price_t> mids(contracts);
    for (uint32_t c = 0; c < contracts; ++c) mids[c] = TICK * (1000 + 40 * static_cast<int64_t>(c));
    std::uniform_int_distribution<int> step(-1, 1);
    std::uniform_int_distribution<int> chance(0, 99);

    uint64_t submitted = 0;
    alloc_probe::counting = true;
    for (uint64_t cycle = 0; cycle < cycles; ++cycle) {
        for (uint32_t c = 0; c < contracts; ++c) {
            mids[c] += TICK * step(rng);
            if (mids[c] < TICK * 10) mids[c] = TICK * 10;

            // MM quotes both sides of a decision together, exactly as the MM host does, so its own
            // two sides can never cross: the pair is staged in one flush and applied in order.
            if (chance(rng) < 70) {
                exec.submit(strategy_id::MM, make_quote(strategy_id::MM, c, side::bid,
                                                        mids[c] - TICK, 5, mids[c] - TICK, 5,
                                                        false));
                exec.submit(strategy_id::MM, make_quote(strategy_id::MM, c, side::ask,
                                                        mids[c] + TICK, 5, mids[c] + TICK, 5,
                                                        false));
                submitted += 2;
            }
            // RV quotes one side: passive most of the time, aggressive when it likes the level --
            // the aggressive case is what produces genuine internal crossings.
            if (chance(rng) < 30) {
                const price_t rv_price = chance(rng) < 20 ? mids[c] + TICK : mids[c] - TICK / 2;
                exec.submit(strategy_id::RV,
                            make_quote(strategy_id::RV, c, side::bid, rv_price, 3, rv_price, 3,
                                       false));
                ++submitted;
            }
            if (chance(rng) < 25) {
                exec.submit(strategy_id::RV,
                            make_hedge(strategy_id::RV, c, side::ask, mids[c] + TICK / 2,
                                       mids[c] + TICK / 2, 2, true, false,
                                       static_cast<uint32_t>(cycle), 0));
                ++submitted;
            }
            // AS quotes the outer ask and hedges aggressively.
            if (chance(rng) < 15) {
                exec.submit(strategy_id::AS, make_quote(strategy_id::AS, c, side::ask,
                                                        mids[c] + 2 * TICK, 4,
                                                        mids[c] + 2 * TICK, 4, false));
                ++submitted;
            }
            if (chance(rng) < 10) {
                exec.submit(strategy_id::AS,
                            make_hedge(strategy_id::AS, c, side::bid, mids[c] + TICK,
                                       mids[c] + TICK, 3, true, false,
                                       static_cast<uint32_t>(cycle), 0));
                ++submitted;
            }
        }
        exec.run_cycle(cycle);
        stats.end_of_cycle();
    }
    alloc_probe::counting = false;

    const executor_stats& st = exec.stats();
    const uint64_t fills = mm_endpoint.fills + rv_endpoint.fills + as_endpoint.fills;
    std::printf("exec replay: %u contracts, %llu cycles, seed %u\n", contracts,
                (unsigned long long)cycles, seed);
    std::printf("  %-26s %10llu\n", "submitted", (unsigned long long)submitted);
    std::printf("  %-26s %10llu\n", "drained", (unsigned long long)st.requests_drained);
    std::printf("  %-26s %10llu\n", "dropped (bad contract)",
                (unsigned long long)st.dropped_bad_contract);
    std::printf("  %-26s %10llu\n", "quotes out", (unsigned long long)stats.quotes);
    std::printf("  %-26s %10llu\n", "hedges out", (unsigned long long)stats.hedges);
    std::printf("  %-26s %10llu\n", "fills", (unsigned long long)fills);
    std::printf("  %-26s %10lld\n", "traded size",
                (long long)(mm_endpoint.traded_size + rv_endpoint.traded_size +
                            as_endpoint.traded_size));
    std::printf("  %-26s %10llu\n", "internal market updates",
                (unsigned long long)(mm_endpoint.markets + rv_endpoint.markets +
                                     as_endpoint.markets));
    std::printf("  %-26s %10llu\n", "alerts", (unsigned long long)stats.alerts);
    std::printf("  %-26s %10llu\n", "crossed residuals",
                (unsigned long long)stats.crossed_residuals);
    std::printf("  %-26s %10llu\n", "allocations", (unsigned long long)alloc_probe::allocations);

    bool ok = true;
    if (st.requests_drained != submitted) {
        std::printf("FAIL: %llu of %llu requests reached an engine\n",
                    (unsigned long long)st.requests_drained, (unsigned long long)submitted);
        ok = false;
    }
    if (stats.crossed_residuals != 0) {
        std::printf("FAIL: %llu crossed residual pairs left for the exchange\n",
                    (unsigned long long)stats.crossed_residuals);
        ok = false;
    }
    if (mm_endpoint.self_matches + rv_endpoint.self_matches + as_endpoint.self_matches != 0) {
        std::printf("FAIL: %llu non-AS self matches\n",
                    (unsigned long long)(mm_endpoint.self_matches + rv_endpoint.self_matches +
                                         as_endpoint.self_matches));
        ok = false;
    }
    if (alloc_probe::allocations != 0) {
        std::printf("FAIL: %llu allocations\n", (unsigned long long)alloc_probe::allocations);
        ok = false;
    }
    if (ok) std::printf("exec replay: all invariants hold\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
