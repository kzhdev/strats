// P3 gate: the incremental path update.
//
//   as_incremental_tests [steps] [seed]
//
// Two independent questions are asked:
//
//   1. does the incremental update agree with a transcription of legacy
//      Path::onBestMarketDataUpdate (:833-928), Path::updatePaths (:931-985) and
//      ShortTermFairOffset::calculateShortTermFairOffset (:199-243) at *every* step of a
//      random stream of market-data, fair-value and cycle events?
//   2. does the delta accumulation stay arithmetically exact -- i.e. does a forced full
//      recompute reproduce the incremental price bit for bit? (The size is *not* expected to
//      match: legacy's size rule is deliberately asymmetric, PLAN §4.3.)

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "config.hpp"
#include "model.hpp"
#include "path_oracle.hpp"
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

// Real config plus the synthetic switch-path model, so both the plain and the offset branches
// are exercised.
model real_model() {
    const as_model_cfg cfg = as_model_cfg::load("config/minimal_cme/AS_model.csv",
                                                "config/minimal_cme/AS_paths.csv",
                                                "config/minimal_cme/secdb.cme.csv");
    return model::build(build_model_inputs(cfg));
}

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

struct counters {
    uint64_t compared = 0;
    uint64_t mismatched = 0;
    double rate() const { return compared ? static_cast<double>(mismatched) / compared : 0.0; }
    void add(bool same) {
        ++compared;
        if (!same) ++mismatched;
    }
};

}  // namespace
namespace {

// ---------------------------------------------------------------------------
// A random event stream driven through the engine and the legacy transcription.
// ---------------------------------------------------------------------------
struct harness {
    model m;
    cycle_state s;
    quote_policy policy{};
    path_engine engine{m, policy};
    std::vector<oracle::path> mirror;
    std::vector<uint32_t> mirror_order;
    std::mt19937 rng;
    std::vector<uint8_t> pre_recalc;
    int step = 0;
    const char* last_op = "none";
    bool diagnosed = false;

    harness(model&& model_in, unsigned seed)
        : m(std::move(model_in)),
          s(m),
          mirror(static_cast<size_t>(m.n_path_slots)),
          mirror_order(static_cast<size_t>(m.n_path_slots)),
          rng(seed),
          pre_recalc(static_cast<size_t>(m.n_path_slots)) {
        for (uint32_t i = 0; i < m.n_path_slots; ++i) mirror_order[i] = m.side_path_slot[i];
    }

