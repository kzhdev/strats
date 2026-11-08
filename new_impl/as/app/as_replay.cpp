// Runnable driver for the new AS engine: loads a real config directory, replays a deterministic
// tape through the host, and asserts the deployment invariants.
//
//   as_replay <config-dir> [cycles] [seed]
//
// Exit code 0 only when every invariant holds, so it can be a smoke gate.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <common/liquidity_ledger.hpp>

#include "config.hpp"
#include "host.hpp"
#include "ledger_bridge.hpp"
#include "model.hpp"
#include "state.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::as;
using tachyon::trade::EXTERNAL;
using tachyon::trade::INTERNAL;
using tachyon::trade::liquidity_ledger;
using tachyon::trade::liquidity_source;
using tachyon::trade::make_market_key;
using tachyon::trade::market_key;
using tachyon::trade::order_side;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  %-56s pass\n", what);
    }
}

// Everything the tape needs to know about what the host emitted.
struct tape_sink {
    uint64_t quotes = 0;
    uint64_t notifications = 0;
    uint64_t hedges = 0;
    uint64_t switch_fills = 0;
    uint64_t fills_forwarded = 0;
    uint64_t alerts = 0;
    // (hedge_id, market) pairs awaiting a done ack, so the tape can drain them
    std::vector<std::pair<uint32_t, market_key>> pending;
    host* h = nullptr;
};

void sink_quote(void* ctx, const quote_request&) {
    ++static_cast<tape_sink*>(ctx)->quotes;
}
void sink_notify(void* ctx, uint32_t) { ++static_cast<tape_sink*>(ctx)->notifications; }
void sink_hedge(void* ctx, const hedge_order& o) {
    tape_sink* s = static_cast<tape_sink*>(ctx);
    ++s->hedges;
    s->pending.emplace_back(o.hedge_id, o.target);
}
void sink_fill(void*, uint32_t, side, qty_t, price_t) {}
void sink_switch(void* ctx, const hedge_plan&) { ++static_cast<tape_sink*>(ctx)->switch_fills; }
void sink_forward(void* ctx, uint32_t, side, qty_t) {
    ++static_cast<tape_sink*>(ctx)->fills_forwarded;
}
void sink_alert(void* ctx, const char*) { ++static_cast<tape_sink*>(ctx)->alerts; }
void sink_cancel(void*) {}

}  // namespace

namespace {

int run(const std::string& dir, int cycles, unsigned seed) {
    const as_model_cfg cfg = as_model_cfg::load(dir + "/AS_model.csv", dir + "/AS_paths.csv",
                                                dir + "/secdb.cme.csv");
    const model m = model::build(build_model_inputs(cfg));
    cycle_state s(m);
    liquidity_ledger ledger;
    tape_sink sink;
    host_ports ports;
    ports.send_quote = &sink_quote;
    ports.notify_quote = &sink_notify;
    ports.send_hedge = &sink_hedge;
    ports.send_fill = &sink_fill;
    ports.send_switch_fill = &sink_switch;
    ports.forward_fill = &sink_forward;
    ports.on_alert = &sink_alert;
    ports.cancel_all = &sink_cancel;
    ports.ctx = &sink;
    host h(m, s, &ledger, ports);
    sink.h = &h;

    for (uint32_t c = 0; c < m.n_all; ++c) {
        for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
            const side sd = static_cast<side>(si);
            h.wire_market(c, sd, true, make_market_key(c, static_cast<order_side>(si), EXTERNAL),
                          liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
            h.wire_market(c, sd, false, make_market_key(c, static_cast<order_side>(si), INTERNAL),
                          liquidity_source::LIQUIDITY_SOURCE_RV_QUOTE);
        }
    }

    std::mt19937 rng(seed);
    auto pick = [&rng](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };

    // a rolling mid per *outright*, and a fair value per contract derived from its legs, so the
    // book is coherent: a spread trades at the combination of its legs, not at a level of its own
    std::vector<price_t> mid(static_cast<size_t>(m.n_all));
    std::vector<price_t> fair(static_cast<size_t>(m.n_all));
    for (uint32_t c = 0; c < m.n_all; ++c) mid[c] = m.tick[c] * 2400;
    auto recompute_fair = [&]() {
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (m.leg_count(c) == 0) {
                fair[c] = mid[c];
                continue;
            }
            price_t p = 0;
            for (uint32_t k = m.leg_begin[c]; k < m.leg_begin[c + 1]; ++k) {
                p += static_cast<price_t>(m.leg_ratio[k]) * mid[m.leg_index[k]];
            }
            fair[c] = p;
        }
    };
    recompute_fair();

