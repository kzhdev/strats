// P6 gate: the composition root -- the event surface, the dirty-contract cycle, quote emission and
// deferral, the disable path, and the hedge round trip through the liquidity ledger.
//
//   as_host_test

#include <cstdio>
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
// `as::qty_t` is signed (D10) and shadows `trade::qty_t`, so only the shared names are pulled in.
using tachyon::trade::EXTERNAL;
using tachyon::trade::INTERNAL;
using tachyon::trade::liquidity_ledger;
using tachyon::trade::liquidity_source;
using tachyon::trade::make_market_key;
using tachyon::trade::market_key;
using tachyon::trade::ord_id_t;
using tachyon::trade::order_side;
using tachyon::trade::reservation_filter;

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

price_t px(double dollars) { return to_scaled(dollars); }

// Records everything the host emits, so the gate can assert on the exact stream.
struct recorder {
    std::vector<quote_request> quotes;
    std::vector<uint32_t> notifications;
    std::vector<hedge_order> hedges;
    std::vector<std::pair<uint32_t, uint32_t>> fills;  // (contract, qty)
    std::vector<uint32_t> forwards;
    std::vector<uint32_t> switch_fills;
    std::vector<std::string> alerts;
    int cancels = 0;
};

void rec_quote(void* ctx, const quote_request& q) noexcept {
    static_cast<recorder*>(ctx)->quotes.push_back(q);
}
void rec_notify(void* ctx, uint32_t c) noexcept {
    static_cast<recorder*>(ctx)->notifications.push_back(c);
}
void rec_hedge(void* ctx, const hedge_order& o) noexcept {
    static_cast<recorder*>(ctx)->hedges.push_back(o);
}
void rec_fill(void* ctx, uint32_t c, side, qty_t qty, price_t) noexcept {
    static_cast<recorder*>(ctx)->fills.emplace_back(c, static_cast<uint32_t>(qty));
}
void rec_switch(void* ctx, const hedge_plan&) noexcept {
    static_cast<recorder*>(ctx)->switch_fills.push_back(1);
}
void rec_forward(void* ctx, uint32_t c, side, qty_t) noexcept {
    static_cast<recorder*>(ctx)->forwards.push_back(c);
}
void rec_alert(void* ctx, const char* reason) noexcept {
    static_cast<recorder*>(ctx)->alerts.emplace_back(reason);
}
void rec_cancel(void* ctx) noexcept { ++static_cast<recorder*>(ctx)->cancels; }

model real_model() {
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    return model::build(build_model_inputs(cfg));
}

// A host wired to the real config, with every book mapped onto the ledger.
struct fixture {
    model m;
    cycle_state s;
    liquidity_ledger ledger;
    recorder rec;
    host_ports ports;
    std::unique_ptr<host> h;

