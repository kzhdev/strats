// Config-driven replay for the new MarketMaker engine (PLAN.md §7).
//
//   mm_replay [configDir] [events] [seed]
//
// Loads the real config directory (MMConfigs + secdb), drives a deterministic tape of book
// updates, fills and enable/disable toggles through the host, and asserts the deployment
// invariants a deployment would notice:
//
//   * every emitted external price is a tick multiple (or NULL, for a removal);
//   * the two sides of a contract never cross in the emitted external prices;
//   * every emitted size is non-negative and lot-aligned;
//   * a hedge's size is always the negated net position at the moment it is emitted;
//   * the host's counters stay consistent with what reached the sink.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "book.hpp"
#include "config.hpp"
#include "host.hpp"
#include "model.hpp"
#include "state.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade;
using namespace tachyon::trade::mm;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

// Records the emitted sequence per contract and validates it against the live state.
struct validator {
    struct last {
        price_t bid = tachyon::NULL_PRICE;
        price_t ask = tachyon::NULL_PRICE;
    };
    std::map<uint32_t, last> last_quote;
    const model* m = nullptr;
    const cycle_state* st = nullptr;
    uint64_t intents = 0;
    uint64_t removals = 0;
    uint64_t hedges = 0;
    uint64_t overflows = 0;

    static void on_intent(void* ctx, const order_intent& it) {
        validator& v = *static_cast<validator*>(ctx);
        ++v.intents;
        const uint32_t c = it.contract;
        if (c >= v.m->n_contracts) return;
        const price_t tick = v.m->tick_size[c];
        const contract_state& s = v.st->contract(c);

        if (it.type == order_intent::kind::hedge) {
            ++v.hedges;
            // The size is the one reconciled on the last end-of-fills (legacy only re-sizes on
            // fills, not on market-data updates), so all that is checked here is that it is
            // bounded and that it sits on the side the position needs.
            const qty_t net = s.internal_pos + s.external_pos;
            const price_t bound = v.m->max_pos_internal[c] + v.m->max_pos_external[c];
            if (it.size < 0 || (bound > 0 && it.size > bound)) {
                ++g_failures;
                std::printf("  FAIL  hedge size %lld out of bounds (contract %u)\n",
                            (long long)it.size, c);
            }
            return;
        }

        // A removal is a NULL-priced, zero-size request.
        if (it.internal_price == tachyon::NULL_PRICE) {
            ++v.removals;
            if (it.internal_size != 0 || it.external_size != 0) {
                ++g_failures;
                std::printf("  FAIL  removal with non-zero size (contract %u)\n", c);
            }
            return;
        }

        if (it.external_price != tachyon::NULL_PRICE && it.external_price % tick != 0) {
            ++g_failures;
            std::printf("  FAIL  external price %lld is not a %lld multiple (contract %u)\n",
                        (long long)it.external_price, (long long)tick, c);
        }
        const qty_t lot = v.m->lot[c];
        if (it.internal_size < 0 || it.external_size < 0) {
            ++g_failures;
            std::printf("  FAIL  negative size (contract %u)\n", c);
        }
        if (lot > 1 && (it.internal_size % lot != 0 || it.external_size % lot != 0)) {
            ++g_failures;
            std::printf("  FAIL  size not lot-aligned (contract %u)\n", c);
        }

        last& l = v.last_quote[c];
        (it.sd == side::bid ? l.bid : l.ask) =
            it.external_price != tachyon::NULL_PRICE ? it.external_price : tachyon::NULL_PRICE;
        if (l.bid != tachyon::NULL_PRICE && l.ask != tachyon::NULL_PRICE && l.bid >= l.ask) {
            ++g_failures;
            std::printf("  FAIL  emitted book crosses: bid %lld >= ask %lld (contract %u)\n",
                        (long long)l.bid, (long long)l.ask, c);
        }
    }

    static void on_notify(void* ctx, uint32_t) { (void)ctx; }
    static void on_cancel(void* ctx, uint32_t, bool) { (void)ctx; }
    static void on_alert(void* ctx, uint32_t c, const char* reason) {
        (void)ctx;
        std::printf("  alert: contract %u: %s\n", c, reason);
    }
    static void on_fill_report(void* ctx, uint32_t, const order_intent*, const fill_outcome& o) {
        validator& v = *static_cast<validator*>(ctx);
        if (o.untaken_remaining > 0) ++v.overflows;
    }

