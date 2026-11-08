// Phase-6 gate: the fill state machine (position_book) and the host — the cycle driver, its gates,
// the maxTicksWide latch, the deferred-emission path and the fill integration.
//
//   mm_host_tests

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "book.hpp"
#include "host.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "positions.hpp"
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

void check_eq(long long got, long long want, const std::string& what) {
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s (got %lld, want %lld)\n", what.c_str(), got, want);
    }
}

constexpr price_t TICK = 25'000'000;
price_t px(int ticks) { return TICK * ticks; }

contract_input make_input() {
    contract_input c;
    c.name = "QMQ1-QMU1";
    c.tick_size = TICK;
    c.lot = 1;
    c.enabled = true;
    c.quote_external = true;
    c.quote_size = 8;
    c.quote_gap = 5;
    c.pct_tob_max_quote = 0.2;
    c.width_pct = {0.1, 0.1, 0.1, 0.1};
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
    c.max_ticks_wide = 6;  // six ticks: latch above, disable above twelve
    c.worsen_payup_pct = 0.65477777777;
    c.queue_importance = 0;
    return c;
}

constexpr double FREQUENCY = 1.0e6;

model make_model(const contract_input& c) {
    model_inputs in;
    in.contracts.push_back(c);
    in.cycles_per_second = FREQUENCY;
    in.current_shift = 2;
    return model::build(in);
}

level make_level(int ticks, qty_t qty, uint32_t orders = 5) {
    return level{px(ticks), qty, orders, 0};
}

// Records everything the host emits, so a test can assert on the sequence and count notifications.
struct recorder {
    std::vector<order_intent> intents;
    uint32_t notifications = 0;
    uint32_t cancels = 0;
    uint32_t alerts = 0;
    std::vector<fill_outcome> fills;

    static void on_quote(void* ctx, const order_intent& it) {
        static_cast<recorder*>(ctx)->intents.push_back(it);
    }
    static void on_hedge(void* ctx, const order_intent& it) {
        static_cast<recorder*>(ctx)->intents.push_back(it);
    }
    static void on_notify(void* ctx, uint32_t) { ++static_cast<recorder*>(ctx)->notifications; }
    static void on_cancel(void* ctx, uint32_t, bool) { ++static_cast<recorder*>(ctx)->cancels; }
    static void on_alert(void* ctx, uint32_t, const char*) { ++static_cast<recorder*>(ctx)->alerts; }
    static void on_fill_report(void* ctx, uint32_t, const order_intent*, const fill_outcome& o) {
        static_cast<recorder*>(ctx)->fills.push_back(o);
    }

    host_ports ports() {
        host_ports p;
        p.send_quote = &on_quote;
        p.send_hedge = &on_hedge;
        p.notify = &on_notify;
        p.cancel_orders = &on_cancel;
        p.on_alert = &on_alert;
        p.on_fill_report = &on_fill_report;
        p.ctx = this;
        return p;
    }

    void clear() {
        intents.clear();
        notifications = 0;
        cancels = 0;
        alerts = 0;
        fills.clear();
    }
};

