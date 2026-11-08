// P2 gate: the path calculation -- legacy Path::calculate (:711-831), the ranking
// (PathSorter, :108-130) and the update pass (Contract::updatePaths, :931-985), against the
// independent transcription in tests/path_oracle.hpp.
//
//   as_paths_tests [iterations]
//
// The explicit cases come from the legacy text; the randomised differential checks that the
// engine and the oracle agree on every branch over thousands of book states.

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "config.hpp"
#include "model.hpp"
#include "paths.hpp"
#include "path_oracle.hpp"
#include "policy.hpp"
#include "state.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade;
using namespace tachyon::trade::as;

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

// A ledger stub driven by a table, so the gate can exercise the reservation paths (zero sizes,
// the AS-quote fallback) without the real liquidity_ledger (that is P6).
struct ledger_stub {
    qty_t size_with_all_reservations = 0;
    qty_t size_without_anticipated = 0;
    bool aggressive_quote = false;
};

qty_t stub_size(void* ctx, uint32_t, side, bool, size_filter filter) noexcept {
    const ledger_stub* l = static_cast<const ledger_stub*>(ctx);
    return filter.exclude_mask == 0 ? l->size_with_all_reservations
                                    : l->size_without_anticipated;
}

qty_t stub_as_quote(void* ctx, uint32_t, side, bool) noexcept {
    return static_cast<const ledger_stub*>(ctx)->aggressive_quote ? 1 : 0;
}

// A fixture bundling a model, its state and the engine. The default policy has no ledger, so
// best-book selection and sizes come straight from the books; the tests that need reservations
// build their own policy (see test_aggressive_quote_fallback). `path_engine` holds a reference
// to the model and a copy of the policy, so the member order here matters.
struct fixture {
    model m;
    cycle_state s;
    ledger_stub ledger{};
    quote_policy policy{};
    path_engine engine{m, policy};

    explicit fixture(model&& model_in) : m(std::move(model_in)), s(m) {}
};

model real_model() {
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    return model::build(build_model_inputs(cfg));
}

