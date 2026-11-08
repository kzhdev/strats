// P5 gate: the hedge stage -- aggressive-quote undo order, the canHedge veto and its forced
// recalculation, the per-lean order construction, and the switch-path transfer.
//
//   as_hedge_tests [iterations] [seed]

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "config.hpp"
#include "hedger.hpp"
#include "hedge_oracle.hpp"
#include "model.hpp"
#include "paths.hpp"
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

// A ledger stub whose aggressive-quote answers are a pure function of the arguments, so the engine
// and the oracle can both query it without interfering (the real ledger lives in P6).
struct quote_stub {
    qty_t internal = 0;   // (contract, side, internal)
    qty_t external = 0;   // (contract, side, external)
    int undo_calls = 0;
    qty_t last_undo_qty = -1;
    side last_undo_side = side::bid;
    bool last_undo_external = false;
};

// no ledger: raw book sizes, but the AS-quote hooks below stay active

qty_t stub_as_quote(void* ctx, uint32_t, side, bool external) noexcept {
    const quote_stub* s = static_cast<const quote_stub*>(ctx);
    return external ? s->external : s->internal;
}

bool stub_undo(void* ctx, uint32_t, side sd, bool external, qty_t qty) noexcept {
    quote_stub* s = static_cast<quote_stub*>(ctx);
    ++s->undo_calls;
    s->last_undo_qty = qty;
    s->last_undo_side = sd;
    s->last_undo_external = external;
    return true;
}

model real_model() {
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    return model::build(build_model_inputs(cfg));
}

