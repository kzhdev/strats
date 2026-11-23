// Phase-1b gate: the differential between the new internal book and an independent transcription
// of the legacy engine (tests/match_oracle.hpp).
//
//   exec_match_tests [steps] [seed]
//
// The two implementations share no code: the oracle keeps the legacy containers (std::map keyed by
// price, a per-price PriceLevel with a sorted std::vector) and the legacy control flow, while the new
// book is the flat 12-slot design. The same random request tape is pushed through both, and after
// every step every observable is compared:
//
//   * each slot's price/size/external leg/book membership;
//   * the aggregated external top per side (price and size);
//   * the persisted price per side;
//   * the published internal best per (subscriber, side), including the liquidity source;
//   * the fill list of a cross, in order (strategy, kind, side, price, size);
//   * the abort flag and the AS bookkeeping of a cross;
//   * the FAK aggressor's filled/remaining/VWAP/self-match count and the passive fills.
//
// The gate is exact (zero mismatches), not a percentage: both implementations are deterministic,
// so any diff is a defect in one of them. The one *intended* deviation -- the price-collision rule
// (PLAN D-EX11) -- is exercised separately in both modes.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <random>
#include <string>

#include "implied_book.hpp"
#include "match_oracle.hpp"
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

constexpr price_t TICK = 25'000'000;  // 0.025 at 1e-9
constexpr uint32_t CONTRACT = 0;

price_t px(int64_t ticks) { return TICK * ticks; }

// A field-by-field diff, capped so a systematic break does not flood the log.
int g_step = 0;  // the tape step the diff belongs to, for the report

struct diff_report {
    const char* field;
    uint64_t mismatches = 0;
    uint64_t compared = 0;

    void hit(const char* what, long long a, long long b) {
        ++compared;
        if (a == b) return;
        ++mismatches;
        if (mismatches <= 3) {
            std::printf("  DIFF [step %d] %s: %s engine=%lld legacy=%lld\n", g_step, field, what, a,
                        b);
        }
    }
    void rate() const {
        std::printf("  %-22s %8llu / %-9llu %s\n", field, (unsigned long long)mismatches,
                    (unsigned long long)compared, mismatches == 0 ? "identical" : "DIFFERS");
    }
};

}  // namespace

namespace {

// One comparator drives every case: it owns the per-field counters and knows how to diff one book
// state against one legacy state.
struct comparator {
    diff_report price{"slot price"};
    diff_report size{"slot size"};
    diff_report ext_price{"slot ext price"};
    diff_report ext_size{"slot ext size"};
    diff_report in_book{"slot in book"};
    diff_report ext_top{"external top"};
    diff_report persisted{"persisted price"};
    diff_report published{"published best"};
    diff_report fills{"fills"};
    diff_report cross_flags{"cross flags"};
    diff_report aggress{"aggress result"};

    void slots(const implied_book& book, const oracle::ime& legacy) {
        for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
            const implied_book::order& a = book.orders()[slot];
            const oracle::order& b = legacy.raw(slot);
            price.hit("price", a.price, b.price);
            size.hit("size", a.size, b.size);
            ext_price.hit("ext price", a.external_price, b.external_price);
            ext_size.hit("ext size", a.external_size, b.external_size);
            //  is the right comparison: a shadowed order is live in the new book and
            // absent from the legacy map, which is exactly the D-EX11 emplace rule.
            in_book.hit("matchable", a.matchable() ? 1 : 0, b.matchable() ? 1 : 0);
        }
    }