    int pick(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

    // One market-data update: the book moves, then the best-book delta is applied when the
    // update involved the best book (legacy's `changed` predicate).
    void market_update() {
        const uint32_t c = static_cast<uint32_t>(pick(0, static_cast<int>(m.n_all) - 1));
        const side sd = pick(0, 1) == 0 ? side::bid : side::ask;
        const bool external = pick(0, 1) == 0;
        const bool no_price = pick(0, 9) == 0;
        const price_t price = no_price ? NULL_PRICE : m.tick[c] * pick(1, 4000);
        const qty_t size = no_price ? 0 : pick(0, 12);

        const price_t old_price = s.best_price_adjusted(m, c, sd);
        if (external) {
            s.update_external(policy, c, sd, price, size);
        } else {
            s.update_internal(c, sd, price, size);
        }
        if (!s.refresh_best(m, policy, c, sd, external)) return;
        if (sd == side::bid) {
            engine.on_best_market_update<side::bid>(s, c, old_price);
            oracle::on_best_market_update<side::bid>(m, s, policy, mirror, mirror_order, c,
                                                    old_price);
        } else {
            engine.on_best_market_update<side::ask>(s, c, old_price);
            oracle::on_best_market_update<side::ask>(m, s, policy, mirror, mirror_order, c,
                                                    old_price);
        }
    }


    // A fair value arrives for one contract; every offset that depends on it is recomputed
    // (legacy handleSTFUpdate -> ShortTermFairOffset::calculateShortTermFairOffset). The
    // previous value is captured first: the engine writes the new one into the shared state,
    // and the oracle must start from the same "before".
    void fair_value_update() {
        const uint32_t c = static_cast<uint32_t>(pick(0, static_cast<int>(m.n_all) - 1));
        s.set_stf(c, pick(0, 6) == 0 ? NULL_PRICE : m.tick[0] * pick(-50, 50));
        for (uint32_t o = 0; o < m.n_offsets; ++o) {
            bool affected = false;
            for (uint32_t k = m.offset_term_begin[o]; k < m.offset_term_begin[o + 1]; ++k) {
                if (m.offset_term_contract[k] == c) affected = true;
            }
            if (!affected) continue;
            const price_t previous = s.offset_value(o);
            engine.update_offset(s, o);
            oracle::update_offset(m, s, mirror, o, previous);
        }
    }

    // A cycle for one quoting contract (legacy runCycle -> updateQuotes -> updatePaths).
    void run_cycle() {
        const uint32_t c = static_cast<uint32_t>(pick(0, static_cast<int>(m.n_all) - 1));
        if (!m.is_quoting(c)) return;
        engine.update_paths(s, c, false);
        oracle::update_paths<side::bid>(m, s, policy, mirror, mirror_order, c, false);
        oracle::update_paths<side::ask>(m, s, policy, mirror, mirror_order, c, false);
    }

    // A forced recalculation (legacy force on enable, parameter or fair-value reset).
    void force_recalc() {
        const uint32_t c = static_cast<uint32_t>(pick(0, static_cast<int>(m.n_all) - 1));
        if (!m.is_quoting(c)) return;
        engine.update_paths(s, c, true);
        oracle::update_paths<side::bid>(m, s, policy, mirror, mirror_order, c, true);
        oracle::update_paths<side::ask>(m, s, policy, mirror, mirror_order, c, true);
    }

    // Compare every path field, the pending flags and the whole ranking.
    void compare(counters& fields, counters& flags, counters& order, counters& validity) {
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (!m.is_quoting(c)) continue;
            for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
                const side sd = static_cast<side>(si);
                const uint32_t key = c * SIDE_COUNT + si;
                const uint32_t count = m.side_path_begin[key + 1] - m.side_path_begin[key];
                for (uint32_t rank = 0; rank < count; ++rank) {
                    const uint32_t slot = s.order_at(m, c, sd, rank);
                    const path_state& got = s.path(slot);
                    const oracle::path& want = mirror[slot];
                    fields.add(got.price == want.pq.price && got.size == want.pq.size &&
                               got.size_determiner == want.size_determiner &&
                               got.lean_on_rv == want.lean_on_rv);
                    const bool flags_same = got.recalc == want.recalc &&
                                            got.resort == want.resort && got.dirty == want.dirty;
                    if (!flags_same && !diagnosed) {
                        diagnosed = true;
                        std::printf("    DIAG step=%d op=%s slot=%u recalc %d(pre %d)/%d "
                                    "resort %d/%d dirty %d/%d valid %d/%d price %lld/%lld\n",
                                    step, last_op, slot, int(got.recalc),
                                    int(pre_recalc[slot]), int(want.recalc), int(got.resort),
                                    int(want.resort), int(got.dirty), int(want.dirty),
                                    int(got.valid()), int(want.pq.valid()),
                                    (long long)got.price, (long long)want.pq.price);
                    }
                    flags.add(flags_same);
                    order.add(mirror_order[m.side_path_begin[key] + rank] == slot);
                    validity.add(got.valid() == want.pq.valid());
                }
            }
        }
    }
};

void test_stream(int steps, unsigned seed, model&& model_in, const char* label) {
    std::printf("incremental stream: %s (%d steps, seed %u)\n", label, steps, seed);
    harness h(std::move(model_in), seed);
    counters fields;
    counters flags;
    counters order;
    counters validity;

    for (int i = 0; i < steps; ++i) {
        h.step = i;
        for (uint32_t slot = 0; slot < h.m.n_path_slots; ++slot) {
            h.pre_recalc[slot] = h.s.path(slot).recalc ? 1 : 0;
        }
        const int r = h.pick(0, 99);
        if (r < 60) {
            h.last_op = "market";
            h.market_update();
        } else if (r < 70) {
            h.last_op = "fair";
            h.fair_value_update();
        } else if (r < 95) {
            h.last_op = "cycle";
            h.run_cycle();
        } else {
            h.last_op = "force";
            h.force_recalc();
        }
        h.compare(fields, flags, order, validity);
        h.s.clear_dirty();
    }

    std::printf("  path fields : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - fields.rate()),
                static_cast<unsigned long long>(fields.mismatched),
                static_cast<unsigned long long>(fields.compared));
    std::printf("  path flags  : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - flags.rate()),
                static_cast<unsigned long long>(flags.mismatched),
                static_cast<unsigned long long>(flags.compared));
    std::printf("  path order  : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - order.rate()),
                static_cast<unsigned long long>(order.mismatched),
                static_cast<unsigned long long>(order.compared));
    std::printf("  validity    : %.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - validity.rate()),
                static_cast<unsigned long long>(validity.mismatched),
                static_cast<unsigned long long>(validity.compared));
    check_eq(static_cast<long long>(fields.mismatched), 0,
             std::string("path fields match the legacy transcription: ") + label);
    check_eq(static_cast<long long>(flags.mismatched), 0,
             std::string("pending flags match the legacy transcription: ") + label);
    check_eq(static_cast<long long>(order.mismatched), 0,
             std::string("ranking matches the legacy transcription: ") + label);
    check_eq(static_cast<long long>(validity.mismatched), 0,
             std::string("validity matches the legacy transcription: ") + label);
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// The arithmetic invariant.
//
// For a path whose every lean has a *positive* multiplier (so the lean side equals the path
// side), or whose leans charge no external commission, the incremental price must equal a fresh
// recompute bit for bit: the delta series and the closed form are the same integer sum.
//
// For a negative-multiplier lean on a commission-charging contract they *cannot* agree, and that
// is legacy behaviour: Path::calculate applies the external commission with the **path's** side
// (:760), while the incremental update prices the lean with the **lean's** side
// (getExtCommissionAdjustedBestPrice<Side>, :1126). Every external/internal transition therefore
// shifts the accumulated price by 2*commission*|multiplier| relative to a fresh calculation
// (PARITY_NOTES D-AS7). The engine reproduces legacy here; what this gate asserts is
//
//   * exact equality where it must hold, and
//   * the derived bound everywhere else, so a new drift cannot hide behind the known one.
// ---------------------------------------------------------------------------
bool exact_by_construction(const model& m, uint32_t d) {
    for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
        if (m.lean_mult[k] < 0 && m.ext_commission[m.lean_contract[k]] != 0) return false;
    }
    return true;
}

