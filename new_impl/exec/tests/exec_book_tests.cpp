// Phase-1 gate for the new executor: the internal (implied) book of one contract.
//
//   exec_book_tests
//
// The book is the replacement for the legacy `_bids`/`_asks` price maps plus the `_bidLevels`
// external levels (InternalMatchingEngine.h:2140-2153). What is pinned here is exactly the part
// that is easy to get subtly wrong:
//
//   * apply/modify/remove bookkeeping, including the external leg of a quote;
//   * the price-collision rule, both the legacy one (D-EX11 `first_wins`, where a second order at
//     the same price is silently unmatchable) and the shipped one (`all_matchable`);
//   * the external-leg continuation of a cross (:1158-1211) -- an exhausted order keeps matching
//     at its own external price, which is why the residual sent to the exchange is smaller than
//     "internal size minus internal fill";
//   * `aggress`, the FAK path (:881-957), including its self-match skip and volume-weighted price.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "implied_book.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;

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

constexpr price_t PX(int64_t ticks) { return 25'000'000 * ticks; }

order_request quote(strategy_id s, side sd, int64_t ticks, qty_t size, price_t external,
                    qty_t external_size, bool persist = false) {
    return make_quote(s, 0, sd, PX(ticks), size, external, external_size, persist);
}

const implied_book::order* find(const implied_book& book, strategy_id s, request_kind k, side sd) {
    return book.find(s, k, sd);
}

// ---------------------------------------------------------------------------
// apply / remove
// ---------------------------------------------------------------------------
void test_apply_and_remove() {
    std::printf("book: apply, modify, remove\n");

    implied_book book;
    book.init(implied_book::price_collision::all_matchable, 0);
    check(!book.has_matchable(side::bid), "a fresh book is empty");

    book.apply(quote(strategy_id::MM, side::bid, 100, 5, PX(100), 5));
    check(book.has_matchable(side::bid), "the quote entered the book");
    check_eq(find(book, strategy_id::MM, request_kind::quote, side::bid)->size, 5, "with its size");
    check_eq(book.external(side::bid).price, PX(100), "and its external leg");
    check_eq(book.external(side::bid).size, 5, "with the external size");

    // A modify that only changes the size keeps the order in place.
    book.apply(quote(strategy_id::MM, side::bid, 100, 3, PX(100), 3));
    check_eq(find(book, strategy_id::MM, request_kind::quote, side::bid)->size, 3, "size updated");
    check_eq(book.external(side::bid).size, 3, "external size updated");

    // A modify that clears the internal side leaves no matchable order but keeps the external leg.
    order_request removal = quote(strategy_id::MM, side::bid, NULL_PRICE, 0, PX(100), 3);
    book.apply(removal);
    check(!book.has_matchable(side::bid), "the internal order is gone");
    check_eq(book.external(side::bid).price, PX(100), "the external price survives");

    book.remove(request_key{strategy_id::MM, request_kind::quote, side::bid});
    check(book.external(side::bid).price == NULL_PRICE, "remove clears the external leg too");
}

void test_persisted_price() {
    std::printf("book: persisted prices (getPersistedPrice)\n");

    implied_book book;
    book.init(implied_book::price_collision::all_matchable, 0);
    check(book.persisted_price(side::bid) == NULL_PRICE, "nothing persisted yet");

    book.apply(quote(strategy_id::MM, side::bid, 100, 5, PX(100), 5, /*persist=*/true));
    check_eq(book.persisted_price(side::bid), PX(100), "a persistent quote persists its price");

    book.apply(quote(strategy_id::RV, side::bid, 101, 5, PX(101), 5, /*persist=*/true));
    check_eq(book.persisted_price(side::bid), PX(101), "the best persisted price wins");

    // A non-persistent quote clears its own entry only.
    book.apply(quote(strategy_id::RV, side::bid, 102, 5, PX(102), 5, /*persist=*/false));
    check_eq(book.persisted_price(side::bid), PX(100), "a non-persistent quote stops persisting");
}

// ---------------------------------------------------------------------------
// Price collisions (D-EX11)
// ---------------------------------------------------------------------------
void test_price_collision() {
    std::printf("book: price collisions\n");

    // Legacy `first_wins`: the second order inserted at a price is not in the map, so it cannot be
    // matched, even though it still quotes externally.
    implied_book legacy;
    legacy.init(implied_book::price_collision::first_wins, 0);
    legacy.apply(quote(strategy_id::MM, side::bid, 100, 5, PX(100), 5));
    legacy.apply(quote(strategy_id::RV, side::bid, 100, 7, PX(100), 7));
    check(find(legacy, strategy_id::MM, request_kind::quote, side::bid)->matchable(),
          "the first order at a price stays matchable");
    check(!find(legacy, strategy_id::RV, request_kind::quote, side::bid)->matchable(),
          "the second order at a price is shadowed (legacy emplace behaviour)");

    // Shipped `all_matchable`: both are matchable, and the earlier one wins the tie-break.
    implied_book fixed;
    fixed.init(implied_book::price_collision::all_matchable, 0);
    fixed.apply(quote(strategy_id::MM, side::bid, 100, 5, PX(100), 5));
    fixed.apply(quote(strategy_id::RV, side::bid, 100, 7, PX(100), 7));
    check(find(fixed, strategy_id::RV, request_kind::quote, side::bid)->matchable(),
          "every order at a price is matchable when the policy is fixed");
    fill_buffer fills;
    const cross_result result = fixed.cross(strategy_id::MM, fills);
    check(result.ok, "no self match");
    check_eq(fills.count, 0, "both are bids: nothing crosses yet");
}