// ---------------------------------------------------------------------------
// fills: allocation against positions and quote sizes (legacy :1013-1072)
// ---------------------------------------------------------------------------
void test_fill_allocation() {
    std::printf("host: fill allocation\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    position_book book(m);
    contract_state& s = st.contract(0);

    // An internal quote fill with no opposite position opens an internal long and consumes the
    // quote's internal size.
    s.quote_of(side::bid).internal_size = 8;
    fill f;
    f.sd = side::bid;
    f.qty = 5;
    f.type = fill_type::internal;
    f.source = fill_source::quote;
    fill_outcome out = book.on_fill(s, 0, f);
    check_eq(s.internal_pos, 5, "internal position opens by the fill");
    check_eq(s.quote_of(side::bid).internal_size, 3, "quote size is consumed");
    check_eq(out.allocated_internal, 5, "allocation reported internal");
    check_eq(s.external_pos, 0, "external position untouched");

    // A further internal fill beyond what we were showing books the excess as external (:1023-1030).
    f.qty = 10;
    out = book.on_fill(s, 0, f);
    check_eq(s.quote_of(side::bid).internal_size, 0, "internal size exhausted");
    check_eq(s.internal_pos, 8, "internal position only takes what was shown");
    check_eq(s.external_pos, 7, "the 7-lot excess is booked external");
    check_eq(out.allocated_internal, 3, "3 lots allocated internal");
    check_eq(out.allocated_external, 7, "7 lots allocated external");

    // An external quote fill only touches the external position — but only after offsetting any
    // opposite position, so the internal long from above is cleared first.
    s.internal_pos = 0;
    s.external_pos = 0;
    s.quote_of(side::ask).external_size = 8;
    f.sd = side::ask;
    f.qty = 3;
    f.type = fill_type::external;
    out = book.on_fill(s, 0, f);
    check_eq(s.external_pos, -3, "an ask fill opens a short external position");
    check_eq(s.quote_of(side::ask).external_size, 5, "external size consumed");
    check_eq(out.allocated_external, 3, "allocated external");

    // A fill larger than the shown external size clamps the size at zero rather than going
    // negative (:1060-1066).
    f.qty = 100;
    book.on_fill(s, 0, f);
    check_eq(s.quote_of(side::ask).external_size, 0, "external size clamps at zero");
    check_eq(s.external_pos, -103, "but the position takes the whole fill");

    // A hedge fill offsets the existing position first, then opens a new one.
    s.internal_pos = 0;
    s.external_pos = 0;
    s.internal_pos = 10;
    f.sd = side::ask;
    f.qty = 4;
    f.source = fill_source::hedge;
    f.type = fill_type::internal;
    out = book.on_fill(s, 0, f);
    check_eq(s.internal_pos, 6, "the hedge offsets the internal long");
    check_eq(out.allocated_internal, -4, "the allocation carries the fill's sign");
}

// ---------------------------------------------------------------------------
// overfills (legacy takeOverFill :238-317, forceTakeOverFill :318-340)
// ---------------------------------------------------------------------------
void test_overfill() {
    std::printf("host: overfill\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    position_book book(m);
    contract_state& s = st.contract(0);

    // An overfill is taken as external exposure, up to the external position limit.
    s.external_pos = 0;
    fill f;
    f.sd = side::bid;
    f.qty = 100;
    f.type = fill_type::external;
    f.source = fill_source::overfill;
    fill_outcome out = book.on_fill(s, 0, f);
    check_eq(s.external_pos, 100, "the overfill becomes external exposure");
    check_eq(out.reported_size, 100, "and is reported whole");
    check_eq(out.untaken_remaining, 0, "nothing left over");

    // Beyond the limit: take what fits and report the remainder (:264-282).
    f.qty = 500;
    out = book.on_fill(s, 0, f);
    check_eq(s.external_pos, 350, "capped at maxPosExternal");
    check_eq(out.untaken_remaining, 500 - 250, "the rest is reported as untaken");
    check_eq(out.reported_size, 250, "only the taken part is reported to the front end");

    // At the limit nothing is taken at all.
    out = book.on_fill(s, 0, f);
    check_eq(s.external_pos, 350, "position unchanged at the limit");
    check_eq(out.reported_size, 0, "nothing reported");
    check_eq(out.untaken_remaining, 500, "the whole fill is untaken");

    // A forced overfill ignores the limit and takes everything (:318-340).
    f.force_over_fill = true;
    out = book.on_fill(s, 0, f);
    check_eq(s.external_pos, 850, "a forced overfill takes the whole size");
    check_eq(out.untaken_remaining, 0, "nothing left");

    // ... and disables external quoting when the limit check is requested.
    fill fc = f;
    fc.check_limit = true;
    fc.force_over_fill = true;
    out = book.on_fill(s, 0, fc);
    check(out.disable_external_quoting, "the limit breach asks for external quoting to stop");
}

// ---------------------------------------------------------------------------
// position limits (legacy isWithinPositionLimits :1211-1225)
// ---------------------------------------------------------------------------
void test_position_limits() {
    std::printf("host: position limits\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    position_book book(m);
    contract_state& s = st.contract(0);

    check(book.within_limits(s, 0), "flat is within limits");
    s.internal_pos = 1500;
    check(book.within_limits(s, 0), "sitting exactly on the internal limit is within it");
    s.internal_pos = 1501;
    check(!book.within_limits(s, 0), "one lot over breaks it");
    s.internal_pos = 0;
    s.external_pos = -351;
    check(!book.within_limits(s, 0), "the external side is checked with its sign ignored");
}

// ---------------------------------------------------------------------------
// the host cycle: dirty tracking, stale batches, notifications
// ---------------------------------------------------------------------------
void test_host_cycle() {
    std::printf("host: market-data cycle\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    recorder rec;
    host h(m, st, rec.ports());
    h.set_strategy_enabled(true);
    contract_state& s = st.contract(0);
    s.contract_can_trade = true;

    level bids[BOOK_LEVELS];
    level asks[BOOK_LEVELS];
    bids[0] = make_level(100, 1000);
    asks[0] = make_level(102, 1000);

    check(h.on_market_data(0, side::bid, bids, 1, 1, 1, 1), "bid snapshot accepted");
    check(h.on_market_data(0, side::ask, asks, 1, 1, 1, 1), "ask snapshot accepted");
    check_eq(st.changed_count(), 1, "the contract is dirty once, not twice");
    h.run_cycle(10);
    check(!rec.intents.empty(), "the first cycle quotes");
    check_eq(rec.notifications, 1, "one notification per contract per cycle");
    check_eq(st.changed_count(), 0, "the dirty set is cleared");

    // An equal or older batch is stale (legacy MarketMaker_MD.cpp:13-19).
    rec.clear();
    check(!h.on_market_data(0, side::bid, bids, 1, 1, 1, 1), "an equal batch is stale");
    check_eq(h.stats().stale_md_rejected, 1, "stale rejections are counted");
    h.run_cycle(20);
    check_eq(rec.notifications, 0, "no work without a fresh batch");

    // A price-only update (qty_num == 0) records the index but does not copy the snapshot
    // (copyBooks only memcpy's on a quantity change).
    const qty_t before = s.side_book(side::bid).top().qty;
    check(h.on_market_data(0, side::bid, bids, 1, 1, 0, 2), "price-only update accepted");
    check_eq(s.side_book(side::bid).top().qty, before, "the snapshot was not copied");
    check_eq(s.side_book(side::bid).price_change_index(), 0, "but the price index was recorded");
    h.run_cycle(30);
}

// ---------------------------------------------------------------------------
// the maxTicksWide latch (legacy :452-482)
// ---------------------------------------------------------------------------
void test_max_ticks_wide() {
    std::printf("host: maxTicksWide latch\n");
    const model m = make_model(make_input());  // maxTicksWide = 6 ticks
    cycle_state st(m);
    recorder rec;
    host h(m, st, rec.ports());
    h.set_strategy_enabled(true);
    contract_state& s = st.contract(0);
    s.contract_can_trade = true;

    level bids[BOOK_LEVELS];
    level asks[BOOK_LEVELS];
    uint64_t batch = 1;

    // A normal book quotes normally and keeps external quoting on.
    bids[0] = make_level(100, 1000);
    asks[0] = make_level(102, 1000);
    h.on_market_data(0, side::bid, bids, 1, 1, 1, batch);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, batch++);
    h.run_cycle(10);
    check(s.quote_external, "external quoting stays on for a normal spread");
    check(!s.max_ticks_wide_breached, "no latch");

    // Seven ticks: above maxTicksWide but below 2x. External quoting stops and both quotes are
    // pulled explicitly.
    rec.clear();
    asks[0] = make_level(107, 1000);
    h.on_market_data(0, side::bid, bids, 1, 1, 1, batch);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, batch++);
    h.run_cycle(20);
    check(s.max_ticks_wide_breached, "the latch is set");
    check(!s.quote_external, "external quoting is disabled");
    check(s.enabled, "but the contract keeps quoting internally");
    check_eq(h.stats().max_ticks_wide_latches, 1, "the latch is counted once");
    uint32_t removals = 0;
    for (const order_intent& it : rec.intents) {
        if (it.type == order_intent::kind::quote && it.internal_price == tachyon::NULL_PRICE) {
            ++removals;
        }
    }
    check_eq(removals, 2, "both quotes are pulled on the transition");
    check(rec.alerts >= 1, "and an alert is raised");

    // Back inside the band: the latch clears.
    rec.clear();
    asks[0] = make_level(102, 1000);
    h.on_market_data(0, side::bid, bids, 1, 1, 1, batch);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, batch++);
    h.run_cycle(30);
    check(!s.max_ticks_wide_breached, "the latch clears");

    // Thirteen ticks: beyond 2x, so the contract is disabled outright.
    rec.clear();
    asks[0] = make_level(113, 1000);
    h.on_market_data(0, side::bid, bids, 1, 1, 1, batch);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, batch++);
    h.run_cycle(40);
    check(!s.enabled, "the contract is disabled at 2x maxTicksWide");
    check_eq(rec.cancels, 1, "and its orders are cancelled");
    check(!s.quote_external, "external quoting is off too");
}

