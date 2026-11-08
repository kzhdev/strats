// Phase-5 gate: the hedger (size reconciliation, hedge pricing, payup) and the full cycle
// assembly (on_md_change / apply_updates) including both cross checks.
//
//   mm_cycle_tests
//
// The gate compares the **ordered request sequence** the cycle produces — not just the final
// prices — because ordering is behaviour: the quote/quote cross check re-emits the other side, a
// hedge flip resets the opposite hedge, and the hedge/payup re-send is unconditional.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "book.hpp"
#include "hedging.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "strategy.hpp"
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
    c.width_pct = {3.0, 2.0, 0.1, 0.1};
    c.hedge_offset = 1.0;   // one tick
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
    c.max_ticks_wide = -1;
    c.worsen_payup_pct = 0.65477777777;
    c.queue_importance = 0;  // the cycle is what is under test here, not the queue model
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

void set_top(contract_state& s, int bid_ticks, qty_t bid_qty, int ask_ticks, qty_t ask_qty,
             uint32_t bid_orders = 5, uint32_t ask_orders = 5) {
    level bids[BOOK_LEVELS];
    level asks[BOOK_LEVELS];
    bids[0] = level{px(bid_ticks), bid_qty, bid_orders, 0};
    asks[0] = level{px(ask_ticks), ask_qty, ask_orders, 0};
    s.side_book(side::bid).init(side::bid);
    s.side_book(side::bid).apply_snapshot(bids, 1);
    s.side_book(side::ask).init(side::ask);
    s.side_book(side::ask).apply_snapshot(asks, 1);
}