    void aggregates(const implied_book& book, const oracle::ime& legacy) {
        for (side sd : {side::bid, side::ask}) {
            const implied_book::external_snapshot a = book.external(sd);
            ext_top.hit("price", a.price, legacy.external_top_price(sd));
            ext_top.hit("size", a.size, legacy.external_top_size(sd));
            persisted.hit("price", book.persisted_price(sd), legacy.persisted_price(sd));
        }
        for (strategy_id subscriber : {strategy_id::RV, strategy_id::AS}) {
            for (side sd : {side::bid, side::ask}) {
                const internal_top a = book.best_for(subscriber, sd);
                const internal_top b = legacy.best_for(subscriber, sd);
                published.hit("price", a.price, b.price);
                published.hit("size", a.size, b.size);
                published.hit("source", static_cast<int>(a.source), static_cast<int>(b.source));
            }
        }
    }

    void fills_of(const fill_buffer& a, const oracle::match_fills& b) {
        // Legacy pushes a zero-size fill record when a dead map entry is reached (:929-938); the
        // engine erases the entry without inventing a trade, so those records are not comparable
        // (D-EX17). Everything else must match exactly.
        uint32_t legacy_count = 0;
        for (const auto& item : b.items) legacy_count += item.size > 0 ? 1u : 0u;
        fills.hit("count", a.count, static_cast<long long>(legacy_count));
        const uint32_t n = std::min<uint32_t>(a.count, static_cast<uint32_t>(b.items.size()));
        uint32_t j = 0;
        for (uint32_t i = 0; i < n; ++i) {
            while (j < b.items.size() && b.items[j].size == 0) ++j;
            if (j >= b.items.size()) break;
            diff_one(a.items[i], b.items[j]);
            ++j;
        }
    }

    void diff_one(const internal_fill& fa, const oracle::fill_record& fb) {
        {
            const internal_fill& a = fa;
            const oracle::fill_record& b = fb;
            fills.hit("strategy", static_cast<int>(a.strategy), static_cast<int>(b.strategy));
            fills.hit("kind", static_cast<int>(a.kind), static_cast<int>(b.kind));
            fills.hit("side", static_cast<int>(a.sd), static_cast<int>(b.sd));
            fills.hit("price", a.price, b.price);
            fills.hit("size", a.size, b.size);
        }
    }

    uint64_t total() const {
        return price.mismatches + size.mismatches + ext_price.mismatches + ext_size.mismatches +
               in_book.mismatches + ext_top.mismatches + persisted.mismatches +
               published.mismatches + fills.mismatches + cross_flags.mismatches +
               aggress.mismatches;
    }

    void report() const {
        price.rate();
        size.rate();
        ext_price.rate();
        ext_size.rate();
        in_book.rate();
        ext_top.rate();
        persisted.rate();
        published.rate();
        fills.rate();
        cross_flags.rate();
        aggress.rate();
    }
};

// The request tape both implementations see.
struct request_gen {
    std::mt19937 rng;
    explicit request_gen(uint32_t seed) : rng(seed) {}

    uint32_t below(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>(rng() % n); }
    bool chance(uint32_t percent) { return below(100) < percent; }

    order_request next(int64_t centre_ticks) {
        const auto strategy = static_cast<strategy_id>(below(STRATEGY_COUNT));
        const auto side_value = static_cast<side>(below(SIDE_COUNT));
        // Only the resting streams take part (the FAK ones are driven through `aggress`).
        const request_kind kind =
            traits_of(strategy, request_kind::hedge).rests && chance(15) ? request_kind::hedge
                                                                         : request_kind::quote;

        int64_t ticks = centre_ticks + static_cast<int64_t>(below(5)) - 2;
        if (chance(8)) {
            // A removal: no price, no size.
            return kind == request_kind::hedge
                       ? make_hedge(strategy, CONTRACT, side_value, NULL_PRICE, NULL_PRICE, 0, true,
                                    false)
                       : make_quote(strategy, CONTRACT, side_value, NULL_PRICE, 0, NULL_PRICE, 0,
                                    false);
        }
        if (chance(20)) ticks = centre_ticks;  // force a collision between strategies

        const qty_t size = 1 + static_cast<qty_t>(below(5));
        const price_t internal = px(ticks);
        const price_t external = chance(25) ? px(ticks + (chance(50) ? 1 : -1)) : internal;
        const qty_t external_size = chance(25) ? size + static_cast<qty_t>(below(3)) : size;

        if (kind == request_kind::hedge) {
            return make_hedge(strategy, CONTRACT, side_value, internal, external, size,
                              /*external=*/true, /*pay_up=*/chance(20));
        }
        return make_quote(strategy, CONTRACT, side_value, internal, size, external, external_size,
                          /*persist=*/chance(30),
                          /*lean_on_rv=*/strategy == strategy_id::AS && chance(20));
    }
};

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// The random differential: the same tape through the new book and the legacy transcription.
// ---------------------------------------------------------------------------
// The trace window: when set (> 0), every step up to it prints both books, so a divergence can be
// watched as it forms instead of guessed at. Used as `exec_match_tests <steps> <seed> <trace_until>`.
int32_t g_trace_until = -1;

