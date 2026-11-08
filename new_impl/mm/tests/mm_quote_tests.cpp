// Phase-3 gate for the new MarketMaker engine: the pricing block (established level, mid,
// width) and the quote price/size computation, for the two documented bypass paths
// (queueImportance == 0 / establishedLevel == 0).
//
//   mm_quote_tests [cycles]
//
// Two kinds of check:
//   * hand-written cases for the branches that a random tape would rarely reach (snap to
//     touch, join/leave hysteresis, payup mode, position caps, maxTicksWide latch, deeper
//     book changes); and
//   * a differential gate against tests/reference_oracle.hpp, which runs the legacy code at
//     the legacy 1e-8 scale. The engine runs at 1e-9, so the comparison measures exactly the
//     D2 truncation surface from PLAN.md §4 rather than asserting a tautology.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "book.hpp"
#include "config.hpp"
#include "model.hpp"
#include "quoting.hpp"
#include "state.hpp"
#include "tests/reference_oracle.hpp"
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

constexpr price_t TICK = 25'000'000;  // 0.025
price_t px(int ticks) { return TICK * ticks; }
int64_t legacy(price_t engine_price) { return engine_price / 10; }  // exact by construction

// The contract used throughout: the first row of config/backtest/model.csv, with the
// queue-position model switched off so only the bypass paths are reachable.
contract_input make_input() {
    contract_input c;
    c.name = "QMQ1-QMU1";
    c.tick_size = TICK;
    c.lot = 1;
    c.venue = 0;
    c.enabled = true;
    c.quote_external = true;
    c.quote_size = 8;
    c.quote_gap = 5;
    c.pct_tob_max_quote = 0.2;
    c.width_pct = {3.0, 2.0, 0.1, 3.0};
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
    c.max_ticks_wide = -1;
    c.worsen_payup_pct = 0.65477777777;
    c.queue_importance = 0;  // phase 3: no queue-position model yet
    return c;
}

constexpr double FREQUENCY = 1.0e6;

model make_model(const contract_input& c) {
    model_inputs in;
    in.contracts.push_back(c);
    in.cycles_per_second = FREQUENCY;
    in.current_shift = 2;  // US
    return model::build(in);
}

// Builds the legacy counterpart of a (model, state) pair at 1e-8.
oracle::legacy_market_maker make_oracle(const model& m, const contract_state& s) {
    oracle::legacy_market_maker o;
    o._tickSize = legacy(m.tick_size[0]);
    o._commission = legacy(m.commission[0]);
    o._minInternalPriceChange = legacy(m.min_internal_price_change[0]);
    o._worsenPayupConditionAmount = legacy(m.worsen_payup_amount[0]);
    o._quoteSize = m.quote_size[0];
    o._quoteGap = m.quote_gap[0];
    o._joinSize = m.join_size[0];
    o._leaveSize = m.leave_size[0];
    o._payUpQuantity = m.payup_qty[0];
    o._payUpOrders = m.payup_orders[0];
    o._payUpMultiple = m.payup_multiple[0];
    o._maxInternallyInitiatedPosition = m.max_pos_internal[0];
    o._maxExternallyInitiatedPosition = m.max_pos_external[0];
    o._pctTobMaxQuote = m.pct_tob_max_quote[0];
    o._widthPercentage = s.width_pct;
    o._queueImportance = m.queue_importance[0];
    o._orderIncrementQty = m.lot[0];
    o._emaEstLevel = s.ema_est_level;
    o._estLevelEmaPeriod = m.est_level_ema_period[0];
    o._estLevelPctEma = m.est_level_pct_ema[0];
    o._establishedLevel = s.established_level;
    o._lastTobQty = s.last_tob_qty;
    o._lastEmaTime = s.last_ema_time;
    o._internallyInitiatedPosition = s.internal_pos;
    o._externallyInitiatedPosition = s.external_pos;
    for (uint32_t sd = 0; sd < SIDE_COUNT; ++sd) {
        const quote_state& q = s.quotes[sd];
        o._quotes[sd].internal_price = q.internal_price == tachyon::NULL_PRICE
                                          ? oracle::kNullPrice
                                          : legacy(q.internal_price);
        o._quotes[sd].internal_size = q.internal_size;
        o._quotes[sd].external_price = q.external_price == tachyon::NULL_PRICE
                                          ? oracle::kNullPrice
                                          : legacy(q.external_price);
        o._quotes[sd].external_size = q.external_size;
        o._quotes[sd].last_internal_price_sent =
            q.last_internal_price_sent == tachyon::NULL_PRICE
                ? oracle::kNullPrice
                : legacy(q.last_internal_price_sent);
        o._quotes[sd].last_external_size_sent = q.last_external_size_sent;
    }
    return o;
}

