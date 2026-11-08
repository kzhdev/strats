// P1 gate: the runtime state layer -- book updates, best-book selection (transcribed from
// legacy Contract::updateBestMarket, src/ArbieSpreader.h:313-341), path runtime and order,
// quotes, short-term fairs and dirty tracking.
//
//   as_state_tests
//
// Every expectation is derived from the legacy text, not from this implementation.

#include <cstdio>
#include <string>

#include "config.hpp"
#include "model.hpp"
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

// A ledger stub: one reservation-free "displayed" size per book, so the tie-break cases can be
// driven without the real liquidity_ledger (which is P6's bridge).
struct size_stub {
    qty_t internal = 0;
    qty_t external = 0;
    int price_changed_calls = 0;
    int last_price_changed_external = -1;
};

qty_t stub_size(void* ctx, uint32_t, side, bool external, size_filter) noexcept {
    size_stub* s = static_cast<size_stub*>(ctx);
    return external ? s->external : s->internal;
}

void stub_price_changed(void* ctx, uint32_t, side, bool external, qty_t) noexcept {
    size_stub* s = static_cast<size_stub*>(ctx);
    ++s->price_changed_calls;
    s->last_price_changed_external = external ? 1 : 0;
}

price_t px(double dollars) { return to_scaled(dollars); }