void trace_step(uint32_t step, const implied_book& book, const oracle::ime& legacy,
                uint32_t first_request, const std::vector<std::array<long long, 7>>& history) {
    std::printf("--- step %u\n", step);
    for (size_t i = first_request; i < history.size(); ++i) {
        const auto& r = history[i];
        std::printf("    apply strat %lld kind %lld side %lld  size %lld @ %lld  ext %lld @ %lld\n",
                    r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
    }
    for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
        const auto& a = book.orders()[slot];
        const auto& b = legacy.raw(slot);
        if (!a.in_internal_map() && !b.in_book && a.price == NULL_PRICE) continue;
        std::printf(
            "    slot %2u s%u k%u %c: engine %lld@%lld ext %lld@%lld %s%s%s | legacy %lld@%lld "
            "ext %lld@%lld %s\n",
            slot, a.strategy, a.kind, a.side_value() == side::bid ? 'B' : 'A', (long long)a.size,
            (long long)a.price, (long long)a.external_size, (long long)a.external_price,
            a.in_internal_map() ? "inmap" : "-", a.live ? "" : " dead", a.shadowed ? " shadow" : "",
            (long long)b.size, (long long)b.price, (long long)b.external_size,
            (long long)b.external_price, b.in_book ? "inmap" : "-");
    }
    legacy.dump_levels("legacy");
    const implied_book::external_snapshot eb = book.external(side::bid);
    const implied_book::external_snapshot ea = book.external(side::ask);
    std::printf("    engine external: bid %lld@%lld  ask %lld@%lld\n", (long long)eb.size,
                (long long)eb.price, (long long)ea.size, (long long)ea.price);
}