price_t drift_bound(const model& m, uint32_t d) {
    price_t bound = 0;
    for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
        bound += 2 * std::abs(static_cast<price_t>(m.lean_mult[k])) *
                 m.ext_commission[m.lean_contract[k]];
    }
    return bound;
}

template <side S>
price_t closed_form_price(const model& m, const cycle_state& s, uint32_t d) {
    price_t price = 0;
    for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
        const uint32_t lean = m.lean_contract[k];
        const side lean_side = m.lean_mult[k] > 0 ? S : other(S);
        const bool external = s.best_is_external(lean, lean_side);
        const market_state& book = s.book(lean, lean_side, external);
        if (book.price == NULL_PRICE) return NULL_PRICE;
        price_t market_price = book.price;
        if (external) {
            market_price = apply_price_offset<S>(market_price, -m.ext_commission[lean]);
        }
        price += static_cast<price_t>(m.lean_mult[k]) * market_price;
    }
    const uint32_t offset = m.path_offset[d];
    if (offset != INVALID_INDEX) {
        if (!s.has_offset_value(offset)) return NULL_PRICE;
        price += s.offset_value(offset);
    }
    return apply_price_offset<S>(price, -m.path_commission[d]);
}

// Cross-check the incremental price against the closed form at every step, so a drift is caught
// at the event that caused it. Exact paths must match exactly; the others must stay inside the
// derived bound.
void check_closed_form(const model& m, const cycle_state& s, int step, const char* op,
                       bool& reported) {
    for (uint32_t slot = 0; slot < m.n_path_slots; ++slot) {
        const path_state& p = s.path(slot);
        if (!p.valid()) continue;
        const uint32_t d = m.slot_path[slot];
        const price_t expected = m.slot_side[slot] == to_index(side::bid)
                                     ? closed_form_price<side::bid>(m, s, d)
                                     : closed_form_price<side::ask>(m, s, d);
        if (expected == NULL_PRICE) continue;
        const price_t diff = p.price - expected;
        const bool ok = exact_by_construction(m, d) ? diff == 0 : std::abs(diff) <= drift_bound(m, d);
        if (ok) continue;
        if (reported) return;
        reported = true;
        std::printf("    DIAG step=%d op=%s slot=%u contract=%u path=%u: incremental %lld "
                    "closed form %lld diff %lld bound %lld exact %d\n",
                    step, op, slot, m.slot_contract[slot], d, (long long)p.price,
                    (long long)expected, (long long)diff, (long long)drift_bound(m, d),
                    int(exact_by_construction(m, d)));
    }
}