// ---------------------------------------------------------------------------
// gating: strategy enable/disable, contract enable, deferred emission
// ---------------------------------------------------------------------------
void test_gating() {
    std::printf("host: gating and deferral\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    recorder rec;
    host h(m, st, rec.ports());
    contract_state& s = st.contract(0);
    s.contract_can_trade = true;

    level bids[BOOK_LEVELS];
    level asks[BOOK_LEVELS];
    bids[0] = make_level(100, 1000);
    asks[0] = make_level(102, 1000);

    // Strategy disabled: the cycle runs but emits nothing.
    h.set_strategy_enabled(false);
    h.on_market_data(0, side::bid, bids, 1, 1, 1, 1);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, 1);
    h.run_cycle(10);
    check(rec.intents.empty(), "a disabled strategy emits nothing");

    // Enabled: it quotes.
    h.set_strategy_enabled(true);
    h.on_market_data(0, side::bid, bids, 1, 1, 1, 2);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, 2);
    h.run_cycle(20);
    check(!rec.intents.empty(), "an enabled strategy quotes");

    // canSendQuotes false: the cycle is deferred and the contract stays dirty.
    rec.clear();
    h.set_can_send_quotes(false);
    asks[0] = make_level(101, 900);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, 3);
    h.run_cycle(30);
    check(rec.intents.empty(), "nothing is emitted while quotes cannot be sent");
    check_eq(st.changed_count(), 1, "the contract stays dirty for the next attempt");

    // Re-enabled: the retained change is emitted on the next cycle.
    h.set_can_send_quotes(true);
    h.run_cycle(40);
    check(!rec.intents.empty(), "the deferred change is emitted once sending resumes");

    // A contract that cannot trade (an exchange halt) emits nothing.
    rec.clear();
    s.contract_can_trade = false;
    h.on_market_data(0, side::bid, bids, 1, 1, 1, 4);
    h.run_cycle(50);
    check(rec.intents.empty(), "a halted contract emits nothing");
}