void test_random_tape(uint32_t steps, uint32_t seed) {
    std::printf("differential: %u random steps (seed %u)\n", steps, seed);

    // `first_wins` is the legacy emplace rule (D-EX11), so the tapes must agree exactly.
    implied_book book(implied_book::price_collision::first_wins);
    book.init(implied_book::price_collision::first_wins, CONTRACT);
    oracle::ime legacy(CONTRACT);
    comparator cmp;
    request_gen gen(seed);

    fill_buffer engine_fills;
    oracle::match_fills legacy_fills;
    uint64_t crosses = 0;
    uint64_t aggressions = 0;
    uint64_t aborts = 0;
    uint64_t divergent_steps = 0;
    uint64_t mask_breaks = 0;
    uint64_t level_breaks = 0;
    bool dumped = false;
    // The last few requests, so the first-divergence report shows what built the state being diffed.
    std::vector<std::array<long long, 7>> request_log;
    std::vector<std::array<long long, 7>> full_history;

    for (uint32_t step = 0; step < steps; ++step) {
        g_step = static_cast<int>(step);
        const uint64_t before_step = cmp.total();
        // 1. one to four order events on a narrow price band.
        const int64_t centre = 100 + static_cast<int64_t>(gen.below(3));
        const uint32_t events = 1 + gen.below(4);
        const uint32_t step_first_request = static_cast<uint32_t>(full_history.size());
        for (uint32_t i = 0; i < events; ++i) {
            const order_request request = gen.next(centre);
            request_log.push_back({(long long)request.strategy_id_value(),
                                   (long long)request.kind_value(), (long long)request.side_value(),
                                   request.kind_value() == request_kind::hedge
                                       ? request.hedge_size
                                       : request.internal_size,
                                   request.internal_price, request.external_size,
                                   request.external_price});
            if (request_log.size() > 8) request_log.erase(request_log.begin());
            full_history.push_back(request_log.back());
            book.apply(request);
            legacy.modify(request);
            legacy.validate("after modify");
        }
        cmp.slots(book, legacy);
        cmp.aggregates(book, legacy);
        if (!book.level_indices_consistent()) {
            ++level_breaks;
            if (level_breaks <= 3) {
                std::printf("  FAIL  cached level index drifted at step %u\n", step);
            }
        }
        if (!book.mask_consistent()) {
            ++mask_breaks;
            if (mask_breaks <= 3) std::printf("  FAIL  membership mask drifted at step %u\n", step);
        }
        if (g_trace_until >= 0 && step <= static_cast<uint32_t>(g_trace_until)) {
            trace_step(step, book, legacy, step_first_request, full_history);
        }
        if (cmp.total() != 0 && !dumped) {
            dumped = true;
            legacy.dump("legacy");
            std::printf("  engine  in-map:");
            for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
                const auto& o = book.orders()[slot];
                if (!o.in_internal_map()) continue;
                std::printf(" [%lld strat%u k%u %lld]", (long long)o.price, o.strategy,
                            (unsigned)o.kind, (long long)o.size);
            }
            std::printf("\n");
            std::printf("FIRST DIVERGENCE at step %u after %u applies (centre %lld)\n", step, events,
                        (long long)centre);
            // The tape up to the divergence, which is what makes a minimal replay possible. Capped so
            // a long run cannot flood the log.
            const size_t history_from = full_history.size() > 96 ? full_history.size() - 96 : 0;
            std::printf("  history to the divergence (%zu requests, showing the last %zu):\n",
                        full_history.size(), full_history.size() - history_from);
            for (size_t i = history_from; i < full_history.size(); ++i) {
                const auto& r = full_history[i];
                std::printf("    { %lld, %lld, %lld, %lld, %lld, %lld, %lld },\n", r[0], r[1], r[2],
                            r[3], r[4], r[5], r[6]);
            }
            for (const auto& r : request_log) {
                std::printf("    req: strat %lld kind %lld side %lld  size %lld @ %lld  ext %lld @ %lld\n",
                            r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
            }
            for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
                const auto& a = book.orders()[slot];
                const auto& b = legacy.raw(slot);
                if (a.price == NULL_PRICE && b.price == NULL_PRICE) continue;
                std::printf(
                    "  slot %2u strat %u kind %u side %u: engine %lld@%lld ext %lld@%lld %s%s\n",
                    slot, a.strategy, a.kind, a.sd, (long long)a.size, (long long)a.price,
                    (long long)a.external_size, (long long)a.external_price,
                    a.live ? "live" : "-", a.shadowed ? " shadowed" : "");
                std::printf("                             legacy %lld@%lld ext %lld@%lld %s%s\n",
                            (long long)b.size, (long long)b.price, (long long)b.external_size,
                            (long long)b.external_price, b.in_book ? "inbook" : "-", "");
            }
        }

        // 2. the cross, aggressor = a random strategy.
        const auto aggressor = static_cast<strategy_id>(gen.below(STRATEGY_COUNT));
        engine_fills.clear();
        legacy_fills.clear();
        const cross_result a = book.cross(aggressor, engine_fills);
        const oracle::match_result b = legacy.match(aggressor, legacy_fills);
        legacy.validate("after cross");
        ++crosses;

        if (!a.ok || !b.ok) {
            // A self match aborts the cross in both implementations. The legacy fault state is not
            // reproduced on purpose (D-EX16): it keeps zero-size orders in the price map, so it can
            // later publish a price with no size and let a dead entry block an insert. Both books
            // are therefore re-synchronised to empty and the tape continues.
            ++aborts;
            cmp.cross_flags.hit("abort", a.ok ? 0 : 1, b.ok ? 0 : 1);
            book.clear();
            legacy = oracle::ime(CONTRACT);
            continue;
        }

        cmp.cross_flags.hit("ok", 1, 1);
        cmp.cross_flags.hit("as_self", a.as_self_match ? 1 : 0, b.as_self_match ? 1 : 0);
        cmp.cross_flags.hit("as_bid", a.as_matched_bid, b.as_matched_bid);
        cmp.cross_flags.hit("as_ask", a.as_matched_ask, b.as_matched_ask);
        cmp.fills_of(engine_fills, legacy_fills);
        cmp.slots(book, legacy);
        cmp.aggregates(book, legacy);

        // 3. a FAK aggression from the opposite side, most steps.
        if (gen.chance(60)) {
            const auto sd = static_cast<side>(gen.below(SIDE_COUNT));
            const auto strategy = static_cast<strategy_id>(gen.below(STRATEGY_COUNT));
            const price_t limit = px(centre + static_cast<int64_t>(gen.below(7)) - 3);
            const qty_t size = 1 + static_cast<qty_t>(gen.below(6));

            engine_fills.clear();
            legacy_fills.clear();
            const implied_book::aggressive_result x =
                book.aggress(sd, strategy, limit, size, engine_fills);
            const oracle::ime::aggress_result y =
                legacy.try_match(sd, strategy, limit, size, legacy_fills);
            ++aggressions;
            cmp.aggress.hit("filled", x.filled, y.filled);
            cmp.aggress.hit("remaining", x.remaining, y.remaining);
            cmp.aggress.hit("vwap", x.vwap, y.vwap);
            cmp.aggress.hit("self matches", x.self_matches, y.self_matches);
            cmp.aggress.hit("as self", x.as_self_match ? 1 : 0, y.as_self_match ? 1 : 0);
            cmp.fills_of(engine_fills, legacy_fills);
            cmp.slots(book, legacy);
            cmp.aggregates(book, legacy);
        }

        if (cmp.total() != before_step) ++divergent_steps;

        // 4. occasionally take a strategy out, the way a cancel-all does.
        if (gen.chance(5)) {
            const auto strategy = static_cast<strategy_id>(gen.below(STRATEGY_COUNT));
            book.remove_strategy(strategy);
            legacy.remove_strategy(strategy);
            cmp.slots(book, legacy);
            cmp.aggregates(book, legacy);
        }
    }

    std::printf("  steps with a divergence: %llu of %u\n", (unsigned long long)divergent_steps,
                steps);
    std::printf("  crosses=%llu (aborts %llu), aggressions=%llu\n", (unsigned long long)crosses,
                (unsigned long long)aborts, (unsigned long long)aggressions);
    cmp.report();
    check_eq(static_cast<long long>(cmp.total()), 0, "no observable differs from the legacy engine");
    check_eq(static_cast<long long>(mask_breaks), 0, "the membership mask never drifted");
    check_eq(static_cast<long long>(level_breaks), 0, "the cached level indices never drifted");
    check(crosses > 0 && aggressions > 0, "the tape exercises both match paths");
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// The intended deviation (D-EX11), asserted from both sides.
// ---------------------------------------------------------------------------
void test_collision_policy_is_the_only_deviation() {
    std::printf("differential: the price-collision rule\n");

    // Two strategies bid at the same price; a third asks through it.
    const order_request mm_bid =
        make_quote(strategy_id::MM, CONTRACT, side::bid, px(100), 5, px(100), 5, false);
    const order_request rv_bid =
        make_quote(strategy_id::RV, CONTRACT, side::bid, px(100), 4, px(100), 4, false);
    const order_request as_ask =
        make_quote(strategy_id::AS, CONTRACT, side::ask, px(99), 9, px(99), 9, false);

    // Legacy semantics: the second bid at the price is not in the map, so only 5 can trade.
    implied_book legacy_like(implied_book::price_collision::first_wins);
    legacy_like.init(implied_book::price_collision::first_wins, CONTRACT);
    legacy_like.apply(mm_bid);
    legacy_like.apply(rv_bid);
    legacy_like.apply(as_ask);
    fill_buffer a;
    legacy_like.cross(strategy_id::AS, a);

    oracle::ime oracle_like(CONTRACT);
    oracle_like.modify(mm_bid);
    oracle_like.modify(rv_bid);
    oracle_like.modify(as_ask);
    oracle::match_fills b;
    oracle_like.match(strategy_id::AS, b);

    check_eq(a.count, static_cast<long long>(b.items.size()),
             "first_wins fills exactly what legacy fills");
    check_eq(a.items[0].size, 5, "only the first order at the price trades");
    check_eq(legacy_like.orders()[to_int(strategy_id::RV) * KIND_COUNT * SIDE_COUNT].live, 1,
             "the second order at the price is still resting in the new book");
    check_eq(oracle_like.find(strategy_id::RV, request_kind::quote, side::bid)->in_book, 0,
             "and was never in the legacy map");

    // Shipped semantics: both are matchable, so the whole 9 trades.
    implied_book shipped(implied_book::price_collision::all_matchable);
    shipped.init(implied_book::price_collision::all_matchable, CONTRACT);
    shipped.apply(mm_bid);
    shipped.apply(rv_bid);
    shipped.apply(as_ask);
    fill_buffer c;
    shipped.cross(strategy_id::AS, c);
    qty_t filled = 0;
    for (uint32_t i = 0; i < c.count; ++i) {
        if (c.items[i].strategy == strategy_id::AS) filled += c.items[i].size;
    }
    check_eq(filled, 9, "all_matchable trades through both orders of the price (the fix)");
    std::printf("  note: the deviation is exactly D-EX11 (%d vs %d lots on the same tape)\n", 5,
                static_cast<int>(filled));
}