void set_book(book& b, const level* levels, uint32_t n) {
    b.init(b.which_side());
    b.apply_snapshot(levels, n);
    b.note_change(1, 1);
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
// bypass path 1: !quoteExternally (legacy :757-760)
// ---------------------------------------------------------------------------
void test_internal_only() {
    std::printf("quote: internal-only path\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 10);

    // spread 2 ticks, width = 2 ticks / 2 * 0.1 = 2.5e6, mid = 101 ticks.
    check_eq(s.mid_point, px(101), "mid = 101 ticks");
    check_eq(s.width, 2'500'000, "width = 2.5e6");

    const bool changed = eng.build_quote(s, st.queue(0), 0, side::bid, /*quote_externally=*/false, false);
    check(changed, "first quote always changes (lastSent is NULL)");
    check_eq(s.quote_of(side::bid).internal_price, px(101) - 2'500'000,
             "internal price = mid - width");
    check_eq(s.quote_of(side::bid).external_price, tachyon::NULL_PRICE,
             "external price is untouched when not quoting externally");
    // The internal size is capped by the position room *and* by the market: pctTob 0.2 of the
    // 1000 lots displayed is 200, which is below the 1500 internal position cap.
    check_eq(s.quote_of(side::bid).internal_size, 200, "internal size is pctTob-capped");
    check_eq(s.quote_of(side::bid).external_size, 0, "no external size");

    // A second identical cycle changes nothing, because the price did not move.
    check(!eng.build_quote(s, st.queue(0), 0, side::bid, false, false), "unchanged price and size -> no request");
}

// ---------------------------------------------------------------------------
// external quoting: size cap, join/leave hysteresis (legacy :700-737)
// ---------------------------------------------------------------------------
void test_external_size_and_join_leave() {
    std::printf("quote: external size, join/leave\n");
    contract_input cfg = make_input();
    cfg.join_size = 10;  // must see 10 lots at our price before joining
    cfg.leave_size = 4;  // and stay while at least 4 are there
    const model m = make_model(cfg);
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    // The book shows only 5 lots: below the join threshold, so we do not join.
    set_top(s, 100, 5, 102, 1000);
    eng.refresh_market(s, 0, 10);
    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);
    check_eq(s.quote_of(side::bid).external_size, 0, "does not join below the join size");

    // Now the book deepens: we join, capped by quoteSize.
    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 20);
    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);
    check_eq(s.quote_of(side::bid).external_size, 8, "joins with the quote size");
    check_eq(s.quote_of(side::bid).external_price, px(100), "external price is the touch");

    // The book thins but stays above the leave threshold: the quote shrinks to the new market
    // cap (0.2 x 30 = 6) rather than leaving entirely.
    set_top(s, 100, 30, 102, 1000);
    eng.refresh_market(s, 0, 30);
    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);
    check_eq(s.quote_of(side::bid).external_size, 6, "keeps quoting, capped at pctTob x 30");

    // Below the leave threshold: we pull the quote.
    set_top(s, 100, 3, 102, 1000);
    eng.refresh_market(s, 0, 40);
    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);
    check_eq(s.quote_of(side::bid).external_size, 0, "leaves below the leave size");
}

// ---------------------------------------------------------------------------
// snap to the touch (legacy :762-766)
// ---------------------------------------------------------------------------
void test_snap_to_touch() {
    std::printf("quote: snap to touch\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    // A wide spread puts the ordinary bid price better than the touch, so legacy snaps it to
    // the touch and takes the commission off the internal price instead.
    set_top(s, 100, 1000, 104, 1000);
    eng.refresh_market(s, 0, 10);
    const price_t ordinary = s.mid_point - s.width;
    check(is_better_price(m.round(0, ordinary, side::bid), px(100), side::bid),
          "precondition: rounds better than the touch");

    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);
    check_eq(s.quote_of(side::bid).external_price, px(100), "external price snaps to the touch");
    check_eq(s.quote_of(side::bid).internal_price, px(100) + m.commission[0],
             "internal price is the touch plus the commission");
}

// ---------------------------------------------------------------------------
// payup mode (legacy :683-699, :356-361)
// ---------------------------------------------------------------------------
void test_payup() {
    std::printf("quote: payup mode\n");
    contract_input cfg = make_input();
    cfg.payup_qty = 5000;  // any displayed size triggers payup
    const model m = make_model(cfg);
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 10);
    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);

    const price_t worsened = px(100) - m.worsen_payup_amount[0];
    check_eq(s.quote_of(side::bid).internal_price, worsened,
             "payup worsens the internal price to the touch less the allowance");
    check_eq(s.quote_of(side::bid).external_price, m.round(0, worsened, side::bid),
             "external price is the rounded worsened price");
}