    uint64_t fills = 0;
    uint64_t stf_updates = 0;
    uint64_t size_violations = 0;

    for (int i = 0; i < cycles; ++i) {
        // drift one outright and republish every contract's two-sided book around its fair value
        uint32_t outright = 0;
        while (outright < m.n_all && m.leg_count(outright) != 0) ++outright;
        const uint32_t count_outrights = outright;
        if (count_outrights > 0) {
            const uint32_t o = static_cast<uint32_t>(pick(0, static_cast<int>(count_outrights) - 1));
            mid[o] += m.tick[o] * pick(-1, 1);
            if (mid[o] < m.tick[o] * 4) mid[o] = m.tick[o] * 4;
        }
        recompute_fair();

        for (uint32_t c = 0; c < m.n_all; ++c) {
            const price_t tick = m.tick[c];
            h.on_market_data(c, side::bid, true, fair[c] - tick,
                             static_cast<qty_t>(pick(1, 12)));
            h.on_market_data(c, side::ask, true, fair[c] + tick,
                             static_cast<qty_t>(pick(1, 12)));
            // occasionally the internal book shows a better price, so the best-book logic moves
            if (pick(0, 9) == 0) {
                h.on_market_data(c, side::bid, false, fair[c] - 2 * tick,
                                 static_cast<qty_t>(pick(1, 8)));
            }
        }

        // a fair value every few cycles
        if (pick(0, 9) == 0) {
            h.on_stf(static_cast<uint32_t>(pick(0, static_cast<int>(m.n_all) - 1)),
                     fair[0] + m.tick[0] * pick(-3, 3));
            ++stf_updates;
        }

        h.run_cycle(static_cast<uint64_t>(i));

        // sometimes a quote gets filled
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (!m.is_quoting(c) || pick(0, 12) != 0) continue;
            const side fill_side = pick(0, 1) == 0 ? side::bid : side::ask;
            if (s.quote_of(c, fill_side).internal_size > 0) {
                h.on_fill(c, fill_side, 1, pick(0, 1) == 0);
                ++fills;
            }
        }

        // drain the hedge acks so the reservations cannot pile up
        for (const auto& pending : sink.pending) {
            h.on_hedge_ack(pending.first, pending.second, 1, 1);
            h.on_hedge_done(pending.first, pending.second);
        }
        sink.pending.clear();

        // invariant: an adjusted size can never exceed the displayed size
        for (uint32_t k = 0; k < m.n_all; ++k) {
            for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
                const side bk = static_cast<side>(si);
                const qty_t displayed = s.book(k, bk, true).size;
                const qty_t adjusted = s.adjusted_size(
                    ledger_bridge::make_policy(*h.bridge()), k, bk, true);
                if (adjusted > displayed) ++size_violations;
            }
        }
    }

    const host_stats& stats = h.stats();
    std::printf(
        "\ncycles=%d  quotes=%llu  notifications=%llu  hedges=%llu  fills=%llu  stf=%llu\n"
        "alerts=%llu  disabled=%s  reservations_taken=%llu  active=%zu\n\n",
        cycles, (unsigned long long)stats.quotes_emitted,
        (unsigned long long)stats.quote_notifications, (unsigned long long)stats.hedge_legs,
        (unsigned long long)fills, (unsigned long long)stf_updates,
        (unsigned long long)stats.validation_failures, stats.disabled ? "yes" : "no",
        (unsigned long long)stats.reservations_taken, ledger.active_reservation_count());

    std::printf("invariants\n");
    check(static_cast<int>(stats.cycles) == cycles, "every requested cycle ran");
    check(stats.quotes_emitted > 0, "quotes were emitted");
    check(stats.quote_notifications > 0, "quote notifications were raised");
    check(fills > 0, "the tape filled quotes");
    check(stats.hedge_legs > 0, "the fills were hedged");
    check(stats.reservations_taken > 0, "every hedge reserved liquidity");
    check(ledger.active_reservation_count() == 0,
          "all reservations drained after the acks (no liquidity leak)");
    check(size_violations == 0, "an adjusted size never exceeded the displayed size");
    check(!stats.disabled, "the run did not have to disable the strategy");
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "config/minimal_cme";
    const int cycles = argc > 2 ? std::atoi(argv[2]) : 500;
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 42u;
    std::printf("as_replay: %s  cycles=%d seed=%u\n", dir.c_str(), cycles, seed);
    return run(dir, cycles, seed);
}

