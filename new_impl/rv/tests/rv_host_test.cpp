// End-to-end host tests: the composition root (config-free synthetic model).
//
// These cover what the engine-level tests cannot: the quote lifecycle, the
// maxTicksThroughMarket disable path, and the hedge reservation round trip through the
// host's own ports. The differential test (rv_tests.cpp) proves the numbers; this file
// proves the wiring.

#include <cmath>
#include <cstdio>
#include <vector>

#include <common/liquidity_ledger.hpp>

#include "host.hpp"
#include "model.hpp"
#include "state.hpp"
#include "valuation.hpp"

using namespace tachyon::trade::rv;
using namespace tachyon::trade;  // market_key / market_type / liquidity_source

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "pass" : "FAIL");
    if (!ok) {
        ++g_failures;
    }
}

// 2 outrights + 2 spreads, 2 vectors: the smallest shape that exercises leans, the club
// and the hedge walk.
struct fixture {
    model_inputs in;
    static constexpr uint32_t N_PRICING = 2;
    static constexpr uint32_t N_ALL = 4;
    static constexpr price_t TICK = to_scaled(0.25);

    fixture() {
        in.n_pricing = N_PRICING;
        in.n_vectors = N_PRICING;
        in.n_contracts = N_ALL;
        in.first_traded = N_PRICING;
        const size_t nn = static_cast<size_t>(N_ALL) * N_ALL;

        in.pricing_matrix = {1.0, 0.05, 0.05, 1.0};
        in.contracts.resize(N_ALL);
        std::vector<leg_input> legs;
        for (uint32_t s = 0; s < N_ALL - N_PRICING; ++s) {
            const uint32_t c = N_PRICING + s;
            in.contracts[c].leg_begin = static_cast<uint32_t>(legs.size());
            in.contracts[c].leg_count = 2;
            legs.push_back({0, s == 0 ? 1.0 : -1.0});
            legs.push_back({1, s == 0 ? -1.0 : 1.0});
        }
        in.legs = legs;

        in.hedge_ratio.assign(nn, 1.0);
        in.quote_size.assign(nn, 7.0);
        in.commissions.assign(nn, 34'000);
        in.leans.assign(nn, 1);
        in.factor_hedged_index.assign(nn, 0);
        in.rhone_max_clip.assign(N_ALL, 100);

        for (uint32_t c = 0; c < N_ALL; ++c) {
            contract_input& ci = in.contracts[c];
            ci.tick_size = TICK;
            ci.width = to_scaled(0.008);
            ci.extra_width = 0;
            ci.price_change_threshold = 0;
            ci.established_level = 1000;
            ci.lot_size = 1;
            ci.weight = c < N_PRICING ? 1.0 : 0.5;
            // Only the spread contracts are traded: the pricing outrights are inputs to the
            // ODM, not quotes. (A priced *and* quoted contract is legal -- see
            // rv_tests::test_priced_and_quoted_overlap -- but this fixture's all-ones lean
            // matrix would have every outright lean on a spread and trip the gate.)
            ci.traded = c >= N_PRICING;
            ci.quote_external = (c % 2) == 0;
            ci.static_quote = false;
            ci.max_ticks_through_market = 10'000 * TICK;  // generous: gate passes
        }
        for (uint32_t v = 0; v < N_PRICING; ++v) {
            vector_input vi;
            vi.range = 0.3 + 0.1 * v;
            vi.max_position = 1000.0;
            vi.initial_ltf = 0.0;
            vi.ema_period_cycles = 0;
            vi.use_ema = false;
            // set_to_market makes the short-term fair track the market, so quote prices react
            // to market data (with ltf = 0 and no EMA they would stay pinned at the width).
            vi.set_to_market = true;
            vi.factor_hedged = true;
            in.vectors.push_back(vi);
        }
    }
};

struct rec {
    uint64_t quotes = 0;
    uint64_t notifications = 0;
    uint64_t alerts = 0;
    std::vector<hedge_order> hedges;
    std::vector<quote_request> requests;

