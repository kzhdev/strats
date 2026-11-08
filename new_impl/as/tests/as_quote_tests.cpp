// P4 gate: the quote stage -- updateQuote<Side>, the external-size crossing clamp, the
// maxTicksThroughMarket bound and the emission change detection -- against the independent
// transcription in tests/quote_oracle.hpp.
//
//   as_quote_tests [iterations] [seed]

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "config.hpp"
#include "model.hpp"
#include "paths.hpp"
#include "policy.hpp"
#include "quote_oracle.hpp"
#include "quotes.hpp"
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

model real_model() {
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    return model::build(build_model_inputs(cfg));
}

// ---------------------------------------------------------------------------
// 1. the quote arithmetic on a hand-driven path
// ---------------------------------------------------------------------------
void test_arithmetic() {
    std::printf("quote arithmetic (edge, commission recovery, tick rounding)\n");
    model m = real_model();
    cycle_state s(m);
    const quote_policy policy{};
    path_engine paths(m, policy);
    quote_engine quotes(m, paths, policy);

    const uint32_t c = 6;  // QMQ2-QMV2, edge $0.0015, quote size 7
    // path 6 is "QV": one lean on the quoted contract, commission $0.01
    s.update_internal(c, side::bid, px(60.00), 10);
    s.update_external(policy, c, side::bid, px(59.75), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    s.update_internal(c, side::ask, px(60.25), 10);
    s.update_external(policy, c, side::ask, px(60.50), 10);
    s.refresh_best(m, policy, c, side::ask, false);
    paths.update_paths(s, c, true);

    quote_gates gates;
    gates.enabled = true;
    gates.can_quote_external = true;
    const quote_result r = quotes.build(s, c, false, gates);

    const quote& bid = s.quote_of(c, side::bid);
    const quote& ask = s.quote_of(c, side::ask);
    check(r.changed(), "the first build emits both sides");
    check_eq(bid.internal_price, px(59.99) - m.edge[c],
             "bid internal = path - edge ($59.99 - $0.0015)");
    check_eq(bid.lean_price, px(59.99) + m.path_commission[6],
             "bid lean price recovers the un-adjusted basket");
    check_eq(bid.internal_size, 7, "internal size is the path size (quote size cap)");
    check_eq(bid.external_price, round_to_tick(bid.internal_price, m.tick_div[c], side::bid),
             "external price is the internal price rounded to the tick");
    check_eq(ask.internal_price, px(60.26) + m.edge[c],
             "ask internal = path + edge ($60.26 + $0.0015)");
    check_eq(ask.external_price, round_to_tick(ask.internal_price, m.tick_div[c], side::ask),
             "ask external price ceils to the tick");

    // a second build with nothing changed must not emit
    const quote_result again = quotes.build(s, c, false, gates);
    check(!again.changed(), "an unchanged market does not re-emit");

    // force and the per-side force flag both bypass change detection
    check(quotes.build(s, c, true, gates).changed(), "force re-emits");
    s.set_force_update(c, side::ask, true);
    const quote_result forced = quotes.build(s, c, false, gates);
    check(forced.ask == quote_status::emit, "the ask force flag re-emits the ask");
    check(!s.force_update(c, side::ask), "the force flag is consumed");

    // a disabled strategy maintains the quote fields but emits nothing
    s.set_force_update(c, side::bid, true);
    gates.enabled = false;
    const quote_result off = quotes.build(s, c, false, gates);
    check(!off.changed(), "a disabled strategy emits nothing");
    check_eq(s.quote_of(c, side::bid).internal_price, bid.internal_price,
             "the quote fields are still maintained while disabled");

    // external quoting off clears the external side
    gates.enabled = true;
    gates.can_quote_external = false;
    s.set_force_update(c, side::bid, true);
    quotes.build(s, c, false, gates);
    check_eq(s.quote_of(c, side::bid).external_price, NULL_PRICE,
             "no external quote when external quoting is off");
    check_eq(s.quote_of(c, side::bid).external_size, 0, "and no external size");
}

}  // namespace
namespace {

// ---------------------------------------------------------------------------
// 2. the external-size crossing clamp and the maxTicks bound
// ---------------------------------------------------------------------------
void test_crossing_and_bound() {
    std::printf("crossing clamp + maxTicksThroughMarket\n");
    model m = real_model();
    cycle_state s(m);
    const quote_policy policy{};
    path_engine paths(m, policy);
    quote_engine quotes(m, paths, policy);

    const uint32_t c = 6;  // QMQ2-QMV2
    s.update_internal(c, side::bid, px(60.00), 10);
    s.update_external(policy, c, side::bid, px(59.75), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    // a cheap external ask, so the rounded bid will cross into it
    s.update_external(policy, c, side::ask, px(59.90), 3);
    s.refresh_best(m, policy, c, side::ask, true);
    s.update_internal(c, side::ask, px(60.25), 10);
    s.refresh_best(m, policy, c, side::ask, false);
    paths.update_paths(s, c, true);

    quote_gates gates;
    gates.enabled = true;
    gates.can_quote_external = true;
    s.set_force_update(c, side::bid, true);
    quotes.build(s, c, false, gates);

    const quote& bid = s.quote_of(c, side::bid);
    check(quotes.is_aggressive<side::bid>(s, c, bid.external_price),
          "the rounded bid crosses the external ask");
    check_eq(bid.external_size, 3, "a crossing quote takes only the opposite book's size");

    // a deeper opposite book: the clamp is then bounded by our own internal size
    s.update_external(policy, c, side::ask, px(59.90), 9);
    s.refresh_best(m, policy, c, side::ask, true);
    s.set_force_update(c, side::bid, true);
    quotes.build(s, c, false, gates);
    check_eq(s.quote_of(c, side::bid).external_size, 7,
             "the clamp is bounded by our own internal size (7)");

    // push the opposite ask away: no crossing, so the full internal size is used
    s.update_external(policy, c, side::ask, px(61.00), 9);
    s.refresh_best(m, policy, c, side::ask, true);
    s.set_force_update(c, side::bid, true);
    quotes.build(s, c, false, gates);
    check(!quotes.is_aggressive<side::bid>(s, c, s.quote_of(c, side::bid).external_price),
          "the quote no longer crosses");
    check_eq(s.quote_of(c, side::bid).external_size, 7, "no clamp: the internal size is used");

    // maxTicksThroughMarket is 100 ticks = $2.5 for this contract
    check_eq(m.max_ticks_through_market[c], px(2.5), "the contract's bound is $2.5");
    quote q = s.quote_of(c, side::bid);
    const price_t opposite_best = s.best_book(c, side::ask).price;
    q.external_price = opposite_best + m.max_ticks_through_market[c];
    check(!quotes.within_max_ticks<side::bid>(s, c, q),
          "ticks through == max is a violation (strict comparison)");
    q.external_price = opposite_best + m.max_ticks_through_market[c] - 1;
    check(quotes.within_max_ticks<side::bid>(s, c, q), "one unit inside the bound is valid");
    quote null_q;
    check(quotes.within_max_ticks<side::bid>(s, c, null_q),
          "a quote without an external price passes");
}


// ---------------------------------------------------------------------------
// 3. the no-path branch (a cancellation)
// ---------------------------------------------------------------------------
void test_cancellation() {
    std::printf("cancellation (no usable path, quote outstanding)\n");
    model m = real_model();
    cycle_state s(m);
    const quote_policy policy{};
    path_engine paths(m, policy);
    quote_engine quotes(m, paths, policy);

    const uint32_t c = 6;
    s.update_internal(c, side::bid, px(60.00), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);

    quote_gates gates;
    gates.enabled = true;
    gates.can_quote_external = true;
    check(quotes.build(s, c, false, gates).bid == quote_status::emit, "a live quote is emitted");

    // the lean disappears: no usable path, so the outstanding quote is withdrawn
    s.update_internal(c, side::bid, NULL_PRICE, 0);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);
    const quote_result r = quotes.build(s, c, false, gates);
    check(r.bid == quote_status::emit, "the outstanding quote is cancelled");
    check_eq(s.quote_of(c, side::bid).internal_price, NULL_PRICE, "the stored quote is reset");

    check(!quotes.build(s, c, false, gates).changed(), "a second build emits nothing");

    // a disabled strategy does not cancel either (legacy returns before the send)
    s.update_internal(c, side::bid, px(60.00), 10);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);
    quotes.build(s, c, false, gates);
    s.update_internal(c, side::bid, NULL_PRICE, 0);
    s.refresh_best(m, policy, c, side::bid, false);
    paths.update_paths(s, c, true);
    gates.enabled = false;
    check(!quotes.build(s, c, false, gates).changed(), "a disabled strategy emits no cancellation");
}