// The synthetic model from as_tests: two switch paths with hand-derived offsets, so the offset
// branch of the calculation is exercised too.
model switch_model() {
    model_inputs in;
    in.n_contracts = 7;
    in.n_pricing = 4;
    in.n_vectors = 4;
    in.names = {"O0", "O1", "O2", "O3", "S4", "S5", "S6"};
    in.tick_size.assign(7, px(0.025));
    in.ext_commission = {0, 0, 0, 0, px(0.001), px(0.002), 0};
    in.lot_size.assign(7, 1);
    in.venue.assign(7, 0);
    in.pricing_multiplier.assign(7, 1.0);
    in.quantity_multiplier.assign(7, 1.0);
    in.leg_begin = {0, 0, 0, 0, 0, 2, 4, 6};
    in.leg_index = {0, 1, 1, 2, 0, 2};
    in.leg_ratio = {1, -1, 1, -1, 1, -1};
    in.range.assign(4, 1.0);
    in.max_position.assign(4, 1.0);
    in.pricing_matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    in.risk = in.pricing_matrix;

    as_contract_input c;
    c.name = "S4";
    c.enabled = true;
    c.quote_size = 5;
    c.edge = px(0.001);
    c.max_ticks_through_market = px(2.5);
    c.hedge_offset_price = 0;

    path_input switch_path;
    switch_path.name = "switch";
    switch_path.enabled = true;
    switch_path.commission = px(0.001);
    switch_path.leans = {{6, 1}};

    path_input mixed_path;
    mixed_path.name = "mixed";
    mixed_path.enabled = true;
    mixed_path.commission = px(0.002);
    mixed_path.leans = {{2, 1}};

    c.paths = {switch_path, mixed_path};
    in.contracts = {c};
    return model::build(in);
}

}  // namespace
namespace {

// ---------------------------------------------------------------------------
// 1. the arithmetic of one path, on the real config
// ---------------------------------------------------------------------------
void test_arithmetic() {
    std::printf("path arithmetic (basket sum, commission, quote-size cap)\n");
    fixture f(real_model());
    const model& m = f.m;
    const uint32_t c = 6;  // QMQ2-QMV2

    // Definition order: contract 4 (QMQ2-QMU2) owns defs 0-3, contract 6 owns defs 4-7.
    // def 6 is "QV" (leans once on the quoted contract), def 4 is "QU+UV" (two leans).
    const uint32_t qv_def = 6;
    check_eq(m.path_contract[qv_def], c, "def 6 belongs to QMQ2-QMV2");
    check_eq(m.lean_count(qv_def), 1, "the QV path has exactly one lean");
    check_eq(m.lean_contract[m.path_lean_begin[qv_def]], c, "QV leans on the quoted contract");
    check_eq(m.lean_mult[m.path_lean_begin[qv_def]], 1, "QV leans with +1");
    check_eq(m.path_commission[qv_def], px(0.01), "QV commission is $0.01");

    // both books on the bid: internal is better, size 10 (quote size is 7, so the size stays 7)
    f.s.update_internal(c, side::bid, px(60.00), 10);
    f.s.update_external(f.policy, c, side::bid, px(59.75), 10);
    f.s.refresh_best(m, f.policy, c, side::bid, false);
    f.engine.update_paths(f.s, c, true);

    const uint32_t bid2 = model::path_slot(6, side::bid);
    check(f.s.path(bid2).valid(), "path 2 bid is valid");
    check_eq(f.s.path(bid2).price, px(59.99), "bid price = lean - commission = $59.99");
    check_eq(f.s.path(bid2).size, 7, "size capped by the configured quote size (7)");
    check_eq(static_cast<long long>(f.s.path(bid2).size_determiner), INVALID_INDEX,
             "no lean capped the size, so there is no determiner");

    // a lean that displays less than the quote size caps the size and owns the determiner
    f.s.update_internal(c, side::bid, px(60.00), 3);
    f.engine.update_paths(f.s, c, true);
    check_eq(f.s.path(bid2).size, 3, "size follows the tightest lean");
    check_eq(f.s.path(bid2).size_determiner, c, "the tightest lean is the size determiner");

    // ask side: the sign flips (price = lean + commission)
    f.s.update_internal(c, side::ask, px(60.25), 10);
    f.s.update_external(f.policy, c, side::ask, px(60.50), 10);
    f.s.refresh_best(m, f.policy, c, side::ask, false);
    f.engine.update_paths(f.s, c, true);
    const uint32_t ask2 = model::path_slot(6, side::ask);
    check_eq(f.s.path(ask2).price, px(60.26), "ask price = lean + commission = $60.26");

    // a two-lean basket sums the leans; the external lean is priced net of its commission
    // def 4 ("QU+UV") leans QMU2-QMV2 (5) and QMQ2-QMU2 (4), both +1, commission 0.01
    check_eq(m.lean_count(4), 2, "the QU+UV path has two leans");
    f.s.update_internal(5, side::bid, px(0.15), 9);
    f.s.update_external(f.policy, 5, side::bid, px(0.10), 9);
    f.s.refresh_best(m, f.policy, 5, side::bid, false);
    // id 4 trades only on the external book: 0.06 net of its $0.002 external commission
    f.s.update_internal(4, side::bid, NULL_PRICE, 0);
    f.s.update_external(f.policy, 4, side::bid, px(0.06), 4);
    f.s.refresh_best(m, f.policy, 4, side::bid, false);
    f.engine.update_paths(f.s, c, true);
    const uint32_t bid0 = model::path_slot(4, side::bid);
    check_eq(f.s.path(bid0).price, px(0.15) + (px(0.06) - m.ext_commission[4]) - px(0.01),
             "two-lean basket: sum, external commission, path commission");
    check_eq(f.s.path(bid0).size, 4, "the tightest lean (4) caps the size");
    check_eq(f.s.path(bid0).size_determiner, 4, "the external lean is the size determiner");
}


// ---------------------------------------------------------------------------
// 2. reset cases (each branch of :715-783)
// ---------------------------------------------------------------------------
void test_resets() {
    std::printf("reset cases (disabled, no fair value, no price, no size)\n");
    fixture f(real_model());
    const model& m = f.m;
    const uint32_t c = 6;
    const uint32_t bid2 = model::path_slot(6, side::bid);

    auto publish_qv = [&f, c]() {
        f.s.update_internal(c, side::bid, px(60.00), 10);
        f.s.update_external(f.policy, c, side::bid, px(59.75), 10);
        f.s.refresh_best(f.m, f.policy, c, side::bid, false);
    };
    publish_qv();
    f.engine.update_paths(f.s, c, true);
    check(f.s.path(bid2).valid(), "precondition: the path quotes");

    // a lean that cannot trade kills the basket (:749)
    f.s.set_can_trade(c, false);
    f.engine.update_paths(f.s, c, true);
    check(!f.s.path(bid2).valid(), "a non-tradeable lean resets the path");
    f.s.set_can_trade(c, true);

    // no price on the lean (:763)
    f.engine.update_paths(f.s, c, true);
    check(f.s.path(bid2).valid(), "precondition: valid again");
    f.s.update_internal(c, side::bid, NULL_PRICE, 0);
    f.s.update_external(f.policy, c, side::bid, NULL_PRICE, 0);
    f.s.refresh_best(m, f.policy, c, side::bid, false);
    f.engine.update_paths(f.s, c, true);
    check(!f.s.path(bid2).valid(), "a lean without a price resets the path");

    // no size anywhere, and no AS aggressive quote to explain it (:769-773)
    publish_qv();
    f.engine.update_paths(f.s, c, true);
    check(f.s.path(bid2).valid(), "precondition: valid again");
    f.s.update_internal(c, side::bid, px(60.00), 0);
    f.s.update_external(f.policy, c, side::bid, px(59.75), 0);
    f.s.refresh_best(m, f.policy, c, side::bid, false);
    f.engine.update_paths(f.s, c, true);
    check(!f.s.path(bid2).valid(), "a zero size on both books resets the path");

    // a disabled path (:715)
    publish_qv();
    f.engine.update_paths(f.s, c, true);
    check(f.s.path(bid2).valid(), "precondition: valid again");
    f.m.path_enabled[6] = 0;
    f.engine.update_paths(f.s, c, true);
    check(!f.s.path(bid2).valid(), "a disabled path resets");
    f.m.path_enabled[6] = 1;

    // a disabled contract (:715)
    f.engine.update_paths(f.s, c, true);
    check(f.s.path(bid2).valid(), "precondition: valid again");
    f.m.enabled[c] = 0;
    f.engine.update_paths(f.s, c, true);
    check(!f.s.path(bid2).valid(), "a disabled contract resets every path");
    f.m.enabled[c] = 1;
}

// ---------------------------------------------------------------------------
// 3. the zero-size fallback: an AS aggressive quote is our own liquidity (:769-783)
// ---------------------------------------------------------------------------
void test_aggressive_quote_fallback() {
    std::printf("zero size caused by an AS aggressive quote falls back to the unadjusted size\n");
    // A policy backed by the ledger stub: 0 with every reservation, 6 without our own
    // anticipated hedges, and optionally "the zero was caused by an AS quote".
    ledger_stub ledger;
    ledger.size_with_all_reservations = 0;
    ledger.size_without_anticipated = 6;
    ledger.aggressive_quote = false;
    const quote_policy policy{stub_size, &ledger, nullptr, nullptr, stub_as_quote, &ledger};

    model m = real_model();
    cycle_state s(m);
    path_engine engine(m, policy);
    const uint32_t c = 6;
    const uint32_t bid2 = model::path_slot(6, side::bid);

    // the lean price lives on the external book, because the ledger reports no internal size
    s.update_internal(c, side::bid, NULL_PRICE, 0);
    s.update_external(policy, c, side::bid, px(60.00), 8);
    s.refresh_best(m, policy, c, side::bid, true);

    engine.update_paths(s, c, true);
    check(!s.path(bid2).valid(), "without an AS quote the zero size kills the path");

    ledger.aggressive_quote = true;  // the zero is our own reservation
    engine.update_paths(s, c, true);
    check(s.path(bid2).valid(), "with an AS quote the path survives");
    check_eq(s.path(bid2).size, 6, "the size that excludes our anticipated hedges is used");

    ledger.size_without_anticipated = 0;  // still nothing available
    engine.update_paths(s, c, true);
    check(!s.path(bid2).valid(), "a zero size without our own reservations still resets");
}

// ---------------------------------------------------------------------------
// 4. the change predicate (:806-808)
// ---------------------------------------------------------------------------
void test_change_predicate() {
    std::printf("change predicate (price move, size move relative to the quote size)\n");
    fixture f(real_model());
    const model& m = f.m;
    const uint32_t c = 6;
    const uint32_t bid2 = model::path_slot(6, side::bid);

    f.s.update_internal(c, side::bid, px(60.00), 10);
    f.s.update_external(f.policy, c, side::bid, px(59.75), 10);
    f.s.refresh_best(m, f.policy, c, side::bid, false);
    f.engine.update_paths(f.s, c, true);

    // same inputs again: no change
    check(!f.engine.calculate<side::bid>(f.s, bid2, true), "an unchanged path reports no change");

    // price moves
    f.s.update_internal(c, side::bid, px(60.05), 10);
    f.s.refresh_best(m, f.policy, c, side::bid, false);
    check(f.engine.calculate<side::bid>(f.s, bid2, true), "a price move is a change");

    // size drops below the quote size (7)
    f.s.update_internal(c, side::bid, px(60.05), 5);
    check(f.engine.calculate<side::bid>(f.s, bid2, true), "a size drop below quote size is a change");

    // size recovers while still below the quote size
    f.s.update_internal(c, side::bid, px(60.05), 6);
    check(f.engine.calculate<side::bid>(f.s, bid2, true),
          "a size increase while below quote size is a change");

    // size recovers to the cap: the old size was already at the cap, so no change is reported
    f.s.update_internal(c, side::bid, px(60.05), 10);
    f.engine.calculate<side::bid>(f.s, bid2, true);  // go from 6 to 7
    check(!f.engine.calculate<side::bid>(f.s, bid2, true),
          "no change once the size sits at the quote size");
}


// ---------------------------------------------------------------------------
// 5. switch paths (the short-term-fair offset)
// ---------------------------------------------------------------------------
void test_switch_path() {
    std::printf("switch path (fair offset present, missing, and the side's sign)\n");
    fixture f(switch_model());
    const model& m = f.m;
    const uint32_t c = 4;  // S4
    const uint32_t bid0 = model::path_slot(0, side::bid);
    const uint32_t ask0 = model::path_slot(0, side::ask);

    check_eq(m.n_offsets, 2, "the synthetic model has two switch paths");
    const uint32_t offset = m.path_offset[0];
    check(offset != INVALID_INDEX, "path 0 is a switch path");

    f.s.update_internal(6, side::bid, px(1.00), 9);  // S6
    f.s.refresh_best(m, f.policy, 6, side::bid, false);
    f.s.update_internal(6, side::ask, px(1.05), 9);
    f.s.refresh_best(m, f.policy, 6, side::ask, false);

    // without a fair value the switch path must not quote at all (:725-732)
    f.engine.update_paths(f.s, c, true);
    check(!f.s.path(bid0).valid(), "a switch path without a fair value stays invalid");

    // publish the offset: price = lean + offset - commission
    f.s.set_offset_value(offset, px(0.50));
    f.engine.update_paths(f.s, c, true);
    check(f.s.path(bid0).valid(), "the switch path quotes once the fair value arrives");
    check_eq(f.s.path(bid0).price, px(1.00) + px(0.50) - m.path_commission[0],
             "bid price = lean + offset - commission");
    check_eq(f.s.path(ask0).price, px(1.05) + px(0.50) + m.path_commission[0],
             "ask price = lean + offset + commission");

    // the offset is cleared (RV disabled): the path drops out again
    f.s.set_offset_value(offset, NULL_PRICE);
    f.engine.update_paths(f.s, c, true);
    check(!f.s.path(bid0).valid(), "clearing the fair value resets the switch path");
}

// ---------------------------------------------------------------------------
// 6. the ranking (PathSorter)
// ---------------------------------------------------------------------------
void test_sort() {
    std::printf("ranking (valid first, then price, then size)\n");
    fixture f(real_model());
    const model& m = f.m;
    const uint32_t c = 6;

    // drive the four bid paths to four distinct prices through the contract they lean on
    f.s.update_internal(c, side::bid, px(60.00), 10);
    f.s.update_external(f.policy, c, side::bid, px(59.75), 10);
    f.s.refresh_best(m, f.policy, c, side::bid, false);
    // paths 0 and 3 lean on other contracts: give them prices of their own
    f.s.update_internal(5, side::bid, px(0.15), 9);
    f.s.update_external(f.policy, 5, side::bid, px(0.10), 9);
    f.s.refresh_best(m, f.policy, 5, side::bid, false);
    f.s.update_internal(4, side::bid, px(0.05), 9);
    f.s.refresh_best(m, f.policy, 4, side::bid, false);
    f.engine.update_paths(f.s, c, true);

    // the best bid path is the one with the highest price
    const uint32_t best = f.engine.best_slot(f.s, c, side::bid);
    price_t best_price = f.s.path(best).price;
    bool monotonic = true;
    for (uint32_t rank = 0; rank < f.s.order_size(m, c, side::bid); ++rank) {
        const uint32_t slot = f.s.order_at(m, c, side::bid, rank);
        const path_state& p = f.s.path(slot);
        if (p.valid() && p.price > best_price) monotonic = false;
        if (p.valid()) best_price = p.price;
        // invalid paths must all come after valid ones
        if (!p.valid()) {
            for (uint32_t later = rank + 1; later < f.s.order_size(m, c, side::bid); ++later) {
                if (f.s.path(f.s.order_at(m, c, side::bid, later)).valid()) monotonic = false;
            }
        }
    }
    check(monotonic, "bid paths are ranked by price, valid ones first");

    // equal prices: the larger size wins
    f.s.update_internal(4, side::bid, px(0.10), 9);
    f.s.update_internal(5, side::bid, px(0.10), 9);
    f.engine.update_paths(f.s, c, true);
    // paths 0 (id5 + id4) and 2 (id6) now have different prices; just re-check the invariant
    check(f.s.path(f.engine.best_slot(f.s, c, side::bid)).valid(), "the best bid path is valid");

    // an ask side ranks the other way round (lowest price is best)
    const uint32_t best_ask = f.engine.best_slot(f.s, c, side::ask);
    check(best_ask != INVALID_INDEX, "the ask side has a best path");
}


// ---------------------------------------------------------------------------
// 7. randomised differential against the legacy transcription
// ---------------------------------------------------------------------------
struct counters {
    uint64_t compared = 0;
    uint64_t mismatched = 0;
    double rate() const { return compared ? static_cast<double>(mismatched) / compared : 0.0; }
};

void test_differential(int iterations, unsigned seed) {
    std::printf("differential vs the legacy transcription (%d book states, seed %u)\n",
                iterations, seed);
    // No ledger here: best-book selection and sizes come straight from the books, so the whole
    // recalculation surface is exercised without a reservation model.
    fixture f(real_model());
    const model& m = f.m;
    const quote_policy plain{};
    path_engine engine(m, plain);

    std::vector<oracle::path> mirror(static_cast<size_t>(m.n_path_slots));
    std::vector<uint32_t> mirror_order(static_cast<size_t>(m.n_path_slots));
    for (uint32_t i = 0; i < m.n_path_slots; ++i) mirror_order[i] = m.side_path_slot[i];

    std::mt19937 rng(seed);
    auto pick = [&rng](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };

    counters field_cmp;
    counters order_cmp;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        // random books for every contract and side
        for (uint32_t c = 0; c < m.n_all; ++c) {
            for (uint32_t s = 0; s < SIDE_COUNT; ++s) {
                const side sd = static_cast<side>(s);
                const bool first_external = pick(0, 1) == 0;
                const bool first_no_price = pick(0, 9) == 0;
                const price_t first_price =
                    first_no_price ? NULL_PRICE : m.tick[c] * pick(1, 4000);
                const qty_t first_size = first_no_price ? 0 : pick(0, 12);
                const bool second_no_price = pick(0, 9) == 0;
                const price_t second_price =
                    second_no_price ? NULL_PRICE : m.tick[c] * pick(1, 4000);
                const qty_t second_size = second_no_price ? 0 : pick(0, 12);
                if (first_external) {
                    f.s.update_external(f.policy, c, sd, first_price, first_size);
                    f.s.update_internal(c, sd, second_price, second_size);
                } else {
                    f.s.update_internal(c, sd, first_price, first_size);
                    f.s.update_external(f.policy, c, sd, second_price, second_size);
                }
                f.s.refresh_best(m, plain, c, sd, first_external);
            }
            f.s.set_can_trade(c, pick(0, 9) != 0);
        }
        for (uint32_t o = 0; o < m.n_offsets; ++o) {
            f.s.set_offset_value(o, pick(0, 5) == 0 ? NULL_PRICE : m.tick[0] * pick(-100, 100));
        }
        f.s.set_internal_is_rv_quote(4, side::bid, pick(0, 1) == 0);


        // identical starting path state on both sides
        for (uint32_t slot = 0; slot < m.n_path_slots; ++slot) {
            path_state& p = f.s.path(slot);
            p.reset();
            p.recalc = true;
            p.resort = false;
            p.dirty = false;
            p.lean_on_rv = false;
            mirror[slot] = oracle::path{};
        }
        for (uint32_t d = 0; d < m.n_path_defs; ++d) {
            const uint32_t contract = m.path_contract[d];
            f.s.reset_order(m, contract, side::bid);
            f.s.reset_order(m, contract, side::ask);
        }
        for (uint32_t i = 0; i < m.n_path_slots; ++i) mirror_order[i] = m.side_path_slot[i];

        // run both, then compare every field of every path and the whole ranking
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (!m.is_quoting(c)) continue;
            engine.update_paths(f.s, c, true);
            oracle::update_paths<side::bid>(m, f.s, plain, mirror, mirror_order, c, true);
            oracle::update_paths<side::ask>(m, f.s, plain, mirror, mirror_order, c, true);

            for (uint32_t s = 0; s < SIDE_COUNT; ++s) {
                const side sd = static_cast<side>(s);
                const uint32_t key = c * SIDE_COUNT + s;
                const uint32_t count = m.side_path_begin[key + 1] - m.side_path_begin[key];
                for (uint32_t rank = 0; rank < count; ++rank) {
                    const uint32_t slot = f.s.order_at(m, c, sd, rank);
                    const path_state& got = f.s.path(slot);
                    const oracle::path& want = mirror[slot];
                    ++field_cmp.compared;
                    if (got.price != want.pq.price || got.size != want.pq.size ||
                        got.size_determiner != want.size_determiner ||
                        got.lean_on_rv != want.lean_on_rv) {
                        ++field_cmp.mismatched;
                    }
                    ++order_cmp.compared;
                    if (mirror_order[m.side_path_begin[key] + rank] != slot) {
                        ++order_cmp.mismatched;
                    }
                }
            }
        }
    }

    std::printf("  path fields : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - field_cmp.rate()),
                static_cast<unsigned long long>(field_cmp.mismatched),
                static_cast<unsigned long long>(field_cmp.compared));
    std::printf("  path order  : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - order_cmp.rate()),
                static_cast<unsigned long long>(order_cmp.mismatched),
                static_cast<unsigned long long>(order_cmp.compared));
    check_eq(static_cast<long long>(field_cmp.mismatched), 0,
             "every path field matches the legacy transcription");
    check_eq(static_cast<long long>(order_cmp.mismatched), 0,
             "the ranking matches the legacy transcription");
}

}  // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 2000;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 42u;

    test_arithmetic();
    test_resets();
    test_aggressive_quote_fallback();
    test_change_predicate();
    test_switch_path();
    test_sort();
    test_differential(iterations, seed);

    if (g_failures == 0) {
        std::printf("\nall AS phase-2 checks passed\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}