    static void on_hedge(void* ctx, const hedge_order& o) {
        static_cast<rec*>(ctx)->hedges.push_back(o);
    }
    static void on_quote(void* ctx, const quote_request& r) {
        auto* self = static_cast<rec*>(ctx);
        ++self->quotes;
        self->requests.push_back(r);
    }
    static void on_notify(void* ctx, uint32_t) { ++static_cast<rec*>(ctx)->notifications; }
    static void on_alert(void* ctx, const char*) { ++static_cast<rec*>(ctx)->alerts; }

    host_ports ports() {
        host_ports p{};
        p.send_hedge = &rec::on_hedge;
        p.send_quote = &rec::on_quote;
        p.notify_quote = &rec::on_notify;
        p.on_alert = &rec::on_alert;
        p.ctx = this;
        return p;
    }
};

void seed_markets(host& h, const model& m, price_t base) {
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const price_t mid = base + static_cast<price_t>(c) * m.tick[c].divisor;
        for (uint32_t i = 0; i < SIDE_COUNT; ++i) {
            const auto sd = static_cast<side>(i);
            const price_t px = sd == side::bid ? mid - m.tick[c].divisor : mid + m.tick[c].divisor;
            h.on_market_data(c, sd, false, px, 100);
            h.on_market_data(c, sd, true, px, 100);
        }
    }
}