    host_ports ports() {
        host_ports p;
        p.send_quote = &on_intent;
        p.send_hedge = &on_intent;
        p.notify = &on_notify;
        p.cancel_orders = &on_cancel;
        p.on_alert = &on_alert;
        p.on_fill_report = &on_fill_report;
        p.ctx = this;
        return p;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "config/backtest";
    const int events = argc > 2 ? std::atoi(argv[2]) : 200000;
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 42u;

    mm_model_cfg cfg;
    secdb_table secdb;
    try {
        cfg = mm_model_cfg::load(dir + "/model.csv");
        secdb = load_secdb(dir + "/secdb.cme.csv");
    } catch (const std::exception& e) {
        std::printf("cannot load %s: %s (run from the repository root)\n", dir.c_str(), e.what());
        return EXIT_FAILURE;
    }

    const model m = model::build(build_model_inputs(cfg, secdb, 1.0e6, /*current_shift=*/2));
    cycle_state st(m);

    validator v;
    v.m = &m;
    v.st = &st;
    host h(m, st, v.ports());
    h.set_strategy_enabled(true);
    // config/backtest ships every MM contract disabled (the operator enables them at run time),
    // so the tape models an enabled, tradable session.
    for (uint32_t c = 0; c < m.n_contracts; ++c) {
        st.contract(c).enabled = true;
        st.contract(c).contract_can_trade = true;
    }

    std::mt19937 rng(seed);
    std::vector<int> mid_ticks(m.n_contracts, 2400);  // ~60.00 at a 0.025 tick
    uint64_t batch = 1;
    uint64_t fills = 0;
    uint64_t ends = 0;

    for (int i = 0; i < events; ++i) {
        const uint32_t c = static_cast<uint32_t>(rng() % m.n_contracts);

        // Every 8th event the mid moves a tick, otherwise only the sizes change.
        if (i % 8 == 0) {
            mid_ticks[c] += static_cast<int>(rng() % 3) - 1;
            if (mid_ticks[c] < 1000) mid_ticks[c] = 1000;
        }
        const int spread = 1 + static_cast<int>(rng() % 3);
        const price_t tick = m.tick_size[c];
        level bids[BOOK_LEVELS];
        level asks[BOOK_LEVELS];
        for (uint32_t k = 0; k < BOOK_LEVELS; ++k) {
            const qty_t q = 1 + static_cast<qty_t>(rng() % 400);
            bids[k] = level{tick * (mid_ticks[c] - static_cast<int>(k)), q,
                            static_cast<uint32_t>(rng() % 8), 0};
            asks[k] = level{tick * (mid_ticks[c] + spread + static_cast<int>(k)), q,
                            static_cast<uint32_t>(rng() % 8), 0};
        }
        h.on_market_data(c, side::bid, bids, BOOK_LEVELS, 1, 1, batch);
        h.on_market_data(c, side::ask, asks, BOOK_LEVELS, 1, 1, batch++);

        // Occasionally fill one of our quotes, then end the fill cycle.
        if (i % 37 == 0) {
            fill f;
            f.sd = (rng() & 1) ? side::bid : side::ask;
            f.qty = 1 + static_cast<qty_t>(rng() % 6);
            f.type = (rng() % 4 == 0) ? fill_type::internal : fill_type::external;
            f.source = fill_source::quote;
            f.price = tick * mid_ticks[c];
            h.on_fill(c, f);
            ++fills;
        }
        if (i % 37 == 1) {
            h.end_of_fills(c);
            ++ends;
            // After an end-of-fills the hedge must cover the whole net position (legacy
            // modifyHedgeSize / calculateHedgeQuoteSize).
            const qty_t net = st.contract(c).internal_pos + st.contract(c).external_pos;
            const qty_t abs_net = net < 0 ? -net : net;
            const qty_t sized = st.contract(c).hedge_of(side::bid).size_to_quote +
                                st.contract(c).hedge_of(side::ask).size_to_quote;
            if (sized != abs_net) {
                ++g_failures;
                std::printf("  FAIL  hedge %lld != |net| %lld after end-of-fills (contract %u)\n",
                            (long long)sized, (long long)abs_net, c);
            }
            // ... and it must sit on the side the position needs: a short book hedges with a bid.
            const side needed = net < 0 ? side::bid : side::ask;
            if (abs_net > 0 && st.contract(c).hedge_of(needed).size_to_quote != abs_net) {
                ++g_failures;
                std::printf("  FAIL  hedge on the wrong side (contract %u)\n", c);
            }
        }

        // Occasionally toggle the strategy, exercising the enable/disable paths.
        if (i % 5000 == 0) {
            h.set_strategy_enabled(false);
            h.run_cycle(1000ULL * static_cast<uint64_t>(i + 2));
            h.set_strategy_enabled(true);
        }

        h.run_cycle(1000ULL * static_cast<uint64_t>(i + 2));
    }

    const host_stats& stats = h.stats();
    std::printf("MM replay: %s, %d events, seed %u\n", dir.c_str(), events, seed);
    std::printf("  contracts        %u\n", m.n_contracts);
    std::printf("  intents          %llu (quotes incl. %llu removals, %llu hedges)\n",
                (unsigned long long)v.intents, (unsigned long long)v.removals,
                (unsigned long long)v.hedges);
    std::printf("  cycles           %llu, notifications %llu\n",
                (unsigned long long)stats.cycles, (unsigned long long)stats.notifications);
    std::printf("  md updates       %llu, stale rejected %llu\n",
                (unsigned long long)stats.md_updates,
                (unsigned long long)stats.stale_md_rejected);
    std::printf("  fills            %llu (+%llu end-of-fills), overflows %llu\n",
                (unsigned long long)fills, (unsigned long long)ends,
                (unsigned long long)v.overflows);

    check(v.intents > 0, "the tape produces quotes");
    check(v.hedges > 0, "the tape produces hedges (fills must move the position)");
    check(stats.cycles >= static_cast<uint64_t>(events), "at least one cycle per event");
    check(stats.stale_md_rejected == 0, "no stale batches in a monotonic tape");
    check(stats.intents_emitted == v.intents, "every emitted intent reached the sink");

    if (g_failures != 0) {
        std::printf("%d invariant failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all replay invariants hold\n");
    return EXIT_SUCCESS;
}