// ---------------------------------------------------------------------------
// 0. price scale (PRICE_SCALE must be 1e9 everywhere)
// ---------------------------------------------------------------------------
void test_price_scale() {
    std::printf("price scale\n");
    check_eq(PRICE_SCALE, 1'000'000'000LL, "PRICE_SCALE is 1e9");
    check_eq(PRICE_SCALE, tachyon::DEFAULT_FIXED_PRICE_MULTIPLIER,
             "PRICE_SCALE matches tachyon's fixed price multiplier");
    check_eq(LEGACY_PRICE_SCALE, 100'000'000LL, "legacy scale is 1e-8 (quantisation only)");
    check_eq(SCALE_RATIO, 10, "1e-9 is one decade finer than 1e-8");

    // a real price, one tick and one unit at 1e-9
    check_eq(px(60.0), 60'000'000'000LL, "to_scaled($60) == 6e10");
    check_eq(px(0.025), 25'000'000LL, "to_scaled(0.025) == 2.5e7 (one tick)");
    check_eq(px(1.0) + 1, 1'000'000'001LL, "one unit of resolution at 1e-9");
    check(to_dollars(PRICE_SCALE) == 1.0, "to_dollars round trip");
    check(to_dollars(1) == 1e-9, "1 unit is 1e-9 dollars");

    // book prices are stored at 1e9: nothing in the state layer rescales them
    cycle_state s;
    s.update_internal(0, side::bid, px(60.0), 5);
    check_eq(s.book(0, side::bid, false).price, 60'000'000'000LL, "internal book keeps 1e-9");
    s.update_internal(0, side::bid, px(60.0) + 1, 5);
    check_eq(s.book(0, side::bid, false).price, 60'000'000'001LL,
             "a one-unit change is representable");
}

// ---------------------------------------------------------------------------
// 1. book updates
// ---------------------------------------------------------------------------
void test_books() {
    std::printf("books (external flush vs internal silence)\n");
    size_stub stub{4, 4};
    const quote_policy policy{stub_size, &stub, stub_price_changed, &stub};

    cycle_state s;
    check(s.update_external(policy, 3, side::bid, px(60.0), 10), "first external update changes");
    check_eq(stub.price_changed_calls, 1, "a price change runs the flush hook once");
    check_eq(stub.last_price_changed_external, 1, "the flush is for the external book");
    check_eq(s.book(3, side::bid, true).price, px(60.0), "the new level landed");
    check_eq(s.book(3, side::bid, true).size, 10, "the new size landed");

    check(!s.update_external(policy, 3, side::bid, px(60.0), 10), "an identical update is not a change");
    check_eq(stub.price_changed_calls, 1, "no flush without a price change");

    check(s.update_external(policy, 3, side::bid, px(60.0), 7), "a size-only update is a change");
    check_eq(stub.price_changed_calls, 1, "a size-only update does not flush adjustments");

    check(s.update_external(policy, 3, side::bid, px(60.5), 7), "a price change is a change");
    check_eq(stub.price_changed_calls, 2, "the flush fires again on the next price change");

    // internal books never flush (Common.h:688-695)
    s.update_internal(3, side::bid, px(60.25), 3);
    check_eq(stub.price_changed_calls, 2, "internal updates never flush adjustments");
    check_eq(s.book(3, side::bid, false).price, px(60.25), "internal level landed");
    check_eq(s.book(3, side::bid, false).size, 3, "internal size landed");
}

// ---------------------------------------------------------------------------
// 2. best-book selection (legacy :313-341)
// ---------------------------------------------------------------------------
void test_best_book() {
    std::printf("best book (price, tie by size, the both-empty case, the `changed` rule)\n");
    size_stub stub{0, 0};
    const quote_policy policy{stub_size, &stub, stub_price_changed, &stub};
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    const model m = model::build(build_model_inputs(cfg));
    cycle_state s(m);
    const uint32_t c = 6;  // QMQ2-QMV2

    // both books empty -> legacy takes the FIRST branch (internal size 0 -> external)
    s.update_internal(c, side::bid, NULL_PRICE, 0);
    s.update_external(policy, c, side::bid, NULL_PRICE, 0);
    stub.internal = 0;
    stub.external = 0;
    s.refresh_best(m, policy, c, side::bid, false);
    check(s.best_is_external(c, side::bid), "both books empty -> external (legacy :318)");

    // internal only / external only
    stub.internal = 5;
    stub.external = 0;
    s.refresh_best(m, policy, c, side::bid, false);
    check(!s.best_is_external(c, side::bid), "internal size only -> internal (:320)");
    stub.internal = 0;
    stub.external = 5;
    s.refresh_best(m, policy, c, side::bid, false);
    check(s.best_is_external(c, side::bid), "external size only -> external");

    // both present: better price wins (BID: higher)
    stub.internal = 5;
    stub.external = 5;
    s.update_internal(c, side::bid, px(60.00), 5);
    s.update_external(policy, c, side::bid, px(60.25), 5);
    s.refresh_best(m, policy, c, side::bid, true);
    check(s.best_is_external(c, side::bid), "BID: higher external price wins");
    s.update_internal(c, side::bid, px(60.50), 5);
    s.refresh_best(m, policy, c, side::bid, false);
    check(!s.best_is_external(c, side::bid), "BID: higher internal price wins");

    // equal price, tie by size (smaller wins, :324-329)
    s.update_external(policy, c, side::bid, px(60.50), 5);
    stub.internal = 5;
    stub.external = 9;
    s.refresh_best(m, policy, c, side::bid, true);
    check(!s.best_is_external(c, side::bid), "equal price, internal smaller -> internal");
    stub.internal = 9;
    stub.external = 5;
    s.refresh_best(m, policy, c, side::bid, true);
    check(s.best_is_external(c, side::bid), "equal price, external smaller -> external");
    stub.internal = 5;
    stub.external = 5;
    s.refresh_best(m, policy, c, side::bid, true);
    check(!s.best_is_external(c, side::bid), "equal price and size -> internal (<=)");

    // ASK side: lower price wins
    s.update_internal(c, side::ask, px(60.50), 5);
    s.update_external(policy, c, side::ask, px(60.25), 5);
    s.refresh_best(m, policy, c, side::ask, true);
    check(s.best_is_external(c, side::ask), "ASK: lower external price wins");
    s.update_internal(c, side::ask, px(60.00), 5);
    s.refresh_best(m, policy, c, side::ask, false);
    check(!s.best_is_external(c, side::ask), "ASK: lower internal price wins");
}


// ---------------------------------------------------------------------------
// 2b. the `changed` predicate and the commission-adjusted best price
// ---------------------------------------------------------------------------
void test_changed_and_prices() {
    std::printf("changed predicate + commission-adjusted best price\n");
    size_stub stub{5, 5};
    const quote_policy policy{stub_size, &stub, stub_price_changed, &stub};
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    const model m = model::build(build_model_inputs(cfg));
    cycle_state s(m);
    const uint32_t c = 6;

    s.update_internal(c, side::bid, px(60.00), 5);
    s.update_external(policy, c, side::bid, px(60.25), 5);
    s.refresh_best(m, policy, c, side::bid, true);
    check(s.best_is_external(c, side::bid), "precondition: external is best");

    // drop the external price: internal takes over, but the update is still "changed" because
    // the updated book was the old best (:335)
    s.update_external(policy, c, side::bid, px(59.75), 5);
    check(s.refresh_best(m, policy, c, side::bid, true),
          "changed when the updated book was the old best");
    check(!s.best_is_external(c, side::bid), "internal is best now");

    // an update to a book that is neither the current nor the previous best is not a change
    s.update_external(policy, c, side::bid, px(60.25), 5);
    s.refresh_best(m, policy, c, side::bid, true);
    check(s.best_is_external(c, side::bid), "precondition: external is best again");
    s.update_internal(c, side::bid, px(59.50), 5);  // still worse than the external book
    check(!s.refresh_best(m, policy, c, side::bid, false),
          "not changed when the updated book is neither old nor new best");

    // commission-adjusted best price (:270-275)
    s.update_external(policy, c, side::bid, px(60.25), 5);
    s.refresh_best(m, policy, c, side::bid, true);
    check(s.best_is_external(c, side::bid), "precondition: external best again");
    check_eq(s.best_price_adjusted(m, c, side::bid), px(60.25) - m.ext_commission[c],
             "BID external best is net of the external commission");
    s.update_internal(c, side::bid, px(60.50), 5);
    s.refresh_best(m, policy, c, side::bid, false);
    check(!s.best_is_external(c, side::bid), "precondition: internal best");
    check_eq(s.best_price_adjusted(m, c, side::bid), px(60.50),
             "internal best is not commission-adjusted");
    // ASK external best: applyPriceOffset<ASK>(price, -commission) == price + commission
    s.update_external(policy, c, side::ask, px(60.25), 5);
    s.update_internal(c, side::ask, px(60.50), 5);
    s.refresh_best(m, policy, c, side::ask, true);
    check(s.best_is_external(c, side::ask), "precondition: external is the ASK best");
    check_eq(s.best_price_adjusted(m, c, side::ask), px(60.25) + m.ext_commission[c],
             "ASK external best adds the external commission");
    check_eq(ticks_through_market<side::ask>(px(60.0), px(60.25)), px(0.25),
             "ASK ticks-through-market is other - quote");
}


// ---------------------------------------------------------------------------
// 3. adjusted size (the policy seam)
// ---------------------------------------------------------------------------
void test_adjusted_size() {
    std::printf("adjusted size (policy seam, raw fallback, filter pass-through)\n");
    size_stub stub{11, 22};
    const quote_policy ledger{stub_size, &stub, nullptr, nullptr};
    const quote_policy none{};

    cycle_state s;
    s.update_internal(0, side::bid, px(60.0), 5);
    s.update_external(ledger, 0, side::bid, px(60.0), 7);

    check_eq(s.adjusted_size(none, 0, side::bid, false), 5, "no ledger -> raw internal size");
    check_eq(s.adjusted_size(none, 0, side::bid, true), 7, "no ledger -> raw external size");
    check_eq(s.adjusted_size(ledger, 0, side::bid, false), 11, "ledger -> internal size");
    check_eq(s.adjusted_size(ledger, 0, side::bid, true), 22, "ledger -> external size");
    check(ledger.has_ledger() && !none.has_ledger(), "has_ledger reports the seam correctly");
    check_eq(static_cast<long long>(all_reservations.exclude_mask), 0,
             "the default filter excludes no reservation");
    check_eq(static_cast<long long>(exclude_anticipated_hedge.exclude_mask),
             static_cast<long long>(reservation_kind::anticipated_hedge),
             "the FAK-pre-adjustment filter excludes anticipated hedges");
}

// ---------------------------------------------------------------------------
// 4. path runtime, order, quotes, fairs, dirty set
// ---------------------------------------------------------------------------
void test_path_runtime() {
    std::printf("path runtime, order, quotes, fairs, dirty\n");
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    const model m = model::build(build_model_inputs(cfg));
    cycle_state s(m);
    const uint32_t c = 6;  // QMQ2-QMV2, 4 paths

    check_eq(s.order_size(m, c, side::bid), 4, "4 bid path slots");
    check_eq(s.order_size(m, c, side::ask), 4, "4 ask path slots");
    int order_mismatch = 0;
    for (uint32_t rank = 0; rank < 4; ++rank) {
        const uint32_t slot = s.order_at(m, c, side::bid, rank);
        if (m.slot_side[slot] != to_index(side::bid)) ++order_mismatch;
        if (m.slot_contract[slot] != c) ++order_mismatch;
    }
    check_eq(order_mismatch, 0, "every bid order entry is a bid slot of the same contract");

    const uint32_t first = s.order_at(m, c, side::bid, 0);
    const uint32_t last = s.order_at(m, c, side::bid, 3);
    s.order_slot(m, c, side::bid, 0) = last;
    s.order_slot(m, c, side::bid, 3) = first;
    check_eq(s.order_at(m, c, side::bid, 0), last, "the order is mutable");
    check_eq(s.order_at(m, c, side::ask, 0), m.side_path_slot[m.side_path_begin[c * SIDE_COUNT + 1]],
             "the ask order is unaffected");
    s.reset_order(m, c, side::bid);
    check_eq(s.order_at(m, c, side::bid, 0), first, "reset_order restores config order");

    path_state& p = s.path(model::path_slot(0, side::bid));
    p.price = px(60.0);
    p.size = 7;
    p.size_determiner = 5;
    check(s.path(model::path_slot(0, side::bid)).valid(), "path 0 bid is valid");
    check(!s.path(model::path_slot(0, side::ask)).valid(), "path 0 ask is untouched");
    check_eq(s.path(model::path_slot(0, side::bid)).size, 7, "path size persisted");
    check_eq(s.path(model::path_slot(0, side::bid)).size_determiner, 5, "determiner persisted");
    s.path(model::path_slot(0, side::bid)).reset();
    check(!s.path(model::path_slot(0, side::bid)).valid(), "reset clears validity");
    check_eq(s.path(model::path_slot(0, side::bid)).size, 0, "reset clears the size");
}


// ---------------------------------------------------------------------------
// 5. quotes, fairs, flags, dirty set, init reset
// ---------------------------------------------------------------------------
void test_quotes_and_dirty() {
    std::printf("quotes, fairs, flags, dirty set, init\n");
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    const model m = model::build(build_model_inputs(cfg));
    cycle_state s(m);
    const uint32_t c = 6;

    // change detection compares the four emitted fields only (legacy Quote::operator!=,
    // ArbieSpreader.h:214-219 -- _leanPrice and _hedgePath are deliberately excluded)
    quote q;
    q.internal_price = px(60.0);
    q.internal_size = 5;
    q.external_price = px(60.0);
    q.external_size = 5;
    check(q == q, "a quote equals itself");
    check(s.last_sent(c, side::bid) != q, "nothing has been sent yet");
    q.lean_price = px(59.99);
    q.hedge_path = 3;
    check(q == q, "lean price and hedge path do not participate in change detection");
    quote other = q;
    other.internal_size = 6;
    check(q != other, "an internal size difference makes quotes unequal");
    other = q;
    other.external_price = px(60.25);
    check(q != other, "an external price difference makes quotes unequal");

    // force flags are per contract and side
    s.set_force_update(c, side::ask, true);
    check(s.force_update(c, side::ask), "force update set for the ask");
    check(!s.force_update(c, side::bid), "force update is not set for the bid");
    s.set_force_update(c, side::ask, false);
    check(!s.force_update(c, side::ask), "force update cleared");

    // short-term fairs start NULL and are per contract
    check(!s.has_stf(c), "no stf before an update");
    check_eq(s.stf(c), NULL_PRICE, "the stf sentinel is NULL_PRICE");
    s.set_stf(c, px(60.001));
    check(s.has_stf(c), "stf present after an update");
    check(!s.has_stf(c - 1), "stf is per contract");
    check_eq(s.offset_value(0), NULL_PRICE, "offset values start NULL");
    s.set_offset_value(0, px(0.5));
    check(s.has_offset_value(0), "offset value present after an update");

    // dirty set: at most once per contract, first-touch order, clearable
    s.mark_dirty(6);
    s.mark_dirty(2);
    s.mark_dirty(6);
    check_eq(s.dirty_count(), 2, "a contract is listed once");
    check_eq(s.dirty_at(0), 6, "first-touch order preserved");
    check_eq(s.dirty_at(1), 2, "second touched contract second");
    check(s.is_dirty(6) && s.is_dirty(2) && !s.is_dirty(3), "is_dirty is exact");
    s.clear_dirty();
    check_eq(s.dirty_count(), 0, "clear_dirty empties the list");
    check(!s.is_dirty(6), "clear_dirty clears the bits");
    s.mark_dirty(6);
    check_eq(s.dirty_count(), 1, "the set is reusable after a clear");

    // can_trade starts true (legacy Contract::_canTrade)
    check(s.can_trade(c), "can_trade defaults to true");
    s.set_can_trade(c, false);
    check(!s.can_trade(c), "can_trade can be cleared");

    // init() is a full reset
    cycle_state fresh(m);
    check_eq(fresh.dirty_count(), 0, "init clears the dirty set");
    check(!fresh.has_stf(c), "init clears the stfs");
    check(!fresh.path(model::path_slot(0, side::bid)).valid(), "init clears the path runtime");
    check(fresh.can_trade(c), "init restores can_trade");
    check_eq(fresh.adjusted_size(quote_policy{}, c, side::bid, false), 0, "init empties the books");
    check(fresh.last_sent(c, side::bid) == quote{}, "init clears the last sent quotes");
}

}  // namespace

int main() {
    test_price_scale();
    test_books();
    test_best_book();
    test_changed_and_prices();
    test_adjusted_size();
    test_path_runtime();
    test_quotes_and_dirty();

    if (g_failures == 0) {
        std::printf("\nall AS phase-1 checks passed\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}