void test_price_invariant(int steps, unsigned seed, model&& model_in, const char* label) {
    std::printf("price invariant: %s (%d steps, seed %u)\n", label, steps, seed);
    harness h(std::move(model_in), seed);

    // warm up so paths have prices to accumulate deltas onto
    for (uint32_t c = 0; c < h.m.n_all; ++c) {
        if (!h.m.is_quoting(c)) continue;
        for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
            const side sd = static_cast<side>(si);
            h.s.update_internal(c, sd, h.m.tick[c] * 2400, 10);
            h.s.refresh_best(h.m, h.policy, c, sd, false);
        }
    }

    // incremental stream: no cycles, no forced recalcs, only deltas and fair values
    bool drift_reported = false;
    for (int step = 0; step < steps; ++step) {
        h.step = step;
        if (h.pick(0, 9) == 0) {
            h.last_op = "fair";
            h.fair_value_update();
        } else {
            h.last_op = "market";
            h.market_update();
        }
        h.s.clear_dirty();
        for (uint32_t c = 0; c < h.m.n_all; ++c) {
            if (!h.m.is_quoting(c)) continue;
            for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
                // keep prices where a delta can land: a path that dropped out is re-priced
                // by the cycle, which this test deliberately does not run
                for (uint32_t rank = 0; rank < h.s.order_size(h.m, c, static_cast<side>(si));
                     ++rank) {
                    path_state& p =
                        h.s.path(h.s.order_at(h.m, c, static_cast<side>(si), rank));
                    if (!p.valid()) p.recalc = true;
                }
            }
            h.engine.update_paths(h.s, c, false);
        }
        check_closed_form(h.m, h.s, step, h.last_op, drift_reported);
    }

    // snapshot the incremental prices, then force a full recompute and compare
    std::vector<price_t> incremental(static_cast<size_t>(h.m.n_path_slots));
    std::vector<qty_t> incremental_size(static_cast<size_t>(h.m.n_path_slots));
    std::vector<uint8_t> incremental_valid(static_cast<size_t>(h.m.n_path_slots));
    for (uint32_t slot = 0; slot < h.m.n_path_slots; ++slot) {
        incremental[slot] = h.s.path(slot).price;
        incremental_size[slot] = h.s.path(slot).size;
        incremental_valid[slot] = h.s.path(slot).valid() ? 1 : 0;
    }

    for (uint32_t c = 0; c < h.m.n_all; ++c) {
        if (!h.m.is_quoting(c)) continue;
        h.engine.update_paths(h.s, c, true);
    }

    counters price_same;
    counters size_same;
    counters exact_checked;
    counters within_bound;
    for (uint32_t slot = 0; slot < h.m.n_path_slots; ++slot) {
        const path_state& p = h.s.path(slot);
        const bool valid_now = p.valid() != 0;
        if (incremental_valid[slot] != (valid_now ? 1 : 0)) continue;  // validity is gated above
        if (!valid_now) continue;
        const uint32_t d = h.m.slot_path[slot];
        const price_t diff = p.price - incremental[slot];
        size_same.add(p.size == incremental_size[slot]);
        if (exact_by_construction(h.m, d)) {
            exact_checked.add(diff == 0);
        } else {
            within_bound.add(std::abs(diff) <= drift_bound(h.m, d));
        }
        price_same.add(diff == 0);
    }
    std::printf("  paths that must match exactly      : %.5f%% (%llu of %llu)\n",
                100.0 * (1.0 - exact_checked.rate()),
                static_cast<unsigned long long>(exact_checked.compared -
                                                exact_checked.mismatched),
                static_cast<unsigned long long>(exact_checked.compared));
    std::printf("  negative-multiplier paths in bound : %.5f%% (%llu of %llu)\n",
                100.0 * (1.0 - within_bound.rate()),
                static_cast<unsigned long long>(within_bound.compared -
                                                within_bound.mismatched),
                static_cast<unsigned long long>(within_bound.compared));
    std::printf("  size  after full recompute: %.5f%% equal (the difference is the legacy\n"
                "                              asymmetry, PLAN 4.3, not a defect)\n",
                100.0 * (1.0 - size_same.rate()));
    check_eq(static_cast<long long>(exact_checked.mismatched), 0,
             std::string("positive-multiplier paths match the recompute exactly: ") + label);
    check_eq(static_cast<long long>(within_bound.mismatched), 0,
             std::string("negative-multiplier paths stay inside the derived bound: ") + label);
    check(price_same.compared > 0,
          std::string("the invariant saw at least one priced path: ") + label);
}

}  // namespace

int main(int argc, char** argv) {
    const int steps = argc > 1 ? std::atoi(argv[1]) : 2000;
    const unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 42u;

    test_stream(steps, seed, real_model(), "real config");
    test_stream(steps, seed + 1, switch_model(), "switch paths");
    test_price_invariant(steps, seed + 2, real_model(), "real config");
    test_price_invariant(steps, seed + 3, switch_model(), "switch paths");

    if (g_failures == 0) {
        std::printf("\nall AS phase-3 checks passed\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}