// ---------------------------------------------------------------------------
// 4. randomised differential against the legacy transcription
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

struct mirrors {
    std::vector<quote> quote_;
    std::vector<quote> last_sent;
    std::vector<uint8_t> force;

    explicit mirrors(uint32_t contracts)
        : quote_(static_cast<size_t>(contracts) * SIDE_COUNT),
          last_sent(static_cast<size_t>(contracts) * SIDE_COUNT),
          force(static_cast<size_t>(contracts) * SIDE_COUNT, 0) {}

    uint32_t key(uint32_t c, side sd) const { return c * SIDE_COUNT + to_index(sd); }
};

struct counters3 {
    counters fields;
    counters status;
    counters sent;
    counters flags;
};


template <side S>
void differential_side(const model& m, cycle_state& s, const quote_policy& policy,
                       const quote_engine& quotes, uint32_t c, bool force,
                       const quote_gates& gates, mirrors& mirror, counters3& cnt) {
    const uint32_t k = mirror.key(c, S);
    mirror.force[k] = s.force_update(c, S) ? 1 : 0;

    const quote_status got =
        quotes.build_side<S>(s, c, force, gates.can_quote_external, gates.enabled);

    bool oracle_force = mirror.force[k] != 0;
    const oracle::quote_result want =
        oracle::build_side<S>(m, s, policy, c, mirror.quote_[k], mirror.last_sent[k],
                              oracle_force, force, gates.can_quote_external, gates.enabled);
    mirror.quote_[k] = want.q;
    mirror.force[k] = oracle_force ? 1 : 0;

    const quote& now = s.quote_of(c, S);
    cnt.fields.add(now.lean_price == want.q.lean_price &&
                   now.internal_price == want.q.internal_price &&
                   now.internal_size == want.q.internal_size &&
                   now.external_price == want.q.external_price &&
                   now.external_size == want.q.external_size &&
                   now.hedge_path == want.q.hedge_path);
    cnt.status.add(static_cast<uint8_t>(got) == static_cast<uint8_t>(want.status));

    const quote& ls = s.last_sent(c, S);
    const quote& ls_want = mirror.last_sent[k];
    cnt.sent.add(ls.internal_price == ls_want.internal_price &&
                 ls.internal_size == ls_want.internal_size &&
                 ls.external_price == ls_want.external_price &&
                 ls.external_size == ls_want.external_size &&
                 ls.hedge_path == ls_want.hedge_path);
    cnt.flags.add(s.force_update(c, S) == (mirror.force[k] != 0));
}