void wire_all(host& h, const model& m) {
    for (uint32_t c = 0; c < m.n_all; ++c) {
        for (uint32_t i = 0; i < SIDE_COUNT; ++i) {
            const auto sd = static_cast<side>(i);
            const auto os = i == 0 ? order_side::BUY : order_side::SELL;
            h.wire_market(c, sd, false, make_market_key(c, os, market_type::INTERNAL),
                          liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE);
            h.wire_market(c, sd, true, make_market_key(c, os, market_type::EXTERNAL),
                          liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
        }
    }
}

// ---------------------------------------------------------------------------
// 1. quote lifecycle: emit on change, stay quiet when nothing moved, emit again
// ---------------------------------------------------------------------------
void test_quote_lifecycle() {
    fixture f;
    const model m = model::build(f.in);
    static cycle_state s;
    s.init(m);
    rec r;
    host h(m, s, nullptr, r.ports());
    seed_markets(h, m, 0);

    h.on_timer(1);
    check(r.quotes > 0, "first cycle emits quotes");
    const uint64_t first = r.quotes;

    h.on_timer(2);  // nothing changed since the last cycle
    check(r.quotes == first, "no re-emission when nothing changed");

    const price_t px = s.markets[market_index(0, side::bid, true)].price;
    h.on_market_data(0, side::bid, true, px - m.tick[0].divisor, 100);
    h.on_timer(3);
    check(r.quotes > first, "price change re-emits the affected quotes");

    bool lots_ok = true;
    for (const quote_request& q : r.requests) {
        if (m.lot[q.contract] > 1 && q.size % m.lot[q.contract] != 0) {
            lots_ok = false;
        }
    }
    check(lots_ok, "emitted sizes respect the contract lot size");
    check(!h.stats().disabled, "nothing disabled the strategy");
}

// ---------------------------------------------------------------------------
// 2. maxTicksThroughMarket: a single violation disables RV and pulls quotes
//    (RelativeValue.h:2871-2880)
// ---------------------------------------------------------------------------
void test_gate_disables() {
    fixture f;
    for (auto& ci : f.in.contracts) {
        ci.max_ticks_through_market = 0;  // reject every quote
    }
    const model m = model::build(f.in);
    static cycle_state s;
    s.init(m);
    rec r;
    host h(m, s, nullptr, r.ports());
    seed_markets(h, m, 0);

    h.on_timer(1);
    check(h.stats().validation_failures > 0, "gate reports a maxTicksThroughMarket violation");
    check(h.stats().disabled, "violation disables the strategy");
    check(r.alerts > 0, "violation raises a critical alert");
    check(!h.external_quoting_enabled(), "external quoting is disabled as well");

    const uint64_t quotes_before = r.quotes;
    h.on_timer(2);
    check(r.quotes == quotes_before, "a disabled strategy emits nothing further");
}

// ---------------------------------------------------------------------------
// 3. hedge -> reservation -> ack -> complete: liquidity is returned
// ---------------------------------------------------------------------------
void test_hedge_reservation_roundtrip() {
    fixture f;
    const model m = model::build(f.in);
    static cycle_state s;
    s.init(m);
    liquidity_ledger ledger;
    rec r;
    host h(m, s, &ledger, r.ports());
    wire_all(h, m);
    seed_markets(h, m, 0);

    h.on_timer(1);
    if (s.quote_of(fixture::N_PRICING, side::bid).price == tachyon::NULL_PRICE) {
        check(false, "traded contract has a bid quote to fill against");
        return;
    }
    h.on_fill(fixture::N_PRICING, side::bid, 3);
    h.on_timer(2);

    check(!r.hedges.empty(), "a fill produces a hedge order");
    check(h.stats().reservations_taken > 0, "the hedge reserves liquidity");
    check(ledger.active_reservation_count() > 0, "the ledger holds the reservation");

    for (const hedge_order& o : r.hedges) {
        h.on_hedge_ack(o.hedge_id, 1, o.size);
        h.on_hedge_done(o.hedge_id);
    }
    check(ledger.active_reservation_count() == 0, "ack + complete releases the reservation");
}

// ---------------------------------------------------------------------------
// 4. a replaced external price level invalidates its reservations
// ---------------------------------------------------------------------------
void test_price_change_invalidates() {
    fixture f;
    const model m = model::build(f.in);
    static cycle_state s;
    s.init(m);
    liquidity_ledger ledger;
    rec r;
    host h(m, s, &ledger, r.ports());
    wire_all(h, m);
    seed_markets(h, m, 0);

    // Make the external book the best one on every contract, so the hedge reserves an
    // *external* market and the flush branch (Common.h:636-644) is the one under test.
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const price_t internal_bid = s.markets[market_index(c, side::bid, false)].price;
        h.on_market_data(c, side::bid, true, internal_bid + 2 * m.tick[c].divisor, 100);
    }

    h.on_timer(1);
    h.on_fill(fixture::N_PRICING, side::bid, 2);
    h.on_timer(2);
    if (r.hedges.empty()) {
        check(false, "a fill produces a hedge order");
        return;
    }
    const hedge_order order = r.hedges.back();
    check(ledger.active_reservation_count() > 0, "reservation exists before the price change");

    // The reservation targets the market the engine named in market_index — which encodes the
    // *quote* side (O4), not order.side_ (the order side). Decode it and replace its price:
    // an external level drops the reservation (Market::update flush, Common.h:636-644), an
    // internal book never does (updateInternalMarket, :673-679).
    const uint32_t mi = order.market_index;
    const uint32_t contract = mi / (SIDE_COUNT * 2);
    const auto quote_side = static_cast<side>((mi % (SIDE_COUNT * 2)) / 2);
    const bool external = (mi % 2) != 0;
    const price_t old_price = s.markets[mi].price;
    h.on_market_data(contract, quote_side, external,
                     old_price + 5 * m.tick[contract].divisor, s.markets[mi].size);
    if (external) {
        check(ledger.active_reservation_count() == 0,
              "external price change drops the reservation");
    } else {
        check(ledger.active_reservation_count() > 0,
              "internal price change keeps the reservation");
    }
}

// ---------------------------------------------------------------------------
// 5. quotes blocked by an open IME transaction are deferred, not dropped
// ---------------------------------------------------------------------------
void test_blocked_quotes_deferred() {
    fixture f;
    const model m = model::build(f.in);
    static cycle_state s;
    s.init(m);
    rec r;
    host h(m, s, nullptr, r.ports());
    seed_markets(h, m, 0);

    h.set_can_send_quotes(false);
    h.on_timer(1);
    check(r.quotes == 0, "no quote leaves while quotes are blocked");
    check(h.stats().quotes_blocked > 0, "the block is counted");

    h.set_can_send_quotes(true);
    h.on_timer(2);
    check(r.quotes > 0, "the retained change is emitted once unblocked");
}

}  // namespace

int main() {
    std::printf("host: quote lifecycle\n");
    test_quote_lifecycle();
    std::printf("host: maxTicksThroughMarket disable path\n");
    test_gate_disables();
    std::printf("host: hedge reservation round trip\n");
    test_hedge_reservation_roundtrip();
    std::printf("host: price-change invalidation\n");
    test_price_change_invalidates();
    std::printf("host: blocked quotes are deferred\n");
    test_blocked_quotes_deferred();
    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