// The synthetic model from the earlier gates: S4 quoted with a switch path (leans S6) and a plain
// path (leans O2), so both hedge flavours are reachable.
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
    path_input sw;
    sw.name = "switch";
    sw.enabled = true;
    sw.commission = px(0.001);
    sw.leans = {{6, 1}};
    path_input mixed;
    mixed.name = "mixed";
    mixed.enabled = true;
    mixed.commission = px(0.002);
    mixed.leans = {{2, 1}};
    c.paths = {sw, mixed};
    in.contracts = {c};
    return model::build(in);
}

}  // namespace
namespace {

// ---------------------------------------------------------------------------
// 1. the hedge legs on the real config
// ---------------------------------------------------------------------------
void test_legs() {
    std::printf("hedge legs (direction, price, payup, market)\n");
    model m = real_model();
    cycle_state s(m);
    quote_stub stub;
    const quote_policy policy{nullptr, nullptr, nullptr, nullptr, stub_as_quote, &stub, stub_undo,
                              &stub};
    path_engine paths(m, policy);
    hedger hedger(m, paths, policy);

    const uint32_t c = 6;  // QMQ2-QMV2, hedge offset 0.0004 * tick(0.025) = $0.00001
    // path 6 ("QV") leans on the quoted contract with +1, so one leg on the other side
    s.update_internal(c, side::bid, px(60.00), 10);
    s.update_external(policy, c, side::bid, px(59.75), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);

    const hedge_plan plan = hedger.hedge(s, c, side::bid, 3, false);
    check(plan.hedged, "the best path can hedge a 3-lot bid fill");
    check_eq(plan.leg_count, 1, "the QV path has one lean, so one hedge leg");
    const hedge_leg& leg = plan.legs[0];
    check_eq(leg.contract, c, "the lean is the quoted contract itself");
    check(leg.order_side == side::ask, "a bought bid is hedged with an ask order");
    // the path leaned on the bid book, so the hedge is priced off the bid plus the payup
    check_eq(leg.external_price, px(60.00), "the external price is the raw lean price");
    check_eq(leg.internal_price, px(60.00) - m.hedge_offset_price[c],
             "an ask hedge is priced off the bid minus the payup");
    check_eq(leg.internal_price, px(60.00) - px(0.00001), "the payup is $0.00001");
    check(!leg.external, "the internal book is the best one");
    check_eq(leg.size, 3, "the leg carries the whole fill quantity");
    check_eq(leg.market_index, market_index(c, side::bid, false), "the leg names the leaned book");
    check_eq(plan.path_id, 2, "the hedge records the path's own id (QV is the third path)");
    check(!plan.switch_transfer, "the real config's paths carry no fair offset, so no RV transfer");

    // the ask side mirrors: a sold ask is hedged with a bid order, priced off the ask plus payup
    s.update_internal(c, side::ask, px(60.25), 10);
    s.update_external(policy, c, side::ask, px(60.50), 10);
    s.refresh_best(m, policy, c, side::ask, false);
    paths.update_paths(s, c, true);
    const hedge_plan ask_plan = hedger.hedge(s, c, side::ask, 2, false);
    check(ask_plan.hedged, "the ask side hedges too");
    check(ask_plan.legs[0].order_side == side::bid, "a sold ask is hedged with a bid order");
    check_eq(ask_plan.legs[0].internal_price, px(60.25) + m.hedge_offset_price[c],
             "a bid hedge is priced off the ask plus the payup");

    // a second hedge event gets a new id
    const hedge_plan again = hedger.hedge(s, c, side::bid, 3, false);
    check(again.hedge_id != plan.hedge_id, "each hedge event has its own id");
    check(again.hedge_id > plan.hedge_id, "the id is monotonic");
}

// ---------------------------------------------------------------------------
// 2. the aggressive-quote release order
// ---------------------------------------------------------------------------
void test_undo() {
    std::printf("aggressive-quote release order\n");
    model m = real_model();
    cycle_state s(m);
    quote_stub stub;
    const quote_policy policy{nullptr, nullptr, nullptr, nullptr, stub_as_quote, &stub, stub_undo,
                              &stub};
    path_engine paths(m, policy);
    hedger hedger(m, paths, policy);

    const uint32_t c = 6;
    s.update_internal(c, side::bid, px(60.00), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);

    // an external fill with nothing reserved: no call at all
    stub.internal = 0;
    stub.external = 0;
    stub.undo_calls = 0;
    hedger.hedge(s, c, side::bid, 3, false);
    check_eq(stub.undo_calls, 0, "an external fill with no reservations releases nothing");

    // an internal fill releases the same-side internal book, then the same-side external one
    // (capped at the fill quantity), then the opposite-side external book
    stub.internal = 5;
    stub.external = 2;
    stub.undo_calls = 0;
    hedger.hedge(s, c, side::bid, 3, true);
    check_eq(stub.undo_calls, 3, "an internal fill releases three books");
    check_eq(stub.last_undo_qty, 2, "the opposite book is released up to the fill quantity");
    check(stub.last_undo_side == side::ask, "the last release is the opposite side");
    check(stub.last_undo_external, "and it is the external book");

    // an external fill still releases the opposite side
    stub.internal = 0;
    stub.external = 0;
    stub.undo_calls = 0;
    hedger.hedge(s, c, side::bid, 3, false);
    check_eq(stub.undo_calls, 0, "nothing reserved on the opposite side -> no call");
    stub.external = 4;
    stub.undo_calls = 0;
    hedger.hedge(s, c, side::bid, 3, false);
    check_eq(stub.undo_calls, 1, "the opposite external book is released");
    check_eq(stub.last_undo_qty, 3, "capped at the fill quantity (3 of 4)");
}


// ---------------------------------------------------------------------------
// 3. the canHedge veto and its forced recalculation
// ---------------------------------------------------------------------------
void test_veto_and_recalc() {
    std::printf("canHedge veto (a stale lean forces a recalculation)\n");
    model m = real_model();
    cycle_state s(m);
    quote_stub stub;
    const quote_policy policy{nullptr, nullptr, nullptr, nullptr, stub_as_quote, &stub, stub_undo,
                              &stub};
    path_engine paths(m, policy);
    hedger hedger(m, paths, policy);

    const uint32_t c = 6;
    s.update_internal(c, side::bid, px(60.00), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);
    check(hedger.hedge(s, c, side::bid, 2, false).hedged, "precondition: the path hedges");

    // move the lean *worse* without telling the paths: the guard exists for exactly this, because
    // the path was priced on a better book than the one that is now available
    const uint32_t slot = paths.best_slot(s, c, side::bid);
    check(slot != INVALID_INDEX, "precondition: a best path exists");
    s.update_internal(c, side::bid, px(59.50), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    check(!paths.can_hedge(s, slot, 2), "the stale path fails canHedge");

    const hedge_plan plan = hedger.hedge(s, c, side::bid, 2, false);
    check(plan.recalc_forced, "the veto forced a recalculation");
    check(plan.hedged, "and the recalculated path hedges");
    check_eq(plan.legs[0].internal_price, px(59.50) - m.hedge_offset_price[c],
             "the leg is priced from the re-priced path");

    // a lean that cannot display the quantity vetoes too, but legacy only re-checks *validity*
    // after the forced recalculation -- so the hedge still goes out. Pinned as legacy behaviour.
    s.update_internal(c, side::bid, px(60.00), 1);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);
    const hedge_plan small = hedger.hedge(s, c, side::bid, 5, false);
    check(small.recalc_forced, "an under-sized lean forces a recalculation");
    check(small.hedged, "legacy then accepts the path on validity alone (no second canHedge)");
    check_eq(small.leg_count, 1, "and sends the legs for the full quantity");
}

// ---------------------------------------------------------------------------
// 4. the switch-path transfer
// ---------------------------------------------------------------------------
void test_switch_transfer() {
    std::printf("switch path transfer (AS -> RV)\n");
    model m = switch_model();
    cycle_state s(m);
    quote_stub stub;
    const quote_policy policy{nullptr, nullptr, nullptr, nullptr, stub_as_quote, &stub, stub_undo,
                              &stub};
    path_engine paths(m, policy);
    hedger hedger(m, paths, policy);

    const uint32_t c = 4;  // S4
    check_eq(m.n_offsets, 2, "the synthetic model has two switch paths");
    // publish the fair value so the switch path quotes
    s.set_offset_value(m.path_offset[0], px(0.50));
    s.set_stf(6, px(1.00));    // S6, the lean of path 0
    s.set_stf(2, px(0.40));    // O2, the lean of path 1
    s.set_stf(4, px(1.50));    // S4 itself
    s.update_internal(6, side::bid, px(1.00), 9);
    s.refresh_best(m, policy, 6, side::bid, false);
    paths.update_paths(s, c, true);

    const hedge_plan plan = hedger.hedge(s, c, side::bid, 4, false);
    check(plan.hedged, "the switch path hedges");
    check(plan.switch_transfer, "a switch path transfers the position to RV");
    check_eq(plan.quoted.contract, c, "the transfer names the quoted contract");
    check_eq(plan.quoted.stf, px(1.50), "with its short-term fair");
    check(plan.quoted.front_side == side::ask, "the front notification is on the hedge side");
    check(plan.quoted.rv_side == side::bid, "the RV transfer is on the fill side");
    check_eq(plan.switch_leg_count, 1, "one transfer leg per lean");
    check_eq(plan.switch_legs[0].contract, 6, "the transfer leg names the lean");
    check_eq(plan.switch_legs[0].stf, px(1.00), "with the lean's fair value");
    check(plan.switch_legs[0].front_side == side::bid,
          "the front notification reports the fill side");
    check(plan.switch_legs[0].rv_side == side::ask, "the RV fill reports the hedge side");
    // legacy quoteAdjustedFillPrice = stf + rvSTFMove * qty
    check_eq(plan.adjusted_fill_price,
             px(1.50) + m.path_rv_stf_move[0] * 4,
             "the adjusted fill price is stf + fair move * quantity (D-AS8)");
    check(m.path_rv_stf_move[0] != 0, "the synthetic path has a non-zero fair move");

    // the synthetic model's two paths are both switch paths (P0 derived {S5:-1} and
    // {O2:-1, S4:+1}); the non-switch branch is covered in test_legs on the real config
    check_eq(m.n_offsets, 2, "both synthetic paths carry a fair offset");
}


// ---------------------------------------------------------------------------
// 5. randomised differential against the legacy transcription
// ---------------------------------------------------------------------------
struct counters {
    uint64_t compared = 0;
    uint64_t mismatched = 0;
    double rate() const { return compared ? static_cast<double>(mismatched) / compared : 0.0; }
    void add(bool same) {
        ++compared;
        if (!same) ++mismatched;
    }
};

void test_differential(int iterations, unsigned seed, model&& model_in, const char* label) {
    std::printf("differential vs the legacy transcription: %s (%d states, seed %u)\n", label,
                iterations, seed);
    model m = std::move(model_in);
    cycle_state s(m);
    quote_stub stub;
    const quote_policy policy{nullptr, nullptr, nullptr, nullptr, stub_as_quote, &stub, stub_undo,
                              &stub};
    path_engine paths(m, policy);
    hedger hedger(m, paths, policy);
    uint32_t oracle_hedge_id = 0;

    std::mt19937 rng(seed);
    auto pick = [&rng](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };

    counters plans;
    counters legs;
    counters transfer;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (uint32_t c = 0; c < m.n_all; ++c) {
            for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
                const side sd = static_cast<side>(si);
                const bool external = pick(0, 1) == 0;
                const bool no_price = pick(0, 9) == 0;
                const price_t price = no_price ? NULL_PRICE : m.tick[c] * pick(1, 4000);
                const qty_t size = no_price ? 0 : pick(0, 12);
                if (external) {
                    s.update_external(policy, c, sd, price, size);
                } else {
                    s.update_internal(c, sd, price, size);
                }
                s.refresh_best(m, policy, c, sd, external);
                s.set_can_trade(c, pick(0, 9) != 0);
                if (pick(0, 3) == 0) s.set_stf(c, m.tick[c] * pick(-50, 50));
            }
        }
        for (uint32_t o = 0; o < m.n_offsets; ++o) paths.update_offset(s, o);
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (m.is_quoting(c)) paths.update_paths(s, c, true);
        }

        stub.internal = pick(0, 2) == 0 ? 0 : pick(0, 6);
        stub.external = pick(0, 2) == 0 ? 0 : pick(0, 6);

        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (!m.is_quoting(c)) continue;
            const side sd = pick(0, 1) == 0 ? side::bid : side::ask;
            const qty_t qty = pick(1, 6);
            const bool is_internal = pick(0, 1) == 0;

            const hedge_plan got = hedger.hedge(s, c, sd, qty, is_internal);
            const hedge_plan want =
                sd == side::bid
                    ? oracle::hedge<side::bid>(m, s, paths, policy, oracle_hedge_id, c, qty,
                                               is_internal)
                    : oracle::hedge<side::ask>(m, s, paths, policy, oracle_hedge_id, c, qty,
                                               is_internal);

            plans.add(got.hedged == want.hedged && got.recalc_forced == want.recalc_forced &&
                      got.path_id == want.path_id && got.hedge_id >= want.hedge_id &&
                      got.switch_transfer == want.switch_transfer &&
                      got.adjusted_fill_price == want.adjusted_fill_price &&
                      got.switch_leg_count == want.switch_leg_count);
            legs.add(got.leg_count == want.leg_count);
            if (got.leg_count == want.leg_count) {
                for (uint8_t i = 0; i < got.leg_count; ++i) {
                    const hedge_leg& a = got.legs[i];
                    const hedge_leg& b = want.legs[i];
                    legs.add(a.contract == b.contract && a.order_side == b.order_side &&
                             a.internal_price == b.internal_price &&
                             a.external_price == b.external_price && a.size == b.size &&
                             a.external == b.external && a.market_index == b.market_index);
                }
            }
            transfer.add(got.quoted.contract == want.quoted.contract &&
                         got.quoted.stf == want.quoted.stf &&
                         got.quoted.front_side == want.quoted.front_side &&
                         got.quoted.rv_side == want.quoted.rv_side);
            for (uint8_t i = 0; i < got.switch_leg_count && i < want.switch_leg_count; ++i) {
                transfer.add(got.switch_legs[i].contract == want.switch_legs[i].contract &&
                             got.switch_legs[i].stf == want.switch_legs[i].stf &&
                             got.switch_legs[i].front_side == want.switch_legs[i].front_side &&
                             got.switch_legs[i].rv_side == want.switch_legs[i].rv_side);
            }
        }
    }


    std::printf("  plan          : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - plans.rate()),
                static_cast<unsigned long long>(plans.mismatched),
                static_cast<unsigned long long>(plans.compared));
    std::printf("  legs          : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - legs.rate()),
                static_cast<unsigned long long>(legs.mismatched),
                static_cast<unsigned long long>(legs.compared));
    std::printf("  switch legs   : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - transfer.rate()),
                static_cast<unsigned long long>(transfer.mismatched),
                static_cast<unsigned long long>(transfer.compared));
    check_eq(static_cast<long long>(plans.mismatched), 0,
             std::string("the hedge plan matches the legacy transcription: ") + label);
    check_eq(static_cast<long long>(legs.mismatched), 0,
             std::string("every hedge leg matches the legacy transcription: ") + label);
    check_eq(static_cast<long long>(transfer.mismatched), 0,
             std::string("the switch transfer matches the legacy transcription: ") + label);
}

}  // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 500;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 42u;

    test_legs();
    test_undo();
    test_veto_and_recalc();
    test_switch_transfer();
    test_differential(iterations, seed, real_model(), "real config");
    test_differential(iterations, seed + 1, switch_model(), "switch paths");

    if (g_failures == 0) {
        std::printf("\nall AS phase-5 checks passed\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}