// ---------------------------------------------------------------------------
// fills end to end: the fill moves the position, end_of_fills re-hedges
// ---------------------------------------------------------------------------
void test_fill_rehedge() {
    std::printf("host: fill then re-hedge\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    recorder rec;
    host h(m, st, rec.ports());
    h.set_strategy_enabled(true);
    contract_state& s = st.contract(0);
    s.contract_can_trade = true;

    level bids[BOOK_LEVELS];
    level asks[BOOK_LEVELS];
    bids[0] = make_level(100, 1000);
    asks[0] = make_level(102, 1000);
    h.on_market_data(0, side::bid, bids, 1, 1, 1, 1);
    h.on_market_data(0, side::ask, asks, 1, 1, 1, 1);
    h.run_cycle(10);
    check_eq(s.internal_pos, 0, "flat to begin with");
    check(!s.is_hedging(side::bid) && !s.is_hedging(side::ask), "no hedge while flat");

    // We buy 5 internally through our own quote.
    s.quote_of(side::bid).internal_size = 8;
    fill f;
    f.sd = side::bid;
    f.qty = 5;
    f.type = fill_type::internal;
    f.source = fill_source::quote;
    rec.clear();
    h.on_fill(0, f);
    check_eq(s.internal_pos, 5, "the fill opens a long");
    check_eq(rec.fills.size(), 1, "the fill is reported to the front end");

    // End of fills: the hedge is sized to the new net position and everything is refreshed.
    rec.clear();
    h.end_of_fills(0);
    check_eq(s.hedge_of(side::ask).size_to_quote, 5, "a long position hedges with an ask of 5");
    check(!rec.intents.empty(), "the cycle emits the new hedge");
    bool saw_hedge = false;
    for (const order_intent& it : rec.intents) {
        if (it.type == order_intent::kind::hedge && it.sd == side::ask && it.size == 5) {
            saw_hedge = true;
        }
    }
    check(saw_hedge, "the ask hedge of 5 is in the emitted sequence");

    // A fill handed over by another strategy is taken by force and ends the cycle inline.
    s.internal_pos = 0;
    s.external_pos = 0;
    s.hedge_of(side::ask).reset();
    fill transferred = f;
    transferred.source = fill_source::rv_disable;
    transferred.from_other_strategy = true;
    transferred.qty = 4;
    rec.clear();
    const fill_outcome out = h.on_fill(0, transferred);
    check_eq(s.external_pos, 4, "the transferred fill is taken");
    check(out.ends_cycle_inline, "and ends the cycle inline");
    check(!rec.intents.empty(), "the inline cycle emitted the re-hedge");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_fill_allocation();
    test_overfill();
    test_position_limits();
    test_host_cycle();
    test_max_ticks_wide();
    test_gating();
    test_fill_rehedge();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all MM phase-6 checks passed\n");
    return EXIT_SUCCESS;
}