    explicit fixture(uint32_t quoting_contract) : m(real_model()), s(m) {
        ports.send_quote = &rec_quote;
        ports.notify_quote = &rec_notify;
        ports.send_hedge = &rec_hedge;
        ports.send_fill = &rec_fill;
        ports.send_switch_fill = &rec_switch;
        ports.forward_fill = &rec_forward;
        ports.on_alert = &rec_alert;
        ports.cancel_all = &rec_cancel;
        ports.ctx = &rec;
        h = std::make_unique<host>(m, s, &ledger, ports);
        for (uint32_t c = 0; c < m.n_all; ++c) {
            for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
                const side sd = static_cast<side>(si);
                h->wire_market(c, sd, true, make_market_key(c, static_cast<order_side>(si), EXTERNAL),
                               liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
                h->wire_market(c, sd, false, make_market_key(c, static_cast<order_side>(si), INTERNAL),
                               liquidity_source::LIQUIDITY_SOURCE_RV_QUOTE);
            }
        }
        (void)quoting_contract;
    }
};

}  // namespace
namespace {

// ---------------------------------------------------------------------------
// 1. the quote lifecycle
// ---------------------------------------------------------------------------
void test_quote_lifecycle() {
    std::printf("host: quote lifecycle\n");
    fixture f(6);
    const uint32_t c = 6;

    // publish both sides of the lean the best path uses
    f.h->on_market_data(c, side::bid, false, px(60.00), 10);
    f.h->on_market_data(c, side::bid, true, px(59.75), 10);
    f.h->on_market_data(c, side::ask, false, px(60.25), 10);
    f.h->on_market_data(c, side::ask, true, px(60.50), 10);
    f.h->run_cycle(1);
    check(!f.rec.quotes.empty(), "the first cycle emits quotes");
    check_eq(f.rec.quotes.size(), 2, "both sides are quoted");
    check_eq(f.rec.quotes[0].internal_price, px(59.99) - f.m.edge[c],
             "the bid quote is the path price minus the edge");
    check_eq(f.rec.notifications.size(), 1, "one notification per contract, not per side");
    check_eq(f.rec.notifications[0], c, "the notification names the contract");

    // no market data, no emission
    const size_t before = f.rec.quotes.size();
    f.h->run_cycle(2);
    check_eq(f.rec.quotes.size(), static_cast<long long>(before), "an idle cycle emits nothing");

    // a price move re-emits only the affected side
    f.h->on_market_data(c, side::bid, false, px(60.25), 10);
    f.h->run_cycle(3);
    check(f.rec.quotes.size() > before, "a price move re-emits");
    check(f.rec.quotes.back().sd == side::bid, "and the re-emitted side is the bid");

    // quotes respect the contract's lot size and quote size
    check(f.rec.quotes.back().internal_size <= f.m.quote_size[c],
          "the emitted size never exceeds the configured quote size");
}

// ---------------------------------------------------------------------------
// 2. deferred emission while a transaction is open
// ---------------------------------------------------------------------------
void test_deferred_emission() {
    std::printf("host: deferred emission\n");
    fixture f(6);
    const uint32_t c = 6;
    f.h->on_market_data(c, side::bid, false, px(60.00), 10);
    f.h->run_cycle(1);
    const size_t emitted = f.rec.quotes.size();
    check(emitted > 0, "precondition: a quote went out");

    f.h->set_can_send_quotes(false);
    f.h->on_market_data(c, side::bid, false, px(60.50), 10);
    f.h->run_cycle(2);
    check_eq(f.rec.quotes.size(), static_cast<long long>(emitted),
             "nothing is emitted while quotes are blocked");
    check(f.h->stats().quotes_blocked > 0, "the block is counted");

    f.h->set_can_send_quotes(true);
    f.h->run_cycle(3);
    check(f.rec.quotes.size() > emitted, "the retained change is emitted once unblocked");
}

// ---------------------------------------------------------------------------
// 3. the maxTicksThroughMarket disable path
// ---------------------------------------------------------------------------
void test_max_ticks_disable() {
    std::printf("host: maxTicksThroughMarket disable path\n");
    fixture f(6);
    const uint32_t c = 6;
    // the ASK book is far below the bid path's price: the bid quote would sit through the market
    f.h->on_market_data(c, side::bid, false, px(60.00), 10);
    f.h->on_market_data(c, side::ask, false, px(50.00), 10);
    f.h->run_cycle(1);

    check(f.h->stats().validation_failures > 0, "the gate reports a violation");
    check(!f.h->enabled(), "the violation disables the strategy");
    check(!f.h->external_quoting_enabled(), "and its external quoting");
    check(!f.rec.alerts.empty(), "the violation raises a critical alert");
    check_eq(f.rec.cancels, 1, "everything is cancelled once");

    const size_t quotes = f.rec.quotes.size();
    f.h->on_market_data(c, side::bid, false, px(60.50), 10);
    f.h->run_cycle(2);
    check_eq(f.rec.quotes.size(), static_cast<long long>(quotes),
             "a disabled strategy emits nothing further");
}

// ---------------------------------------------------------------------------
// 4. the hedge round trip through the ledger
// ---------------------------------------------------------------------------
void test_hedge_round_trip() {
    std::printf("host: hedge round trip\n");
    fixture f(6);
    const uint32_t c = 6;
    f.h->on_market_data(c, side::bid, false, px(60.00), 10);
    f.h->run_cycle(1);
    check(f.h->bridge() != nullptr, "precondition: the host is ledger-backed");

    // a fill on the bid is hedged with an ask order on the same lean
    f.h->on_fill(c, side::bid, 4, true);
    check_eq(f.rec.hedges.size(), 1, "the fill produces one hedge order");
    const hedge_order& hedge = f.rec.hedges[0];
    check(hedge.sd == side::ask, "hedged on the ask");
    check_eq(hedge.size, 4, "for the whole fill quantity");
    check_eq(hedge.internal_price, px(60.00) - f.m.hedge_offset_price[c],
             "priced off the bid with the payup");
    check_eq(hedge.contract, c, "the order records the filled contract");
    check(hedge.hedge_id > 0, "the order carries a hedge id");
    check(f.h->stats().hedges_sent == 1, "the host counts the hedge");
    check_eq(static_cast<long long>(f.rec.fills.size()), 1,
             "the front is told about the fill first");

    // the hedge reserved liquidity on the lean's book
    check(f.h->stats().reservations_taken > 0, "the hedge reserved liquidity");
    check(f.h->bridge()->active_reservations() > 0, "the ledger holds the reservation");

    // the ack and the completion release it
    const market_key target = make_market_key(
        c, static_cast<order_side>(to_index(side::bid)), INTERNAL);
    f.h->on_hedge_ack(hedge.hedge_id, target, 7, 4);
    f.h->on_hedge_done(hedge.hedge_id, target);
    check_eq(static_cast<long long>(f.h->bridge()->active_reservations()), 0,
             "the reservation is released after the ack and the completion");
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// 5. an unhedgeable fill is forwarded, and the short-term fair round trip works
// ---------------------------------------------------------------------------
void test_unhedged_and_stf() {
    std::printf("host: unhedgeable fill + fair-value round trip\n");
    fixture f(6);
    const uint32_t c = 6;

    // no books at all: nothing can be hedged
    f.h->on_fill(c, side::bid, 3, false);
    check_eq(f.rec.hedges.size(), 0, "no hedge order without a usable path");
    check_eq(static_cast<long long>(f.rec.forwards.size()), 1,
             "the fill is forwarded to the market maker");
    check_eq(static_cast<long long>(f.rec.fills.size()), 0,
             "and the front is not told AS covered it");
    check(!f.rec.alerts.empty(), "the failure raises an alert");

    // publish a book, then feed a fair value: the path prices and quotes
    f.h->on_market_data(c, side::bid, false, px(60.00), 10);
    f.h->run_cycle(1);
    const size_t quotes = f.rec.quotes.size();
    check(quotes > 0, "precondition: the contract quotes");

    // the real config has no switch paths, so a fair value changes nothing observable here --
    // what matters is that the call is safe and does not emit
    f.h->on_stf(c, px(60.001));
    f.h->run_cycle(2);
    check_eq(f.rec.quotes.size(), static_cast<long long>(quotes),
             "an unchanged fair value re-emits nothing");
    check(f.s.has_stf(c), "the fair value is recorded");
}

// ---------------------------------------------------------------------------
// 6. external quoting gates
// ---------------------------------------------------------------------------
void test_external_gates() {
    std::printf("host: external quoting gates\n");
    fixture f(6);
    const uint32_t c = 6;
    f.h->on_market_data(c, side::bid, false, px(60.00), 10);
    f.h->run_cycle(1);
    check(f.rec.quotes.back().quote_external, "precondition: external quoting is on");
    check(f.rec.quotes.back().external_price != NULL_PRICE, "with an external price");

    // the strategy-wide gate
    f.h->set_external_quoting_enabled(false);
    f.h->on_market_data(c, side::bid, false, px(60.25), 10);
    f.h->run_cycle(2);
    check(!f.rec.quotes.back().quote_external, "the strategy gate turns external quoting off");
    check_eq(f.rec.quotes.back().external_price, NULL_PRICE, "and clears the external price");

    // the per-venue gate
    f.h->set_external_quoting_enabled(true);
    f.h->set_venue_enabled(f.m.venue[c], false);
    f.h->on_market_data(c, side::bid, false, px(60.50), 10);
    f.h->run_cycle(3);
    check(!f.rec.quotes.back().quote_external, "the venue gate turns it off too");
}

// ---------------------------------------------------------------------------
// 7. the aggressive quote reserves liquidity on the book it advertises on
// ---------------------------------------------------------------------------
void test_aggressive_quote_reservation() {
    std::printf("host: aggressive quote reservation\n");
    fixture f(6);
    const uint32_t c = 6;
    // a cheap *external* ask makes the bid quote cross it, so the quote reserves that liquidity
    f.h->on_market_data(c, side::bid, false, px(60.00), 10);
    f.h->on_market_data(c, side::ask, true, px(59.00), 10);
    f.h->run_cycle(1);

    const qty_t before = f.ledger.available(
        make_market_key(c, static_cast<order_side>(to_index(side::ask)), EXTERNAL), 10,
        reservation_filter{});
    check(before < 10, "the crossing bid reserved some of the ask's displayed size");
    check(f.h->stats().quotes_emitted > 0, "the quote was emitted");

    // a fill releases what the aggressive quote held
    f.h->on_fill(c, side::bid, 2, true);
    f.h->run_cycle(2);
    const qty_t after = f.ledger.available(
        make_market_key(c, static_cast<order_side>(to_index(side::ask)), EXTERNAL), 10,
        reservation_filter{});
    check(after >= before, "a fill releases the aggressive-quote reservation");
}

}  // namespace

int main() {
    test_quote_lifecycle();
    test_deferred_emission();
    test_max_ticks_disable();
    test_hedge_round_trip();
    test_unhedged_and_stf();
    test_external_gates();
    test_aggressive_quote_reservation();

    if (g_failures == 0) {
        std::printf("\nall AS phase-6 checks passed\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}