// ---------------------------------------------------------------------------
// position caps (legacy :1748-1790)
// ---------------------------------------------------------------------------
void test_position_caps() {
    std::printf("quote: position caps\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    // Long internal position: the bid is capped by the remaining room.
    s.internal_pos = 1490;
    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 10);
    eng.build_quote(s, st.queue(0), 0, side::bid, false, false);
    check_eq(s.quote_of(side::bid).internal_size, 10, "bid size = maxPos - position");

    // At the cap the bid size becomes 0 (legacy returns 0, not a negative size).
    s.internal_pos = 1500;
    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 20);
    eng.build_quote(s, st.queue(0), 0, side::bid, false, false);
    check_eq(s.quote_of(side::bid).internal_size, 0, "at the cap the bid size is 0");

    // Short beyond the cap on the other side: the bid gets the full position room back, but the
    // pctTob market cap (0.2 x 1000 = 200) still binds.
    s.internal_pos = -1600;
    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 30);
    eng.build_quote(s, st.queue(0), 0, side::bid, false, false);
    check_eq(s.quote_of(side::bid).internal_size, 200, "short position frees the position cap");

    // External cap behaves the same way.
    s.external_pos = 350;
    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 40);
    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);
    check_eq(s.quote_of(side::bid).external_size, 0, "external cap reached -> no external quote");
}

// ---------------------------------------------------------------------------
// cycle gating: the maxTicksWide latch and the deeper-change rule (legacy :603-628)
// ---------------------------------------------------------------------------
void test_update_gating() {
    std::printf("quote: update gating\n");
    const model m = make_model(make_input());
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    set_top(s, 100, 1000, 102, 1000);
    eng.refresh_market(s, 0, 10);
    check(eng.update_side(s, st.queue(0), 0, side::bid, true, true), "top-of-book change updates the quote");

    // Latched maxTicksWide: nothing is recalculated at all (:605).
    s.max_ticks_wide_breached = true;
    check(!eng.update_side(s, st.queue(0), 0, side::bid, true, true), "latch suppresses the update");
    s.max_ticks_wide_breached = false;

    // Deeper change: the quote sits on level 2 of the ladder, so a change at level 5 is
    // ignored while a change at level 2 is not.
    level bids[BOOK_LEVELS];
    level asks[BOOK_LEVELS];
    for (int i = 0; i < static_cast<int>(BOOK_LEVELS); ++i) {
        bids[i] = level{px(100 - i), 1000, 5, 0};
        asks[i] = level{px(102 + i), 1000, 5, 0};
    }
    s.side_book(side::bid).init(side::bid);
    s.side_book(side::bid).apply_snapshot(bids, BOOK_LEVELS);
    s.side_book(side::ask).init(side::ask);
    s.side_book(side::ask).apply_snapshot(asks, BOOK_LEVELS);
    eng.refresh_market(s, 0, 20);
    eng.build_quote(s, st.queue(0), 0, side::bid, true, false);
    s.quote_of(side::bid).external_price = px(98);  // level index 2
    // Make the next recalculation report a change, so "was it recalculated?" is observable:
    // the change detection compares against the last price *sent*.
    s.quote_of(side::bid).last_internal_price_sent = px(90);

    s.side_book(side::bid).clear_change_indices();
    s.side_book(side::bid).note_change(0, 6);  // qty change at level 5
    check(!eng.update_side(s, st.queue(0), 0, side::bid, false, true), "change behind the quote is ignored");

    s.side_book(side::bid).clear_change_indices();
    s.side_book(side::bid).note_change(0, 3);  // qty change at level 2 == our level
    check(eng.update_side(s, st.queue(0), 0, side::bid, false, true), "change at the quote level updates it");
}

// ---------------------------------------------------------------------------
// differential gate: engine (1e-9) vs the legacy transcription (1e-8)
// ---------------------------------------------------------------------------
// A price difference is either exact, inside the documented D-scale window (PLAN.md §4: the
// truncating divisions move a price by a few 1e-9 units), or a full-tick decision break.
// Measured maxima: 9 units without the queue model, 24 with it (the per-order offset
// truncations compound through cost/qty). 64 leaves headroom and is still three orders of
// magnitude below one tick (25e6 units).
constexpr int64_t SUB_TICK_WINDOW = 64;

struct field_stats {
    uint64_t total = 0;
    uint64_t exact = 0;
    uint64_t sub_tick = 0;
    uint64_t broken = 0;
    int64_t max_deviation = 0;  // largest |engine - 10 * oracle| seen, in 1e-9 units