// ---------------------------------------------------------------------------
// hedge size reconciliation (legacy :1102-1215)
// ---------------------------------------------------------------------------
void test_hedge_size_reconcile() {
    std::printf("cycle: hedge size reconciliation\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    strategy strat(m);
    contract_state& s = st.contract(0);
    intent_log log;

    // Short 10 lots: the hedge has to buy 10.
    s.internal_pos = -10;
    log.clear();
    check(strat.hedge().reconcile_size(s, log, 0), "a new hedge is a change");
    check_eq(log.size(), 1, "one hedge intent");
    check(log.at(0).type == order_intent::kind::hedge, "it is a hedge");
    check(log.at(0).sd == side::bid, "a short position hedges with a bid");
    check_eq(log.at(0).size, 10, "for the whole net position");
    check(log.at(0).persist, "hedges are persistent LMT orders");
    check_eq(s.hedge_of(side::bid).size_to_quote, 10, "the bid hedge carries the size");

    // The same position again: nothing changes.
    log.clear();
    check(!strat.hedge().reconcile_size(s, log, 0), "same size -> no change");
    check_eq(log.size(), 0, "so nothing is emitted");

    // Flip to long: the ask hedge appears first, then the bid hedge is explicitly zeroed.
    s.internal_pos = 10;
    log.clear();
    check(strat.hedge().reconcile_size(s, log, 0), "the flip is a change");
    check_eq(log.size(), 2, "two intents: the new side and the reset one");
    check(log.at(0).sd == side::ask && log.at(0).size == 10, "ask hedge sized 10 comes first");
    check(log.at(1).sd == side::bid && log.at(1).size == 0, "the bid hedge is reset to 0");
    check_eq(s.hedge_of(side::bid).size_to_quote, 0, "bid hedge cleared");

    // Back to flat: the remaining side is pulled.
    s.internal_pos = 0;
    log.clear();
    check(strat.hedge().reconcile_size(s, log, 0), "flat is a change");
    check_eq(log.size(), 1, "only the side that had size is re-sent");
    check(log.at(0).sd == side::ask && log.at(0).size == 0, "ask hedge reset to 0");

    // Flat and already pulled: no request at all.
    log.clear();
    check(!strat.hedge().reconcile_size(s, log, 0), "nothing to do when flat");
    check_eq(log.size(), 0, "no intents");

    // Internal and external positions net out.
    s.internal_pos = 40;
    s.external_pos = -40;
    log.clear();
    check(!strat.hedge().reconcile_size(s, log, 0), "net zero position -> no hedge");
    check_eq(log.size(), 0, "no intents");
}

// ---------------------------------------------------------------------------
// hedge pricing (legacy :1147-1163)
// ---------------------------------------------------------------------------
void test_hedge_prices() {
    std::printf("cycle: hedge pricing\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    strategy strat(m);
    contract_state& s = st.contract(0);

    set_top(s, 100, 1000, 102, 1000);  // touch px(100) / px(102)
    s.mid_point = px(101);
    s.width = 2'500'000;
    s.quote_of(side::ask).internal_price = px(102);
    s.hedge_of(side::bid).size_to_quote = 10;

    price_t internal_price = 0;
    price_t external_price = 0;
    const bool payup = strat.hedge().hedge_prices(s, 0, side::bid, internal_price, external_price);
    check(!payup, "a deep opposite book is not payup");
    // ref = min(our ask quote px(102), ask touch px(102)) = px(102); a bid steps back by the
    // hedge offset (one tick) to px(101); the mid is px(101), so the better bid is px(101).
    check_eq(internal_price, px(101), "buy hedge sits at the touch less one tick, at least mid");
    check_eq(external_price, m.round(0, internal_price, side::bid), "external is the rounded price");

    // An ask-side hedge mirrors it against the bid side.
    s.hedge_of(side::ask).size_to_quote = 10;
    s.hedge_of(side::bid).size_to_quote = 0;
    s.quote_of(side::bid).internal_price = px(100);
    check(!strat.hedge().hedge_prices(s, 0, side::ask, internal_price, external_price),
          "still not payup");
    // ref = max(our bid px(100), bid touch px(100)) = px(100); an ask steps up by one tick to
    // px(101); the better ask is the lower one, i.e. the mid px(101).
    check_eq(internal_price, px(101), "sell hedge sits at the touch plus one tick, at most mid");

    // Payup: the other side's book is thin, so the hedge chases its touch.
    contract_input cfg = make_input();
    cfg.payup_qty = 5000;
    const model m_payup = make_model(cfg);
    cycle_state st_payup(m_payup);
    strategy strat_payup(m_payup);
    contract_state& sp = st_payup.contract(0);
    set_top(sp, 100, 1000, 102, 1000);
    sp.mid_point = px(101);
    sp.hedge_of(side::bid).size_to_quote = 10;
    const bool payup_bid =
        strat_payup.hedge().hedge_prices(sp, 0, side::bid, internal_price, external_price);
    check(payup_bid, "a thin opposite book is payup");
    check_eq(external_price, px(102), "payup external price is the opposite touch");
    check_eq(internal_price, px(102) + m_payup.commission[0],
             "payup internal price is the touch less the commission for a bid");
}

// ---------------------------------------------------------------------------
// cross checks (legacy :512-570): after one instrument moves, the other must not cross it
// ---------------------------------------------------------------------------
void test_cross_checks() {
    std::printf("cycle: cross checks\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    strategy strat(m);
    contract_state& s = st.contract(0);
    intent_log log;

    set_top(s, 100, 1000, 102, 1000);
    s.mid_point = px(101);

    // ---- quote/quote (:513-537) ----
    // The bid was sent at px(101) and the ask now prices px(101) too: the bid must be re-sent.
    s.quote_of(side::bid).last_internal_price_sent = px(101);
    s.quote_of(side::bid).internal_price = px(101);
    s.quote_of(side::ask).internal_price = px(101);
    s.quote_of(side::ask).last_internal_price_sent = px(101);
    bool bid_changed = false;
    bool ask_changed = true;
    log.clear();
    strat.quotes().cross_quote(s, log, 0, true, bid_changed, ask_changed);
    check(bid_changed, "the untouched bid is forced out when it would cross the new ask");
    check_eq(log.size(), 1, "one forced quote");
    check(log.at(0).type == order_intent::kind::quote && log.at(0).sd == side::bid,
          "the forced quote is the bid");
    check(log.at(0).persist, "persist follows the external-quoting flag");
    check_eq(s.quote_of(side::bid).last_internal_price_sent, px(101), "and lastSent is refreshed");

    // Both sides changed: nothing is forced.
    bid_changed = true;
    ask_changed = true;
    log.clear();
    strat.quotes().cross_quote(s, log, 0, true, bid_changed, ask_changed);
    check_eq(log.size(), 0, "no cross check when both sides moved");

    // Neither changed: also nothing.
    bid_changed = false;
    ask_changed = false;
    log.clear();
    strat.quotes().cross_quote(s, log, 0, true, bid_changed, ask_changed);
    check_eq(log.size(), 0, "no cross check when neither side moved");

    // ---- hedge/quote (:539-570) ----
    // A buy hedge at px(101) against an ask quote sent at px(101): they touch. If the hedge is the
    // one that moved, the quote is pushed out; if the quote moved, the hedge is.
    s.hedge_of(side::bid).size_to_quote = 10;
    s.hedge_of(side::bid).internal_price = px(101);
    s.hedge_of(side::bid).in_payup = false;
    s.quote_of(side::ask).last_internal_price_sent = px(101);
    log.clear();
    check(strat.cross_hedge(s, log, 0, side::bid, true, /*hedge_changed=*/true,
                            /*other_side_quote_changed=*/false),
          "a hedge that moved onto the opposite quote forces that quote");
    check_eq(log.size(), 1, "one forced quote");
    check(log.at(0).sd == side::ask, "the ask quote is the one pushed out");

    log.clear();
    check(strat.cross_hedge(s, log, 0, side::bid, true, /*hedge_changed=*/false,
                            /*other_side_quote_changed=*/true),
          "a quote that moved onto the hedge forces the hedge");
    check_eq(log.size(), 1, "one forced hedge");
    check(log.at(0).type == order_intent::kind::hedge && log.at(0).sd == side::bid,
          "the hedge is the one pushed out");

    // A hedge in payup is chasing the touch on purpose, so it is exempt (:541-543).
    s.hedge_of(side::bid).in_payup = true;
    log.clear();
    check(!strat.cross_hedge(s, log, 0, side::bid, true, true, false),
          "a payup hedge is exempt from the cross check");
    check_eq(log.size(), 0, "no intents");
}

// ---------------------------------------------------------------------------
// the cycle sequence: on_md_change and apply_updates emit in the legacy order
// ---------------------------------------------------------------------------
void test_cycle_sequence() {
    std::printf("cycle: request sequence\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    strategy strat(m);
    contract_state& s = st.contract(0);
    order_queue q;
    intent_log log;

    set_top(s, 100, 1000, 102, 1000);
    s.internal_pos = -10;  // short: the hedge must buy 10

    // applyUpdates (:1194-1210): reconcile the hedge size first, then force both quotes out and
    // reprice the hedge.
    log.clear();
    const bool changed = strat.apply_updates(s, q, 0, /*quote_externally=*/true, log);
    check(changed, "the first update changes something");
    check(log.size() >= 3, "at least: size hedge, two quotes, priced hedge");
    check(log.at(0).type == order_intent::kind::hedge && log.at(0).sd == side::bid,
          "the size-only hedge comes first (modifyHedgeSize runs before the refresh)");
    check_eq(log.at(0).size, 10, "sized to the net position");
    check_eq(log.at(0).internal_price, tachyon::NULL_PRICE,
             "with no price yet, exactly like legacy before the repricing");

    uint32_t bids = 0;
    uint32_t asks = 0;
    for (uint32_t i = 0; i < log.size(); ++i) {
        if (log.at(i).type == order_intent::kind::quote) {
            (log.at(i).sd == side::bid) ? ++bids : ++asks;
        }
    }
    check_eq(bids, 1, "one bid quote request");
    check_eq(asks, 1, "one ask quote request");
    check(log.at(log.size() - 1).type == order_intent::kind::hedge,
          "the repriced hedge is last");

    // A second apply_updates: the forced quote path re-emits the quotes (forceUpdate=true, which
    // is what legacy does), but the hedge is only re-sent when its price actually moved.
    log.clear();
    strat.apply_updates(s, q, 0, true, log);
    uint32_t hedge_intents = 0;
    for (uint32_t i = 0; i < log.size(); ++i) {
        if (log.at(i).type == order_intent::kind::hedge && log.at(i).size != 0) ++hedge_intents;
    }
    check_eq(hedge_intents, 0, "the sized hedge is not re-sent when its price did not move");

    // on_md_change (:484-509): both quotes react to a top-of-book change.
    set_top(s, 100, 900, 102, 1100);
    s.side_book(side::bid).note_change(1, 1);
    s.side_book(side::ask).note_change(1, 1);
    strat.quotes().refresh_market(s, 0, 10);
    log.clear();
    const bool md_changed = strat.on_md_change(s, q, 0, /*top_book_changed=*/true, true, log);
    check(md_changed, "the market data change produces requests");
    uint32_t quote_intents = 0;
    for (uint32_t i = 0; i < log.size(); ++i) {
        if (log.at(i).type == order_intent::kind::quote) ++quote_intents;
    }
    check(quote_intents >= 2, "both quotes react to a top-of-book change");

    // Ordering invariant: quoting first, hedging after.
    bool seen_hedge = false;
    bool hedge_then_quote = false;
    for (uint32_t i = 0; i < log.size(); ++i) {
        if (log.at(i).type == order_intent::kind::hedge) seen_hedge = true;
        else if (seen_hedge) hedge_then_quote = true;
    }
    check(!hedge_then_quote, "no quote request follows a hedge request in the md-change path");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_hedge_size_reconcile();
    test_hedge_prices();
    test_cross_checks();
    test_cycle_sequence();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all MM phase-5 checks passed\n");
    return EXIT_SUCCESS;
}