// ---------------------------------------------------------------------------
// The external-leg continuation (:1158-1211)
// ---------------------------------------------------------------------------
void test_external_leg_continuation() {
    std::printf("book: an exhausted order keeps matching on its external leg\n");

    implied_book book;
    book.init(implied_book::price_collision::all_matchable, 0);

    // RV bids 10 at 101 (aggressor). MM offers 3 internally at 100 and 5 externally at 99: once
    // its internal size is exhausted the external leg keeps matching at 99, exactly as legacy
    // matchBidExternal/matchAskExternal do.
    book.apply(quote(strategy_id::RV, side::bid, 101, 10, PX(101), 10));
    book.apply(quote(strategy_id::MM, side::ask, 100, 3, PX(99), 5));

    fill_buffer fills;
    const cross_result result = book.cross(strategy_id::RV, fills);
    check(result.ok, "the cross succeeded");
    check_eq(fills.count, 4, "two fills, each reported to both orders");
    check_eq(fills.items[0].size, 3, "first fill is the internal size");
    check_eq(fills.items[0].price, PX(100), "at the ask's passive internal price");
    // The internal fill also reduces the external size (legacy doExternalReductionAfterInternalFill
    // runs for *both* orders), so the external leg can only offer 5 - 3 = 2 afterwards.
    check_eq(fills.items[2].size, 2, "second fill is what the external leg still had");
    check_eq(fills.items[2].price, PX(99), "at the ask's own external price");

    // The residual is what is left of the bid: 10 - 3 - 2 = 5.
    check_eq(find(book, strategy_id::RV, request_kind::quote, side::bid)->size, 5,
             "the bid has 5 left internally");
    check_eq(find(book, strategy_id::RV, request_kind::quote, side::bid)->external_size, 5,
             "the bid's external size was reduced by the internal fill");
    check_eq(find(book, strategy_id::MM, request_kind::quote, side::ask)->external_size, 0,
             "the ask's external leg is used up");
    check(!book.has_matchable(side::ask), "the ask is out of the book");
}

// ---------------------------------------------------------------------------
// aggress(): the FAK path
// ---------------------------------------------------------------------------
void test_aggress() {
    std::printf("book: FAK aggress\n");

    implied_book book;
    book.init(implied_book::price_collision::all_matchable, 0);
    // One order per addressable stream, so two ask levels need two strategies.
    book.apply(quote(strategy_id::MM, side::ask, 100, 2, PX(100), 2));
    book.apply(quote(strategy_id::RV, side::ask, 102, 4, PX(102), 4));

    fill_buffer fills;
    // AS bids up to 101: only the 100 level is acceptable.
    implied_book::aggressive_result result =
        book.aggress(side::bid, strategy_id::AS, PX(101), 10, fills);
    check_eq(result.filled, 2, "filled what the acceptable level held");
    check_eq(result.remaining, 8, "the rest has to go elsewhere");
    check_eq(result.vwap, PX(100), "volume-weighted price");
    check_eq(fills.count, 1, "one passive fill recorded");

    // Raising the limit takes the second level, and the price is a true VWAP across both fills.
    fills.clear();
    book.apply(quote(strategy_id::MM, side::ask, 100, 2, PX(100), 2));
    result = book.aggress(side::bid, strategy_id::AS, PX(110), 4, fills);
    check_eq(result.filled, 4, "both levels were taken");
    check_eq(result.vwap, PX(101), "VWAP of 2@100 and 2@102 is 101");
    check_eq(book.has_matchable(side::ask), 1, "the 102 level still rests");
}

void test_aggress_skips_own_orders() {
    std::printf("book: FAK aggress skips its own orders\n");

    implied_book book;
    book.init(implied_book::price_collision::all_matchable, 0);
    book.apply(quote(strategy_id::RV, side::ask, 100, 3, PX(100), 3));
    book.apply(quote(strategy_id::MM, side::ask, 101, 4, PX(101), 4));

    fill_buffer fills;
    const implied_book::aggressive_result result =
        book.aggress(side::bid, strategy_id::RV, PX(110), 6, fills);
    check_eq(result.self_matches, 1, "RV's own order was skipped and counted");
    check_eq(result.filled, 4, "only the MM order was taken");
    check_eq(result.remaining, 2, "the rest is unmatched");
    check_eq(book.external(side::ask).price, PX(100), "RV's own order is still resting");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_apply_and_remove();
    test_persisted_price();
    test_price_collision();
    test_external_leg_continuation();
    test_aggress();
    test_aggress_skips_own_orders();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-1 checks passed\n");
    return EXIT_SUCCESS;
}