    void add_price(price_t engine_value, price_t oracle_value) {
        ++total;
        const bool engine_null = engine_value == tachyon::NULL_PRICE;
        const bool oracle_null = oracle_value == oracle::kNullPrice;
        if (engine_null || oracle_null) {
            if (engine_null && oracle_null) {
                ++exact;
            } else {
                ++broken;
            }
            return;
        }
        const int64_t scaled = oracle_value * 10;  // oracle is at 1e-8, the engine at 1e-9
        const int64_t diff = engine_value - scaled;
        const int64_t magnitude = diff < 0 ? -diff : diff;
        if (magnitude > max_deviation) max_deviation = magnitude;
        if (magnitude == 0) {
            ++exact;
        } else if (magnitude <= SUB_TICK_WINDOW) {
            ++sub_tick;
        } else {
            ++broken;
        }
    }

    void add_qty(qty_t engine_value, qty_t oracle_value) {
        ++total;
        if (engine_value == oracle_value) {
            ++exact;
        } else {
            ++broken;
        }
    }

    double exact_rate() const { return total ? static_cast<double>(exact) / total : 1.0; }

    void report(const char* name) const {
        std::printf(
            "  %-16s exact %8llu/%8llu (%7.4f%%)  sub-tick %6llu  breaks %llu  max-dev %lld\n",
            name, (unsigned long long)exact, (unsigned long long)total, 100.0 * exact_rate(),
            (unsigned long long)sub_tick, (unsigned long long)broken, (long long)max_deviation);
    }
};

void test_differential(int cycles) {
    std::printf("differential gate: engine (1e-9) vs oracle (1e-8), %d cycles\n", cycles);

    const model m = make_model(make_input());
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);
    oracle::legacy_market_maker orc = make_oracle(m, s);

    field_stats mid, width, established, internal_price, external_price, internal_size,
        external_size, changed_flag;

    std::mt19937 rng(20251107);
    uint64_t now = 1000;

    for (int i = 0; i < cycles; ++i) {
        now += 1 + rng() % 5000;

        // Occasionally move the positions, so the size caps are exercised too.
        if (i % 16 == 0) {
            s.internal_pos = static_cast<qty_t>(rng() % 3400) - 1700;
            s.external_pos = static_cast<qty_t>(rng() % 800) - 400;
            orc._internallyInitiatedPosition = s.internal_pos;
            orc._externallyInitiatedPosition = s.external_pos;
        }
        // Occasionally change the active shift's width percentage (updateWidthPercent).
        if (i % 64 == 0) {
            static const double kWidths[4] = {0.1, 1.0, 3.0, 2.0};
            s.width_pct = kWidths[rng() % 4];
            orc._widthPercentage = s.width_pct;
        }

        const int base = 2000 + static_cast<int>(rng() % 400);
        const int spread = 1 + static_cast<int>(rng() % 8);
        const qty_t bid_qty = 1 + static_cast<qty_t>(rng() % 600);
        const qty_t ask_qty = 1 + static_cast<qty_t>(rng() % 600);
        const uint32_t bid_orders = static_cast<uint32_t>(rng() % 8);
        const uint32_t ask_orders = static_cast<uint32_t>(rng() % 8);

        level bids[BOOK_LEVELS];
        level asks[BOOK_LEVELS];
        for (int k = 0; k < static_cast<int>(BOOK_LEVELS); ++k) {
            bids[k] = level{px(base - k), 1 + static_cast<qty_t>(rng() % 300),
                            static_cast<uint32_t>(rng() % 8), 0};
            asks[k] = level{px(base + spread + k), 1 + static_cast<qty_t>(rng() % 300),
                            static_cast<uint32_t>(rng() % 8), 0};
        }
        bids[0].qty = bid_qty;
        bids[0].order_count = bid_orders;
        asks[0].qty = ask_qty;
        asks[0].order_count = ask_orders;

        s.side_book(side::bid).init(side::bid);
        s.side_book(side::bid).apply_snapshot(bids, BOOK_LEVELS);
        s.side_book(side::ask).init(side::ask);
        s.side_book(side::ask).apply_snapshot(asks, BOOK_LEVELS);

        oracle::book_side bid_top;
        bid_top.s = side::bid;
        bid_top.l[0] = oracle::top{legacy(px(base)), bid_qty, bid_orders};
        oracle::book_side ask_top;
        ask_top.s = side::ask;
        ask_top.l[0] = oracle::top{legacy(px(base + spread)), ask_qty, ask_orders};
        const std::map<price_t, std::vector<oracle::legacy_order>> no_orders;

        // ---- pricing block ----
        eng.refresh_market(s, 0, now);
        orc.onTopBookChange(bid_top, ask_top, now);
        mid.add_price(s.mid_point, orc._midPoint);
        width.add_price(s.width, orc._width);
        established.add_qty(s.established_level, orc._establishedLevel);

        // ---- quotes, both sides, same external-quoting decision ----
        const bool quote_externally = (rng() & 1) != 0;
        for (side sd : {side::bid, side::ask}) {
            const oracle::book_side& book_top = sd == side::bid ? bid_top : ask_top;
            const bool engine_changed = eng.build_quote(s, st.queue(0), 0, sd, quote_externally, false);
            const bool oracle_changed = orc.updateQuote(sd, book_top, no_orders, quote_externally, false);
            changed_flag.add_qty(engine_changed ? 1 : 0, oracle_changed ? 1 : 0);

            const quote_state& eq = s.quote_of(sd);
            const oracle::quote& oq = orc._quotes[to_index(sd)];
            internal_price.add_price(eq.internal_price, oq.internal_price);
            external_price.add_price(eq.external_price, oq.external_price);
            internal_size.add_qty(eq.internal_size, oq.internal_size);
            external_size.add_qty(eq.external_size, oq.external_size);
        }
    }

    mid.report("mid");
    width.report("width");
    established.report("establishedSize");
    internal_price.report("internalPrice");
    external_price.report("externalPrice");
    internal_size.report("internalSize");
    external_size.report("externalSize");
    changed_flag.report("changedFlag");

    // The decision fields must be identical (D3 allows 1e-4 of cycles of slack; we expect 0).
    check(external_price.broken == 0, "no external-price decision differs");
    check(internal_size.broken == 0, "no internal-size decision differs");
    check(external_size.broken == 0, "no external-size decision differs");
    check(changed_flag.broken == 0, "the 'must re-send' decision is identical");
    // Intermediate values may only differ inside the documented truncation window.
    check(mid.broken == 0, "no mid breaks outside the D-scale window");
    check(width.broken == 0, "no width breaks outside the D-scale window");
    check(internal_price.broken == 0, "no internal-price breaks outside the D-scale window");
    check(established.broken == 0, "the established level is identical (quantity domain)");
    check(external_price.exact_rate() >= 0.9999, "external price parity >= 99.99% (D3)");
    check(internal_size.exact_rate() >= 0.9999, "internal size parity >= 99.99% (D3)");
    check(external_size.exact_rate() >= 0.9999, "external size parity >= 99.99% (D3)");
    check(changed_flag.exact_rate() >= 0.9999, "change-decision parity >= 99.99% (D3)");
    check(established.exact_rate() == 1.0, "established level parity is 100% (qty domain)");
}

