// Full-pipeline driver: the three REAL strategy engines -> mailboxes -> implied engine -> exchange.
//
//   exec_pipeline [rvAsConfigDir] [mmConfigDir] [cycles] [seed]
//
// Nothing here is a mock except the exchange and the market data:
//
//   * the market maker is an `mm::adapter` built from the MM config, pushing through
//     `mm_request_sink` into mailbox[MM];
//   * RV and AS are their own hosts, pushing through `rv_adapter` / `as_adapter` into their
//     mailboxes;
//   * the engine drains all three, matches internally, and sends the residual to the gateway;
//   * `mm_bridge` / `rv_bridge` / `as_bridge` feed the fills and the internal book back into the
//     strategies, which is what makes the next cycle's decisions react to what happened.
//
// That closes the loop, so it is the deployment smoke test: what the strategies emit is what the
// engine sees, what the engine matches reaches the strategies as fills, and the only orders that
// leave are residuals that cannot trade internally.
//
// Invariants asserted (exit non-zero on failure):
//   * every request the strategies pushed was drained (nothing lost, nothing double-counted);
//   * the engine reports exactly as many fills as the bridges received;
//   * no crossed residual pair rests at the end of any cycle;
//   * the publisher only ever addressed RV and AS (the MM is not a subscriber);
//   * RV and AS received internal market updates, i.e. the loop is actually closed.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <adapters/strategy_adapters.hpp>
#include <adapters/strategy_bridges.hpp>
#include <as/config.hpp>
#include <mm/config.hpp>
#include <rv/config.hpp>

#include "executor.hpp"
#include "ports.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;
namespace rv = tachyon::trade::rv;
namespace as = tachyon::trade::as;
namespace mm = tachyon::trade::mm;

// ---- exchange side -------------------------------------------------------
// Records what leaves the engine and validates the resting residual pair per contract.
class venue final : public exchange_gateway {
public:
    void init(uint32_t contracts) {
        bid.assign(contracts, NULL_PRICE);
        ask.assign(contracts, NULL_PRICE);
        hedges_seen.assign(contracts, 0);
    }

    void send_quote(const exchange_order& order) noexcept override {
        ++quotes;
        if (order.contract >= bid.size()) return;
        // A size-0 quote is a price-only order and cannot cross.
        const price_t price = order.size > 0 ? order.price : NULL_PRICE;
        (order.sd == side::bid ? bid : ask)[order.contract] = price;
    }

    void send_hedge(const exchange_order& order) noexcept override {
        ++hedges;
        hedged_size += order.size;
        if (order.contract < hedges_seen.size()) ++hedges_seen[order.contract];
        pending.push_back(order);
    }

    void cancel(uint32_t, strategy_id, bool) noexcept override { ++cancels; }
    void alert(uint32_t, const char* reason, int32_t) noexcept override {
        ++alerts;
        if (alerts <= 5) std::printf("  alert: %s\n", reason ? reason : "");
    }

    // Called once per cycle: a crossed pair that rests would be a missed internal match.
    void end_of_cycle() {
        for (size_t c = 0; c < bid.size(); ++c) {
            if (bid[c] != NULL_PRICE && ask[c] != NULL_PRICE && bid[c] >= ask[c]) {
                ++crossed;
                bid[c] = NULL_PRICE;
                ask[c] = NULL_PRICE;
            }
        }
    }

    std::vector<price_t> bid;
    std::vector<price_t> ask;
    std::vector<uint32_t> hedges_seen;
    std::vector<exchange_order> pending;  // hedge legs awaiting the simulated ack
    uint64_t quotes = 0;
    uint64_t hedges = 0;
    uint64_t cancels = 0;
    uint64_t alerts = 0;
    uint64_t crossed = 0;
    qty_t hedged_size = 0;
};

// ---- config loading ------------------------------------------------------
// RV and AS must price from the *same* model, or their contract ids, tick grids and fairs do not
// line up and AS (correctly) disables itself on maxTicksThroughMarket. `AS_model.csv` is a full RV
// model as well (PricingMatrix / FactorConfigs / RVConfigs), so one file feeds both engines; the
// driver falls back to the legacy RV model file when a config directory has no AS model.
std::string shared_model_path(const std::string& dir) {
    if (std::filesystem::exists(dir + "/AS_model.csv")) return dir + "/AS_model.csv";
    if (std::filesystem::exists(dir + "/CME_small_model.csv")) return dir + "/CME_small_model.csv";
    return dir + "/model.csv";
}