void test_differential(int iterations, unsigned seed) {
    std::printf("differential vs the legacy transcription (%d book states, seed %u)\n",
                iterations, seed);
    model m = real_model();
    cycle_state s(m);
    const quote_policy policy{};
    path_engine paths(m, policy);
    quote_engine quotes(m, paths, policy);
    mirrors mirror(m.n_all);

    std::mt19937 rng(seed);
    auto pick = [&rng](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };
    counters3 cnt;

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
            }
            if (m.is_quoting(c)) paths.update_paths(s, c, true);
        }

        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (!m.is_quoting(c)) continue;
            quote_gates gates;
            gates.enabled = pick(0, 5) != 0;
            gates.can_quote_external = pick(0, 1) == 0;
            const bool force = pick(0, 9) == 0;
            s.set_force_update(c, side::bid, pick(0, 4) == 0);
            s.set_force_update(c, side::ask, pick(0, 4) == 0);
            differential_side<side::bid>(m, s, policy, quotes, c, force, gates, mirror, cnt);
            differential_side<side::ask>(m, s, policy, quotes, c, force, gates, mirror, cnt);
        }
    }

    std::printf("  quote fields : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - cnt.fields.rate()),
                static_cast<unsigned long long>(cnt.fields.mismatched),
                static_cast<unsigned long long>(cnt.fields.compared));
    std::printf("  status       : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - cnt.status.rate()),
                static_cast<unsigned long long>(cnt.status.mismatched),
                static_cast<unsigned long long>(cnt.status.compared));
    std::printf("  last sent    : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - cnt.sent.rate()),
                static_cast<unsigned long long>(cnt.sent.mismatched),
                static_cast<unsigned long long>(cnt.sent.compared));
    std::printf("  force flags  : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - cnt.flags.rate()),
                static_cast<unsigned long long>(cnt.flags.mismatched),
                static_cast<unsigned long long>(cnt.flags.compared));
    check_eq(static_cast<long long>(cnt.fields.mismatched), 0,
             "every quote field matches the legacy transcription");
    check_eq(static_cast<long long>(cnt.status.mismatched), 0,
             "the emit/violation decision matches the legacy transcription");
    check_eq(static_cast<long long>(cnt.sent.mismatched), 0,
             "the last-sent record matches the legacy transcription");
    check_eq(static_cast<long long>(cnt.flags.mismatched), 0,
             "force-flag consumption matches the legacy transcription");
}

}  // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 2000;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 42u;

    test_arithmetic();
    test_crossing_and_bound();
    test_cancellation();
    test_differential(iterations, seed);

    if (g_failures == 0) {
        std::printf("\nall AS phase-4 checks passed\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}