// ---------------------------------------------------------------------------
// the queue-position S-curve (legacy :779-788)
// ---------------------------------------------------------------------------
contract_input make_queue_input() {
    contract_input c = make_input();
    c.queue_importance = 3;  // the value in config/backtest/model.csv

    return c;
}

void test_s_curve() {
    std::printf("quote: queue-position S-curve\n");
    const model m = make_model(make_queue_input());
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    s.established_level = 64;  // a power of two, so 1/est is exact
    s.width = 2'500'000;
    const price_t price = px(100);
    const double inv = 1.0 / 64.0;

    // Half way through the queue the offset is exactly zero: the logistic is centred on 0.5.
    const auto halfway = eng.price_at_queue_position(s, 0, side::bid, price, 32, inv);
    check_eq(halfway.internal_price, price, "qp = est/2 -> zero offset");

    // The front of the queue quotes more aggressively than the back, for a bid.
    const auto front = eng.price_at_queue_position(s, 0, side::bid, price, 0, inv);
    const auto back = eng.price_at_queue_position(s, 0, side::bid, price, 64, inv);
    const price_t front_offset = front.internal_price - price;
    const price_t back_offset = back.internal_price - price;
    // 0.635148952... * 2'500'000 = 1'587'872.38
    check(front_offset > 1'587'870 && front_offset < 1'587'875, "front offset ~ +1.588e6");
    check(back_offset < -1'587'870 && back_offset > -1'587'875, "back offset ~ -1.588e6");
    check_eq(front_offset, -back_offset, "the curve is antisymmetric about the midpoint");

    // Both ends clamp to the end they pass.
    check_eq(eng.price_at_queue_position(s, 0, side::bid, price, 5'000, inv).internal_price,
             back.internal_price, "qp beyond the level clamps to the back");
    check_eq(eng.price_at_queue_position(s, 0, side::bid, price, -5, inv).internal_price,
             front.internal_price, "negative qp clamps to the front");

    // The ask side mirrors it (a better ask is a lower price).
    const auto ask_front = eng.price_at_queue_position(s, 0, side::ask, price, 0, inv);
    check_eq(ask_front.internal_price, price - front_offset, "ask front offset is negated");
    check_eq(ask_front.external_price, m.round(0, ask_front.internal_price, side::ask),
             "the external price is the rounded internal price");

    // Queue importance 0 flattens the curve (it is a bypass in price_of, but the formula itself
    // must agree).
    const model m_flat = make_model(make_input());  // queue_importance stays 0
    cycle_state st_flat(m_flat);
    quote_engine eng_flat(m_flat);
    contract_state& sf = st_flat.contract(0);
    sf.established_level = 64;
    sf.width = 2'500'000;
    check_eq(eng_flat.price_at_queue_position(sf, 0, side::bid, price, 0, inv).internal_price,
             price, "importance 0 -> zero offset everywhere");
}

// ---------------------------------------------------------------------------
// the walk over our resting orders (:810-851) and the spray (:834-851)
// ---------------------------------------------------------------------------
void test_queue_walk() {
    std::printf("quote: queue-position walk\n");
    contract_input cfg = make_queue_input();
    cfg.width_pct = {0.9, 0.9, 0.9, 0.9};  // width = 8 ticks / 2 * 0.9 = 90e6
    const model m = make_model(cfg);
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    const int base = 100;
    set_top(s, base, 1000, base + 8, 1000);
    eng.refresh_market(s, 0, 10);
    s.established_level = 64;
    const double inv = 1.0 / 64.0;

    // The ordinary bid price is mid - width = px(104) - 90e6 = px(100) + 10e6, which rounds to
    // the touch, so the target level is the bid's own level.
    const price_t price = s.mid_point - s.width;
    check_eq(m.round(0, price, side::bid), px(base), "target is the bid touch");

    // Nothing resting yet: the back of the queue prices two ticks worse than the target, so
    // legacy keeps the ordinary price and quotes **no size** (:800-803) rather than quoting at
    // a level it cannot defend.
    {
        order_queue q;
        const auto ps = eng.price_of(s, q, 0, side::bid, price, true);
        const auto at_back = eng.price_at_queue_position(s, 0, side::bid, price, 1000, inv);
        check_eq(ps.internal_price, price, "the ordinary price is what we show");
        check(at_back.external_price < px(base), "the back of the queue is a worse level");
        check_eq(ps.size, 0, "so the quote carries no size");
    }

    // One order at half of the established level: accepted, and it prices the whole quote.
    order_queue q;
    q.upsert(1, px(base), 5, 32, side::bid);
    {
        const auto ps = eng.price_of(s, q, 0, side::bid, price, true);
        check_eq(ps.internal_price, price, "qp = est/2 -> prices at the ordinary price");
        check_eq(ps.external_price, px(base), "external price is the target");
        check_eq(ps.size, 5, "size is the sum of the accepted orders");
    }

    // A second order deep in the queue prices a tick worse and stops the walk.
    q.upsert(2, px(base), 7, 64, side::bid);
    {
        const auto ps = eng.price_of(s, q, 0, side::bid, price, true);
        check_eq(ps.size, 5, "the unwanted order is excluded");
        check_eq(ps.internal_price, price, "cost/qty over the accepted orders only");
    }

    // The same two orders the other way round: both accepted.
    {
        order_queue q2;
        q2.upsert(1, px(base), 5, 0, side::bid);   // at the front: more aggressive
        q2.upsert(2, px(base), 7, 32, side::bid);  // half way
        const auto ps = eng.price_of(s, q2, 0, side::bid, price, true);
        const auto at_front = eng.price_at_queue_position(s, 0, side::bid, price, 0, inv);
        check_eq(ps.size, 12, "both orders accepted");
        check_eq(ps.external_price, px(base), "average better than target -> stay on the target");
        check_eq(ps.internal_price, price, "legacy returns the ordinary price in that branch");
        check(at_front.external_price > px(base), "the front order alone would quote better");
    }

    // A queue position the exchange has not sent yet is assumed to be at the back.
    {
        order_queue q3;
        q3.upsert(1, px(base), 5, order_queue::NOT_SET, side::bid);
        const auto ps = eng.price_of(s, q3, 0, side::bid, price, true);
        check_eq(ps.size, 0, "NOT_SET = back of the queue = unwanted at this width");
    }
}

void test_spray() {
    std::printf("quote: spray\n");
    const model m = make_model(make_queue_input());  // width pct 0.1 -> width = 2.5e6
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);

    // Only 32 lots displayed, so the back of the queue sits at half the established level and
    // still rounds to the target: the spray is admissible.
    set_top(s, 100, 32, 102, 1000);
    eng.refresh_market(s, 0, 10);
    s.established_level = 64;  // refresh_market seeds the EMA; pin the level for the test

    const price_t price = s.mid_point - s.width;
    order_queue q;
    q.upsert(1, px(100), 5, 0, side::bid);  // at the front, accepted

    const auto ps = eng.price_of(s, q, 0, side::bid, price, true);
    // The spray asks for one more quote size on top of the 5 resting, but pctTob caps the level
    // at 0.2 x 32 = 6 lots, so only one lot is added (legacy getSprayQty, :866-872).
    check_eq(ps.size, 6, "5 resting + 1 sprayed (capped by pctTob)");
    check_eq(ps.external_price, px(100), "still on the target level");

    // With the book deep enough that the pctTob cap does not bind, the full quote size is added.
    {
        set_top(s, 100, 400, 102, 1000);
        eng.refresh_market(s, 0, 30);
        s.established_level = 64;
        order_queue q2;
        q2.upsert(1, px(100), 5, 0, side::bid);
        const auto full = eng.price_of(s, q2, 0, side::bid, s.mid_point - s.width, true);
        check_eq(full.size, 13, "5 resting + 8 sprayed when the cap allows it");
    }

    // With a gap larger than the room behind the last order, no spray is added.
    contract_input wide_gap = make_queue_input();
    wide_gap.quote_gap = 10'000;
    const model m_gap = make_model(wide_gap);
    cycle_state st_gap(m_gap);
    quote_engine eng_gap(m_gap);
    contract_state& sg = st_gap.contract(0);
    set_top(sg, 100, 32, 102, 1000);
    eng_gap.refresh_market(sg, 0, 10);
    sg.established_level = 64;
    order_queue qg;
    qg.upsert(1, px(100), 5, 0, side::bid);
    const auto no_spray =
        eng_gap.price_of(sg, qg, 0, side::bid, sg.mid_point - sg.width, true);
    check_eq(no_spray.size, 5, "no spray when the gap behind the last order is too small");
}