// ---------------------------------------------------------------------------
// A hand-built case for the external-leg continuation, the subtlest branch.
// ---------------------------------------------------------------------------
void test_external_leg_continuation_matches() {
    std::printf("differential: external-leg continuation\n");

    // RV bids 10 at 101 (aggressor); MM offers 3 internally at 100 with 5 externally at 99.
    const order_request rv_bid =
        make_quote(strategy_id::RV, CONTRACT, side::bid, px(101), 10, px(101), 10, false);
    const order_request mm_ask =
        make_quote(strategy_id::MM, CONTRACT, side::ask, px(100), 3, px(99), 5, false);

    implied_book book(implied_book::price_collision::first_wins);
    book.init(implied_book::price_collision::first_wins, CONTRACT);
    oracle::ime legacy(CONTRACT);
    book.apply(rv_bid);
    legacy.modify(rv_bid);
    book.apply(mm_ask);
    legacy.modify(mm_ask);

    fill_buffer a;
    oracle::match_fills b;
    book.cross(strategy_id::RV, a);
    legacy.match(strategy_id::RV, b);

    comparator cmp;
    cmp.fills_of(a, b);
    cmp.slots(book, legacy);
    cmp.aggregates(book, legacy);
    check_eq(cmp.total(), 0, "the continuation branch agrees with the transcription");
    check_eq(a.count, 4, "two fills, each reported to both orders");
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t steps = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 20000;
    const uint32_t seed = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 42;
    g_trace_until = argc > 3 ? static_cast<int32_t>(std::atoi(argv[3])) : -1;

    test_collision_policy_is_the_only_deviation();
    test_external_leg_continuation_matches();
    test_random_tape(steps, seed);

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-1b checks passed\n");
    return EXIT_SUCCESS;
}