bool load_rv(const std::string& dir, const std::string& model_path, rv::model& out) {
    try {
        const rv::rv_model_cfg cfg = rv::rv_model_cfg::load(model_path);
        const rv::secdb_table secdb = rv::load_secdb(dir + "/secdb.cme.csv");
        std::unordered_map<std::string, double> overrides;
        for (const auto& name : cfg.pricing_contracts) {
            if (secdb.find(name) == secdb.end()) overrides[name] = 0.025;
        }
        out = rv::model::build(rv::build_model_inputs(cfg, secdb, cfg.width_multiplier(), 1.0,
                                                      overrides));
        return true;
    } catch (const std::exception& e) {
        std::printf("exec_pipeline: RV config failed: %s\n", e.what());
        return false;
    }
}

bool load_as(const std::string& dir, const std::string& model_path, as::model& out) {
    try {
        const as::as_model_cfg cfg = as::as_model_cfg::load(model_path, dir + "/AS_paths.csv",
                                                           dir + "/secdb.cme.csv");
        out = as::model::build(as::build_model_inputs(cfg));
        return true;
    } catch (const std::exception& e) {
        std::printf("exec_pipeline: AS config failed: %s\n", e.what());
        return false;
    }
}

}  // namespace

namespace {

using tachyon::trade::INTERNAL;
using tachyon::trade::EXTERNAL;
using tachyon::trade::liquidity_ledger;
using tachyon::trade::make_market_key;
using tachyon::trade::order_side;

// ---- the run -------------------------------------------------------------
int run(const std::string& rv_dir, const std::string& mm_dir, uint64_t cycles, uint32_t seed) {
    // ---- models ----------------------------------------------------------
    rv::model rv_model;
    as::model as_model;
    // One model file for both engines: same ids, same tick grids, same fairs (see shared_model_path).
    const std::string model_path = shared_model_path(rv_dir);
    if (!load_rv(rv_dir, model_path, rv_model)) return EXIT_FAILURE;
    if (!load_as(rv_dir, model_path, as_model)) return EXIT_FAILURE;

    mm::mm_model_cfg mm_cfg;
    mm::secdb_table mm_secdb;
    try {
        mm_cfg = mm::mm_model_cfg::load(mm_dir + "/model.csv");
        mm_secdb = mm::load_secdb(mm_dir + "/secdb.cme.csv");
    } catch (const std::exception& e) {
        std::printf("exec_pipeline: MM config failed: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // The driver feeds a synthetic tape, so the strategy's own quote-validity bound (a safety gate
    // against quoting through the market) is widened here rather than tuned per config: the gate
    // itself is covered by rv_host_test, and letting it fire would just disable RV for the run. Note
    // a configured 0 rejects every quote (legacy compares `through < bound`), so a floor is needed
    // rather than a multiplier.
    for (size_t c = 0; c < rv_model.max_ticks_through_market.size(); ++c) {
        const price_t tick = c < rv_model.tick.size() ? rv_model.tick[c].divisor : 1;
        const price_t floor = 100 * tick;
        if (rv_model.max_ticks_through_market[c] < floor) {
            rv_model.max_ticks_through_market[c] = floor;
        }
    }

    // The adapters only *hold a reference* to the executor and a reference to a host, so they can
    // be built before either is initialised: nothing is emitted before the first cycle. That
    // resolves the cycle "executor needs the contract count, the count needs the MM model, the MM
    // model needs the sink, the sink needs the executor".
    venue exchange;
    executor exec;

    mm_request_sink<executor> mm_sink(exec);
    rv_adapter<executor> rv_out(exec);
    as_adapter<executor> as_out(exec);

    mm::adapter mm_ad(mm_cfg, mm_secdb, 1.0e6, /*initial_shift=*/2, mm_sink);
    mm::host& mm_host = const_cast<mm::host&>(mm_ad.get_host());

    const uint32_t contracts = std::max({mm_ad.get_model().n_contracts, rv_model.n_all,
                                         as_model.n_all});
    executor_config config;
    config.contracts = contracts;
    config.queue_capacity = 4096;
    exec.init(config, &exchange);
    exchange.init(contracts);

    rv::cycle_state rv_state;
    rv_state.init(rv_model);
    rv::host rv_host(rv_model, rv_state, /*ledger=*/nullptr, rv_out.ports());

    as::cycle_state as_state(as_model);
    liquidity_ledger ledger;
    as::host as_host(as_model, as_state, &ledger, as_out.ports());

    // ---- the inbound half ------------------------------------------------
    mm_bridge mm_in(mm_host);
    rv_bridge rv_in(rv_host);
    as_bridge as_in(as_host);
    exec.register_strategy(strategy_id::MM, &mm_in);
    exec.register_strategy(strategy_id::RV, &rv_in);
    exec.register_strategy(strategy_id::AS, &as_in);

    // ---- wiring ----------------------------------------------------------
    mm_ad.enable(true);
    for (uint32_t c = 0; c < mm_ad.get_model().n_contracts; ++c) {
        mm_ad.set_contract_enabled(c, true);
        mm_ad.set_contract_can_trade(c, true);
    }
    for (uint32_t c = 0; c < rv_model.n_all; ++c) {
        rv_host.set_max_clip(c, 1000);  // the model carries 0 = not supplied (S12)
        for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
            const auto sd = static_cast<rv::side>(si);
            const auto os = si == 0 ? order_side::BUY : order_side::SELL;
            rv_host.wire_market(c, sd, true, make_market_key(c, os, EXTERNAL),
                                tachyon::trade::liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
            rv_host.wire_market(c, sd, false, make_market_key(c, os, INTERNAL),
                                tachyon::trade::liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE);
        }
    }
    for (uint32_t c = 0; c < as_model.n_all; ++c) {
        for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
            const auto sd = static_cast<as::side>(si);
            const auto os = si == 0 ? order_side::BUY : order_side::SELL;
            as_host.wire_market(c, sd, true, make_market_key(c, os, EXTERNAL),
                                tachyon::trade::liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
            as_host.wire_market(c, sd, false, make_market_key(c, os, INTERNAL),
                                tachyon::trade::liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE);
        }
    }

    // ---- the tape --------------------------------------------------------
    std::printf("exec pipeline: %u contracts (mm %u, rv %u, as %u), %llu cycles, seed %u\n",
                contracts, mm_ad.get_model().n_contracts, rv_model.n_all, as_model.n_all,
                (unsigned long long)cycles, seed);

    // The tape has to be *fair-consistent*: RV and AS price from their own model, so a market that
    // walks away from those fairs would (correctly) trip their maxTicksThroughMarket gate and
    // disable them, leaving nothing to match. Outrights therefore walk on their own tick grid and
    // spreads are always re-derived from their legs, exactly as rv_replay does.
    rv::valuation fairs(rv_model);
    fairs.recompute(rv_state, 0);

    std::vector<price_t> base(rv_model.n_all);
    std::vector<price_t> tick(rv_model.n_all);
    std::vector<int64_t> offset(rv_model.n_all, 0);
    for (uint32_t c = 0; c < rv_model.n_all; ++c) {
        base[c] = rv_state.stf[c] != NULL_PRICE ? rv_state.stf[c] : rv::to_scaled(40.0);
        tick[c] = rv_model.tick[c].divisor;
    }

    // A spread's book is the sum of its legs, so it can never drift away from its own fair.
    auto contract_price = [&](uint32_t c) -> price_t {
        const uint32_t begin = rv_model.leg_begin[c];
        const uint32_t end = rv_model.leg_begin[c + 1];
        if (end - begin <= 1) return base[c] + offset[c] * tick[c];
        double sum = 0.0;
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t leg = rv_model.leg_index[k];
            sum += rv_model.leg_ratio[k] * static_cast<double>(base[leg] + offset[leg] * tick[leg]);
        }
        return static_cast<price_t>(sum);
    };

    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> step(-1, 1);
    std::uniform_int_distribution<int> chance(0, 99);
    // Three feeds never tick at exactly the same instant; the skew is what makes the strategies'
    // quotes overlap, so the internal book actually crosses.
    std::uniform_int_distribution<int> skew(-1, 1);

    // Seed the strategies' mids from the tape before the first cycle. Without this RV's first
    // validation runs against a NULL mid (its own fair is derived from the other pricing contracts'
    // mids, which are still unset) and it disables itself on maxTicksThroughMarket -- exactly the
    // ordering rv_replay gets right by seeding the books before its loop.
    for (uint32_t c = 0; c < rv_model.n_all; ++c) {
        if (rv_model.used_in_pricing[c]) rv_state.mid[c] = contract_price(c);
    }

    // Warm start: prime the strategies' *internal* books and short-term fairs from the tape, the way a
    // running deployment already has them when a process restarts. Without this RV's first cycle
    // prices off an empty internal book, lands far from the market and its own maxTicksThroughMarket
    // gate disables it for the whole run. From cycle 1 the engine publishes these books itself.
    for (uint32_t c = 0; c < rv_model.n_all; ++c) {
        const price_t mid = contract_price(c);
        const price_t grid = tick[c];
        if (rv_model.used_in_pricing[c]) rv_state.mid[c] = mid;
        rv_host.on_market_data(c, rv::side::bid, false, mid - grid, 30);
        rv_host.on_market_data(c, rv::side::ask, false, mid + grid, 30);
        if (rv_state.stf[c] != NULL_PRICE) rv_state.stf[c] = mid;
        as_host.on_stf(c, mid);
        if (c < as_model.n_all) {
            as_host.on_market_data(c, as::side::bid, false, mid - grid, 30);
            as_host.on_market_data(c, as::side::ask, false, mid + grid, 30);
        }
    }

    for (uint64_t cycle = 0; cycle < cycles; ++cycle) {
        const uint64_t now = cycle;

        // 1. walk the pricing outrights; every other book follows from them.
        for (uint32_t c = 0; c < rv_model.n_pricing; ++c) {
            if (chance(rng) < 30) offset[c] += step(rng);
        }

        // 2. republish every book.
        for (uint32_t c = 0; c < contracts; ++c) {
            const price_t mid = c < rv_model.n_all
                                    ? contract_price(c)
                                    : base[0] + 40 * static_cast<int64_t>(c) * tick[0];
            const price_t grid = c < rv_model.n_all ? tick[c] : tick[0];
            const price_t mm_mid = mid + grid * skew(rng);
            const price_t rv_mid = mid + grid * skew(rng);
            const price_t as_mid = mid + grid * skew(rng);

            if (c < mm_ad.get_model().n_contracts) {
                const mm::price_t mm_tick = mm_ad.get_model().tick_size[c];
                const int64_t mid_ticks = mm_tick > 0 ? mm_mid / mm_tick : 0;
                mm::level bids[mm::BOOK_LEVELS];
                mm::level asks[mm::BOOK_LEVELS];
                for (uint32_t k = 0; k < mm::BOOK_LEVELS; ++k) {
                    bids[k] = mm::level{
                        mm_tick * (mid_ticks - 1 - static_cast<int64_t>(k)), 200, 4, 0};
                    asks[k] = mm::level{
                        mm_tick * (mid_ticks + 1 + static_cast<int64_t>(k)), 200, 4, 0};
                }
                mm::market_data_view view;
                view.levels = bids;
                view.count = mm::BOOK_LEVELS;
                view.price_num = 1;
                view.qty_num = 1;
                view.batch_id = now;
                mm_ad.on_market_data(c, mm::side::bid, view);
                view.levels = asks;
                view.batch_id = now + 1;
                mm_ad.on_market_data(c, mm::side::ask, view);
            }
            if (c < rv_model.n_all) {
                rv_host.on_market_data(c, rv::side::bid, true, rv_mid - grid, 30);
                rv_host.on_market_data(c, rv::side::ask, true, rv_mid + grid, 30);
                // The tape *is* the market, so the pricing mid is the tape's price. (`fairs.mid()`
                // returns NULL until the ODM has every factor, which would clobber the seed and
                // trip RV's maxTicksThroughMarket on the first cycle.)
                if (rv_model.used_in_pricing[c]) rv_state.mid[c] = mid;
            }
            if (c < as_model.n_all) {
                as_host.on_market_data(c, as::side::bid, true, as_mid - grid, 30);
                as_host.on_market_data(c, as::side::ask, true, as_mid + grid, 30);
            }
        }

        // AS prices from a short-term fair, which in the deployment arrives from the pricing engine
        // (legacy AS::handleSTFUpdate). Feed RV's fair for the contracts RV prices -- that is where
        // the STF comes from in production -- or AS trips its own maxTicksThroughMarket gate.
        for (uint32_t c = 0; c < as_model.n_all; ++c) {
            const price_t fair = c < rv_model.n_all ? rv_state.stf[c] : NULL_PRICE;
            if (fair != NULL_PRICE) as_host.on_stf(c, fair);
        }

        // The strategies decide, then the executor drains their mailboxes.
        mm_ad.run_cycle(now);
        rv_host.on_timer(now);
        as_host.run_cycle(now);
        exec.run_cycle(now);

        // The venue leg: every hedge order is acknowledged and completed immediately, which is
        // what releases the strategies' liquidity reservations.
        for (const exchange_order& hedge : exchange.pending) {
            const auto os = hedge.sd == side::bid ? order_side::BUY : order_side::SELL;
            const tachyon::trade::market_key target = make_market_key(hedge.contract, os, INTERNAL);
            if (hedge.strategy == strategy_id::RV && hedge.contract < rv_model.n_all) {
                rv_host.on_hedge_ack(hedge.hedge_id, 1, hedge.size);
                rv_host.on_hedge_done(hedge.hedge_id);
            } else if (hedge.strategy == strategy_id::AS && hedge.contract < as_model.n_all) {
                as_host.on_hedge_ack(hedge.hedge_id, target, 1, hedge.size);
                as_host.on_hedge_done(hedge.hedge_id, target);
            }
        }
        exchange.pending.clear();
        exchange.end_of_cycle();
    }

    // ---- report ----------------------------------------------------------
    const executor_stats& st = exec.stats();
    uint64_t published = 0;
    uint64_t engine_fills = 0;
    for (uint32_t c = 0; c < contracts; ++c) {
        published += exec.engine(c).stats().publishes;
        engine_fills += exec.engine(c).stats().fills;
    }
    const uint64_t bridge_fills = mm_in.fills + rv_in.fills + as_in.fills;

    std::printf("  %-28s %10llu\n", "requests drained", (unsigned long long)st.requests_drained);
    std::printf("  %-28s %10llu\n", "mailbox drops", (unsigned long long)st.queue_dropped);
    std::printf("  %-28s %10llu\n", "contracts flushed", (unsigned long long)st.contracts_flushed);
    std::printf("  %-28s %10llu\n", "internal fills", (unsigned long long)engine_fills);
    std::printf("  %-28s %10llu (mm %llu, rv %llu, as %llu)\n", "fills to strategies",
                (unsigned long long)bridge_fills, (unsigned long long)mm_in.fills,
                (unsigned long long)rv_in.fills, (unsigned long long)as_in.fills);
    std::printf("  %-28s %10llu\n", "internal market updates",
                (unsigned long long)(rv_in.market_updates + as_in.market_updates));
    std::printf("  %-28s %10llu\n", "residual quotes out", (unsigned long long)exchange.quotes);
    std::printf("  %-28s %10llu\n", "hedge legs out", (unsigned long long)exchange.hedges);
    std::printf("  %-28s %10llu\n", "hedges completed",
                (unsigned long long)(rv_in.hedges_done + as_in.hedges_done));
    std::printf("  %-28s %10llu\n", "engine publications", (unsigned long long)published);
    std::printf("  %-28s %10llu\n", "alerts", (unsigned long long)exchange.alerts);
    std::printf("  %-28s %10llu\n", "crossed residuals", (unsigned long long)exchange.crossed);
    // Per-strategy health, which is what tells a reader whether a strategy is even quoting.
    std::printf("  %-28s %10s\n", "rv validation failures / disabled", "");
    std::printf("  %-28s %10llu / %d\n", "  rv",
                (unsigned long long)rv_host.stats().validation_failures,
                rv_host.stats().disabled ? 1 : 0);
    std::printf("  %-28s %10llu / %d\n", "  as",
                (unsigned long long)as_host.stats().validation_failures,
                as_host.stats().disabled ? 1 : 0);
    std::printf("  %-28s %10llu / %d\n", "  mm (quoting disables)",
                (unsigned long long)mm_in.external_quoting_disables,
                mm_host.stats().disabled ? 1 : 0);
    // A strategy that gated itself out is a property of this synthetic tape, not of the engine: RV
    // validates every quote against the traded contract's opposite side (a lean the tape does not
    // model can trip it), and AS needs internal liquidity on its lean contracts to hedge, which a tape
    // whose internal book holds only what the strategies quote cannot always provide. The engine
    // invariants below are unaffected either way.
    if (rv_host.stats().disabled || as_host.stats().disabled) {
        std::printf("  note: a strategy disabled itself on this synthetic tape (rv %s, as %s); see "
                    "PLAN.md 7\n",
                    rv_host.stats().disabled ? "yes" : "no",
                    as_host.stats().disabled ? "yes" : "no");
    }

    bool ok = true;
    if (st.requests_drained == 0) {
        std::printf("FAIL: no request reached the engine\n");
        ok = false;
    }
    if (bridge_fills != engine_fills) {
        std::printf("FAIL: engine reported %llu fills but the strategies received %llu\n",
                    (unsigned long long)engine_fills, (unsigned long long)bridge_fills);
        ok = false;
    }
    if (exchange.crossed != 0) {
        std::printf("FAIL: %llu crossed residual pairs rested on the exchange\n",
                    (unsigned long long)exchange.crossed);
        ok = false;
    }
    if (published == 0) {
        std::printf("FAIL: the engine never published an internal market\n");
        ok = false;
    }
    if (ok) std::printf("exec pipeline: all invariants hold\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string rv_dir = argc > 1 ? argv[1] : "config/minimal_cme";
    const std::string mm_dir = argc > 2 ? argv[2] : "config/backtest";
    const uint64_t cycles = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2000;
    const uint32_t seed = argc > 4 ? static_cast<uint32_t>(std::atoi(argv[4])) : 42;
    return run(rv_dir, mm_dir, cycles, seed);
}