// ---------------------------------------------------------------------------
// differential gate with the queue model enabled (queueImportance = 3)
// ---------------------------------------------------------------------------
void test_queue_differential(int cycles) {
    std::printf("differential gate (queue model): engine vs oracle, %d cycles\n", cycles);

    const model m = make_model(make_queue_input());
    cycle_state st(m);
    quote_engine eng(m);
    contract_state& s = st.contract(0);
    oracle::legacy_market_maker orc = make_oracle(m, s);

    field_stats internal_price, external_price, internal_size, external_size, changed_flag;

    std::mt19937 rng(987654321);
    uint64_t now = 1000;

    for (int i = 0; i < cycles; ++i) {
        now += 1 + rng() % 3000;

        if (i % 16 == 0) {
            s.internal_pos = static_cast<qty_t>(rng() % 3400) - 1700;
            s.external_pos = static_cast<qty_t>(rng() % 800) - 400;
            orc._internallyInitiatedPosition = s.internal_pos;
            orc._externallyInitiatedPosition = s.external_pos;
        }
        if (i % 64 == 0) {
            static const double kWidths[4] = {0.1, 0.9, 1.0, 3.0};
            s.width_pct = kWidths[rng() % 4];
            orc._widthPercentage = s.width_pct;
        }

        const int base = 2000 + static_cast<int>(rng() % 200);
        const int spread = 1 + static_cast<int>(rng() % 8);

        level bids[BOOK_LEVELS];
        level asks[BOOK_LEVELS];
        oracle::book_side bid_bs;
        oracle::book_side ask_bs;
        bid_bs.s = side::bid;
        ask_bs.s = side::ask;
        for (int k = 0; k < 3; ++k) {  // three levels exercise targetSize + the implied qty
            const qty_t bq = 1 + static_cast<qty_t>(rng() % 400);
            const qty_t aq = 1 + static_cast<qty_t>(rng() % 400);
            const qty_t bimpl = rng() % 3 == 0 ? static_cast<qty_t>(rng() % 50) : 0;
            const qty_t aimpl = rng() % 3 == 0 ? static_cast<qty_t>(rng() % 50) : 0;
            bids[k] = level{px(base - k), bq, static_cast<uint32_t>(rng() % 8), bimpl};
            asks[k] = level{px(base + spread + k), aq, static_cast<uint32_t>(rng() % 8), aimpl};
            bid_bs.l[k] = oracle::top{legacy(bids[k].price), bq, bids[k].order_count};
            bid_bs.implied[k] = bimpl;
            ask_bs.l[k] = oracle::top{legacy(asks[k].price), aq, asks[k].order_count};
            ask_bs.implied[k] = aimpl;
        }
        for (uint32_t k = 3; k < BOOK_LEVELS; ++k) {
            bids[k] = level{};
            asks[k] = level{};
        }

        s.side_book(side::bid).init(side::bid);
        s.side_book(side::bid).apply_snapshot(bids, BOOK_LEVELS);
        s.side_book(side::ask).init(side::ask);
        s.side_book(side::ask).apply_snapshot(asks, BOOK_LEVELS);

        // Resting orders at the ladder prices, fed identically to engine and oracle.
        st.queue(0).init();
        std::map<price_t, std::vector<oracle::legacy_order>> bid_orders;
        std::map<price_t, std::vector<oracle::legacy_order>> ask_orders;
        uint32_t next_id = 1;
        for (int sd = 0; sd < static_cast<int>(SIDE_COUNT); ++sd) {
            const side sn = static_cast<side>(sd);
            auto& orders = sd == 0 ? bid_orders : ask_orders;
            const int n = static_cast<int>(rng() % 4);  // 0..3 orders
            for (int k = 0; k < n; ++k) {
                const int off = static_cast<int>(rng() % 3);
                const price_t price = sd == 0 ? px(base - off) : px(base + spread + off);
                const qty_t qty = 1 + static_cast<qty_t>(rng() % 20);
                const qty_t qp =
                    (rng() % 6 == 0) ? order_queue::NOT_SET : static_cast<qty_t>(rng() % 300);
                st.queue(0).upsert(next_id, price, qty, qp, sn);
                // The oracle works in legacy units, so its map key must be converted too —
                // the engine keeps 1e-9 everywhere, the oracle looks up at 1e-8.
                orders[legacy(price)].push_back(
                    oracle::legacy_order{next_id, legacy(price), qty, qp});
                ++next_id;
            }
        }
        eng.refresh_market(s, 0, now);
        orc.onTopBookChange(bid_bs, ask_bs, now);

        const bool quote_externally = (rng() & 1) != 0;
        for (side sd : {side::bid, side::ask}) {
            const oracle::book_side& bs = sd == side::bid ? bid_bs : ask_bs;
            const auto& orders = sd == side::bid ? bid_orders : ask_orders;
            const bool engine_changed =
                eng.build_quote(s, st.queue(0), 0, sd, quote_externally, false);
            const bool oracle_changed = orc.updateQuote(sd, bs, orders, quote_externally, false);
            changed_flag.add_qty(engine_changed ? 1 : 0, oracle_changed ? 1 : 0);

            const quote_state& eq = s.quote_of(sd);
            const oracle::quote& oq = orc._quotes[to_index(sd)];
            internal_price.add_price(eq.internal_price, oq.internal_price);
            external_price.add_price(eq.external_price, oq.external_price);
            internal_size.add_qty(eq.internal_size, oq.internal_size);
            external_size.add_qty(eq.external_size, oq.external_size);
        }
    }

    internal_price.report("q internalPrice");
    external_price.report("q externalPrice");
    internal_size.report("q internalSize");
    external_size.report("q externalSize");
    changed_flag.report("q changedFlag");

    check(external_price.broken == 0, "queue model: no external-price decision differs");
    check(internal_size.broken == 0, "queue model: no internal-size decision differs");
    check(external_size.broken == 0, "queue model: no external-size decision differs");
    check(changed_flag.broken == 0, "queue model: the re-send decision is identical");
    check(internal_price.broken == 0, "queue model: no price breaks outside the D-scale window");
    check(external_price.exact_rate() >= 0.9999, "queue model: external price parity >= 99.99%");
    check(internal_size.exact_rate() >= 0.9999, "queue model: internal size parity >= 99.99%");
    check(external_size.exact_rate() >= 0.9999, "queue model: external size parity >= 99.99%");
    check(changed_flag.exact_rate() >= 0.9999, "queue model: re-send parity >= 99.99%");
}

}  // namespace

int main(int argc, char** argv) {
    const int cycles = argc > 1 ? std::atoi(argv[1]) : 20000;

    test_internal_only();
    test_external_size_and_join_leave();
    test_snap_to_touch();
    test_payup();
    test_position_caps();
    test_update_gating();
    test_s_curve();
    test_queue_walk();
    test_spray();
    test_differential(cycles);
    test_queue_differential(cycles);

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all MM phase-3/4 checks passed\n");
    return EXIT_SUCCESS;
}
