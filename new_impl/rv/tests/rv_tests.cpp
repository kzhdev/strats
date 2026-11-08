// Standalone test binary for the new RV engine (no test framework, matching the
// style of build/new_impl_test.cpp).
//
//   rv_tests [cycles]
//
// Covers: exact tick division, model init/validation (D3/V1), the differential
// gate against the reference oracle (D1: >= 99.99% identical, all diffs counted),
// and the hedge decision path.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "config.hpp"
#include "ledger_bridge.hpp"
#include "model.hpp"
#include "state.hpp"
#include "valuation.hpp"
#include "quotes.hpp"
#include "strategy.hpp"
#include "tests/reference_oracle.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade;
using namespace tachyon::trade::rv;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

struct counters {
    uint64_t total = 0;
    uint64_t mismatched = 0;
    void add(bool same) {
        ++total;
        if (!same) ++mismatched;
    }
    double rate() const { return total ? static_cast<double>(mismatched) / total : 0.0; }
};

// ---------------------------------------------------------------------------
// 0. price scale invariants (PRICE_SCALE = 1e9)
// ---------------------------------------------------------------------------
void test_price_scale() {
    std::printf("price scale (%lld = 1e9)\n", (long long)PRICE_SCALE);
    check(PRICE_SCALE == 1'000'000'000, "PRICE_SCALE is 1e9");
    check(to_rhone_price(1.0) == PRICE_SCALE, "to_rhone_price($1) == PRICE_SCALE");
    check(to_rhone_price(0.000000001) == 1, "1e-9 resolution");
    check(to_dollars(PRICE_SCALE) == 1.0, "to_dollars round trip");
    check(to_scaled(0.025) == 25'000'000, "to_scaled(0.025) == 2.5e7");
    check(to_scaled(60.0) == 60'000'000'000, "to_scaled($60) == 6e10");
    // truncation, not rounding (S1): $1.9999999999 -> 1_999_999_999, not 2e9
    check(to_rhone_price(1.9999999999) == 1'999'999'999, "to_rhone_price truncates");
    // the tick divider must still be exact at the new scale
    const fast_div fd = make_fast_div(to_scaled(0.025));
    check(round_to_tick(to_scaled(60.0) + 1, fd, side::bid) == to_scaled(60.0),
          "round_tick floors at 1e-9 scale");
    check(round_to_tick(to_scaled(60.0) + 1, fd, side::ask) == to_scaled(60.025),
          "round_tick ceils at 1e-9 scale");
}

// ---------------------------------------------------------------------------
// 1. exact tick division (replaces the per-lean int64 `%`)
// ---------------------------------------------------------------------------
void test_fast_div() {
    std::printf("fast_div / round_to_tick\n");
    const int64_t divisors[] = {1, 2, 8, 1024, 1'000'000, 2'500'000, 25'000'000,
                                3'000'000, 7, 12'500'000};
    uint64_t checked = 0;
    bool ok = true;
    for (int64_t d : divisors) {
        const fast_div fd = make_fast_div(d);
        for (int64_t x = -2'000'003; x <= 2'000'003; ++x) {
            int64_t q = 0, r = 0;
            floor_divmod(x, fd, q, r);
            const int64_t eq = x / d;
            const int64_t er = x % d;
            if (r != (er < 0 ? er + d : er) || q * d + r != x || r < 0 || r >= d) {
                ok = false;
                if (g_failures < 5) std::printf("  floor_divmod(%lld,%lld)\n", (long long)x, (long long)d);
            }
            divmod_trunc(x, fd, q, r);
            if (q != eq || r != er) {
                ok = false;
                if (g_failures < 5) std::printf("  divmod_trunc(%lld,%lld)\n", (long long)x, (long long)d);
            }
            ++checked;
        }
        // boundaries around multiples, both signs, plus the fast-path limit
        for (int64_t base = -40; base <= 40; ++base) {
            for (int64_t off = -3; off <= 3; ++off) {
                const int64_t x = base * d + off;
                int64_t q = 0, r = 0;
                divmod_trunc(x, fd, q, r);
                if (q != x / d || r != x % d) ok = false;
                ++checked;
            }
        }
        for (int64_t x : {FAST_DIV_LIMIT - 1, FAST_DIV_LIMIT, FAST_DIV_LIMIT + 1, -FAST_DIV_LIMIT,
                          -FAST_DIV_LIMIT - 1, (int64_t)1 << 62, -((int64_t)1 << 62)}) {
            int64_t q = 0, r = 0;
            divmod_trunc(x, fd, q, r);
            if (q != x / d || r != x % d) ok = false;
            ++checked;
        }
    }
    check(ok, "divmod matches hardware division for every divisor");
    std::printf("  %llu division cases checked\n", (unsigned long long)checked);

    // round_to_tick must equal the legacy transcription (S2), negatives included
    bool tick_ok = true;
    for (int64_t d : divisors) {
        const fast_div fd = make_fast_div(d);
        for (int64_t x = -5 * d - 20; x <= 5 * d + 20; ++x) {
            for (side sd : {side::bid, side::ask}) {
                if (round_to_tick(x, fd, sd) != oracle::round_to_tick_legacy(x, d, sd)) {
                    tick_ok = false;
                }
            }
        }
    }
    check(tick_ok, "round_to_tick == legacy RoundToTickSize");
}

// ---------------------------------------------------------------------------
// test configuration: 4 vectors / 4 pricing contracts / 10 contracts
// (the shape of config/backtest/model.csv, CME QMQ1..QMX1)
// ---------------------------------------------------------------------------
struct fixture {
    model_inputs in;
    std::vector<leg_input> legs;

    static constexpr uint32_t N_PRICING = 4;
    static constexpr uint32_t N_ALL = 10;
    static constexpr price_t TICK = to_scaled(0.25);  // QM 0.25 tick

    fixture() {
        in.n_pricing = N_PRICING;
        in.n_vectors = N_PRICING;
        in.n_contracts = N_ALL;
        in.first_traded = N_PRICING;
        const size_t nn = static_cast<size_t>(N_ALL) * N_ALL;

        // factor loadings: 4 vectors over 4 outrights (invertible, mildly coupled)
        const double pricing[N_PRICING][N_PRICING] = {
            {1.0, 0.02, -0.02, 0.0},
            {0.02, 1.0, 0.02, -0.02},
            {-0.02, 0.02, 1.0, 0.02},
            {0.0, -0.02, 0.02, 1.0}};
        in.pricing_matrix.assign(N_PRICING * N_PRICING, 0.0);
        for (uint32_t r = 0; r < N_PRICING; ++r)
            for (uint32_t c = 0; c < N_PRICING; ++c)
                in.pricing_matrix[r * N_PRICING + c] = pricing[r][c];

        // spreads: legs (0,1), (1,2) with +/-1, and (2,3) with ratio 2
        for (uint32_t c = 0; c < N_PRICING; ++c) {
            in.contracts.resize(N_ALL);
            in.contracts[c].leg_begin = 0;  // outrights carry no legs
            in.contracts[c].leg_count = 0;
        }
        for (uint32_t s = 0; s < N_ALL - N_PRICING; ++s) {
            const uint32_t begin = static_cast<uint32_t>(legs.size());
            if (s == 0) {
                legs.push_back({0, 1.0});
                legs.push_back({1, -1.0});
            } else if (s == 1) {
                legs.push_back({1, 1.0});
                legs.push_back({2, -1.0});
            } else {
                legs.push_back({2, 1.0});
                legs.push_back({3, -2.0});
            }
            const uint32_t c = N_PRICING + s;
            in.contracts[c].leg_begin = begin;
            in.contracts[c].leg_count = 2;
        }
        in.legs = legs;
        in.contracts.resize(N_ALL);

        in.hedge_ratio.assign(nn, 1.0);
        in.quote_size.assign(nn, 7.0);
        in.commissions.assign(nn, 34'000);  // 0.00034
        in.leans.assign(nn, 1);
        in.factor_hedged_index.assign(nn, 0);
        in.rhone_max_clip.assign(N_ALL, 100);

        for (uint32_t c = 0; c < N_ALL; ++c) {
            contract_input& ci = in.contracts[c];
            ci.tick_size = TICK;
            ci.width = to_scaled(0.008);  // RVWidth
            ci.extra_width = 0;
            ci.price_change_threshold = 0;
            ci.established_level = 1000;
            ci.lot_size = 1;
            ci.weight = c < N_PRICING ? 1.0 : 0.5;
            // Every contract is quoted here *on purpose*: the differential then also covers
            // the priced+quoted overlap (PLAN §12) on the four pricing columns, whose
            // all-ones lean matrix makes them lean on the spreads as well.
            ci.traded = true;
            ci.quote_external = (c % 2) == 0;
            ci.weighted_mid = (c % 3) == 0;
            ci.static_quote = false;
            ci.static_quote_size = 0;
        }
        // a pair that must not be quotable (negative hedge ratio -> folded out of CSR)
        in.hedge_ratio[0 * N_ALL + 5] = -1.0;
        // different quote sizes
        for (uint32_t c = 0; c < N_ALL; ++c) {
            for (uint32_t j = 0; j < N_ALL; ++j) {
                in.quote_size[c * N_ALL + j] = (j % 2 == 0) ? 7.0 : 50.0;
            }
        }

        for (uint32_t v = 0; v < N_PRICING; ++v) {
            vector_input vi;
            vi.range = 0.3 + 0.1 * v;
            vi.max_position = 1000.0;
            vi.initial_ltf = 0.0;
            vi.ema_period_cycles = 0;
            vi.use_ema = (v == 1);
            vi.set_to_market = (v == 2);
            vi.factor_hedged = true;
            in.vectors.push_back(vi);
        }
        in.vectors[1].ema_period_cycles = 1'000'000;
        in.vectors[0].factor_hedged = false;
    }
};

// ---------------------------------------------------------------------------
// 1b. boundary of the reciprocal fast path: still exact at the 1e9 scale?
// ---------------------------------------------------------------------------
void test_fast_div_limit() {
    std::printf("fast_div near the fast-path limit (2^52 = %.2e units = $%.2g)\n",
                static_cast<double>(FAST_DIV_LIMIT),
                static_cast<double>(FAST_DIV_LIMIT) / static_cast<double>(PRICE_SCALE));
    const int64_t divisors[] = {to_scaled(0.025), to_scaled(0.25), to_scaled(0.01),
                                to_scaled(0.005), 1, 3, 7};
    const int64_t bases[] = {FAST_DIV_LIMIT - 100'000, FAST_DIV_LIMIT, FAST_DIV_LIMIT + 100'000,
                             -FAST_DIV_LIMIT - 100'000, -FAST_DIV_LIMIT, -FAST_DIV_LIMIT + 100'000,
                             int64_t(1) << 53, -(int64_t(1) << 53)};
    bool ok = true;
    uint64_t cases = 0;
    for (const int64_t d : divisors) {
        const fast_div fd = make_fast_div(d);
        for (const int64_t base : bases) {
            for (int64_t off = -3'000; off <= 3'000; off += 13) {
                const int64_t x = base + off;
                int64_t q = 0, r = 0;
                divmod_trunc(x, fd, q, r);
                if (q != x / d || r != x % d) ok = false;
                for (const side sd : {side::bid, side::ask}) {
                    if (round_to_tick(x, fd, sd) != oracle::round_to_tick_legacy(x, d, sd)) {
                        ok = false;
                    }
                }
                ++cases;
            }
        }
    }
    check(ok, "fast path and hardware fallback agree around +/-2^52 and +/-2^53");
    // practical headroom: no futures price is anywhere near the threshold
    check(to_scaled(1'000'000.0) < FAST_DIV_LIMIT,
          "$1m price is still inside the fast path at the 1e9 scale");
    check(to_scaled(60.0) * 1000 < FAST_DIV_LIMIT, "$60 price has >3 orders of headroom");
    std::printf("  %llu boundary cases checked\n", (unsigned long long)cases);
}

// ---------------------------------------------------------------------------
// 1c. JPX-shaped magnitudes at the 1e9 scale (large price, coarse and fine ticks)
// ---------------------------------------------------------------------------
void test_jpx_magnitude() {
    std::printf("large and small magnitude instruments at 1e9\n");
    struct price_case {
        const char* name;
        double price;   // index points / yen / USD-per-100JPY
        double tick;    // price points
    } cases[] = {{"Nikkei225 45,000 tick 10", 45'000.0, 10.0},
                 {"Nikkei mini 45,000 tick 5", 45'000.0, 5.0},
                 {"TOPIX 2,800 tick 0.5", 2'800.0, 0.5},
                 {"JGB 10y 140 tick 0.01", 140.0, 0.01},
                 // CME currency futures: the finest ticks in the repo (secdb
                 // config/minimal_cme/secdb.cme.all.csv, 86 rows of 6J...)
                 {"CME 6J yen 0.0070 tick 5e-7", 0.0070, 0.0000005},
                 {"CME 6J calendar 0.0000050 tick 1e-7", 0.0000050, 0.0000001}};

    for (const price_case& c : cases) {
        const price_t px = to_scaled(c.price);
        const price_t tick = to_scaled(c.tick);
        const fast_div fd = make_fast_div(tick);

        // 1. well inside the reciprocal fast path, with headroom to spare
        const bool in_fast_path = px < FAST_DIV_LIMIT;
        check(in_fast_path, std::string(c.name) + ": price inside the fast path");
        check(tick > 0 && px % tick == 0, std::string(c.name) + ": tick divides the price exactly");

        // 2. the divider is exact at this magnitude (mid + 1 unit rounds correctly)
        int64_t q = 0, r = 0;
        const price_t probe = px + 1;
        divmod_trunc(probe, fd, q, r);
        check(q == probe / tick && r == probe % tick,
              std::string(c.name) + ": divmod exact at magnitude");
        check(round_to_tick(probe, fd, side::bid) == px,
              std::string(c.name) + ": bid floors a 1-unit overshoot");
        check(round_to_tick(probe, fd, side::ask) == px + tick,
              std::string(c.name) + ": ask ceils a 1-unit overshoot");

        // 3. matches the legacy transcription across a tick neighbourhood
        bool same = true;
        for (int64_t off = -3; off <= 3; ++off) {
            for (const side sd : {side::bid, side::ask}) {
                same = same && round_to_tick(px + off * tick / 4, fd, sd) ==
                                   oracle::round_to_tick_legacy(px + off * tick / 4, tick, sd);
            }
        }
        check(same, std::string(c.name) + ": round_to_tick == legacy across a neighbourhood");

        // 4. int64 headroom for the widest pipeline product (32-leg spread at this price)
        const price_t widest = to_scaled(c.price * 32.0);
        check(widest > 0 && widest < FAST_DIV_LIMIT * 1000,
              std::string(c.name) + ": 32-leg sum stays far from int64 overflow");
        std::printf("  %-38s price=%15lld tick=%12lld (%3.0e x headroom)\n", c.name,
                    (long long)px, (long long)tick,
                    static_cast<double>(FAST_DIV_LIMIT) / static_cast<double>(px));
    }

    // The finest ticks in the repo are 1e-7 (CME 6J calendars). 1e-7 is exact on both
    // grids, but 1e9 also represents ticks *below* 1e-8 — which 1e-8 cannot:
    //   tick 5e-9 -> 5 units at 1e9, but 0.5 -> rounds to 0 at 1e8 (init failure).
    check(to_scaled(0.0000001) == 100, "1e-7 tick is 100 units at 1e9");
    check(to_scaled(0.000000001) == 1, "1e-9 tick is representable");
    check(to_scaled(0.000000005) == 5, "5e-9 tick is representable at 1e9");
    check(static_cast<int64_t>(0.000000005 * 1e8) == 0,
          "5e-9 tick would truncate to 0 on the legacy 1e-8 grid (would fail init)");
    // FX prices sit ~640 million x below the fast-path limit
    check(to_scaled(0.007) * 1000 < FAST_DIV_LIMIT, "6J price has huge headroom");
}

// ---------------------------------------------------------------------------
// 2. model init + validation (D3 square-only, V1 singular, V3 caps)
// ---------------------------------------------------------------------------
void test_model_init() {
    std::printf("model init / validation\n");
    fixture f;
    const model m = model::build(f.in);

    // risk is the inverse-transpose: sum_k risk[c][k] * P[k][r] == delta(c, r)
    double worst = 0.0;
    for (uint32_t c = 0; c < m.n_pricing; ++c) {
        for (uint32_t r = 0; r < m.n_pricing; ++r) {
            double sum = 0.0;
            for (uint32_t k = 0; k < m.n_pricing; ++k) {
                sum += m.risk[static_cast<size_t>(c) * m.n_vectors + k] *
                       f.in.pricing_matrix[static_cast<size_t>(k) * m.n_pricing + r];
            }
            worst = std::max(worst, std::fabs(sum - (c == r ? 1.0 : 0.0)));
        }
    }
    check(worst < 1e-12, "risk * pricing == identity (inverse-transpose orientation)");

    // ODM reproduces the outright prices from the outright mids
    {
        oracle::oracle_model om(f.in);
        double odm_worst = 0.0;
        for (uint32_t p = 0; p < m.n_pricing; ++p) {
            for (uint32_t c = 0; c < m.n_all; ++c) {
                odm_worst = std::max(odm_worst, std::fabs(m.odm[p * m.n_all + c] - om.odm[p][c]));
            }
        }
        check(odm_worst < 1e-12, "ODM matches the oracle reconstruction");
    }

    // CSR folds out the negative hedge ratio
    for (uint32_t k = m.lean_begin[0]; k < m.lean_begin[1]; ++k) {
        check(m.lean_index[k] != 5, "negative hedge ratio excluded from CSR leans");
    }
    check(m.lean_begin[m.n_all] == m.lean_index.size(), "CSR terminator");

    // ODM must be the weighted least-squares operator for weights w (legacy forms
    // A^T W^T W A => w enters squared). Property test: for arbitrary mid, the residual
    // r = A*p - mid must be W^2-orthogonal to the columns of A. This pins the
    // convention without reusing the code that builds ODM.
    {
        const uint32_t np = m.n_pricing;
        std::vector<double> a(static_cast<size_t>(m.n_all) * np, 0.0);
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (f.in.contracts[c].leg_count > 1) {
                for (uint32_t k = 0; k < f.in.contracts[c].leg_count; ++k) {
                    const leg_input& leg = f.in.legs[f.in.contracts[c].leg_begin + k];
                    a[static_cast<size_t>(c) * np + leg.index] += leg.ratio;
                }
            } else {
                a[static_cast<size_t>(c) * np + c] = 1.0;
            }
        }
        std::vector<double> mid(m.n_all);
        for (uint32_t c = 0; c < m.n_all; ++c) {
            mid[c] = 3.7 + 0.13 * static_cast<double>(c) + 0.001 * static_cast<double>(c % 3);
        }
        double worst_normal = 0.0;
        for (uint32_t p = 0; p < np; ++p) {  // column p of A
            double dot = 0.0;
            for (uint32_t c = 0; c < m.n_all; ++c) {
                double predicted = 0.0;
                for (uint32_t q = 0; q < np; ++q) {
                    predicted += a[static_cast<size_t>(c) * np + q] * [&] {
                        double s = 0.0;
                        for (uint32_t k = 0; k < m.n_all; ++k) {
                            s += m.odm[static_cast<size_t>(q) * m.n_all + k] * mid[k];
                        }
                        return s;
                    }();
                }
                const double w = f.in.contracts[c].weight;
                dot += a[static_cast<size_t>(c) * np + p] * (w * w) * (predicted - mid[c]);
            }
            worst_normal = std::max(worst_normal, std::fabs(dot));
        }
        check(worst_normal < 1e-9, "ODM satisfies the weighted normal equations (w squared)");
    }

    // non-square and singular configs must fail at init
    {
        model_inputs bad = f.in;
        bad.n_pricing = 3;  // vectors != pricing
        bad.pricing_matrix.resize(bad.n_vectors * bad.n_pricing);
        bool threw = false;
        try {
            model::build(bad);
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "non-square pricing matrix rejected (D3)");
    }
    {
        model_inputs bad = f.in;
        // rows 2 and 3 become copies of row 0 -> rank 2 of 4
        for (uint32_t col = 0; col < bad.n_pricing; ++col) {
            bad.pricing_matrix[2 * bad.n_pricing + col] = bad.pricing_matrix[col];
            bad.pricing_matrix[3 * bad.n_pricing + col] = bad.pricing_matrix[col];
        }
        bool threw = false;
        try {
            model::build(bad);
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "singular pricing matrix rejected (V1)");
    }
}

// ---------------------------------------------------------------------------
// 3. differential test against the reference oracle (D1 gate)
// ---------------------------------------------------------------------------
struct rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ull;
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
    int64_t span(int64_t lo, int64_t hi) {
        return lo + static_cast<int64_t>(next() % static_cast<uint64_t>(hi - lo + 1));
    }
};

// legacy calculateMidpoint, written independently of the engine
double legacy_mid(const fixture& f, const cycle_state& s, uint32_t c) {
    const market_state& bid = s.markets[market_index(c, side::bid, true)];
    const market_state& ask = s.markets[market_index(c, side::ask, true)];
    if (f.in.contracts[c].weighted_mid) {
        const qty_t est = static_cast<qty_t>(f.in.contracts[c].established_level);
        const qty_t as = ask.size < est ? ask.size : est;
        const qty_t bs = bid.size < est ? bid.size : est;
        const double denom = static_cast<double>(bs + as);
        if (denom == 0.0) return 0.0;
        return (to_dollars(ask.price) * static_cast<double>(bs) +
                to_dollars(bid.price) * static_cast<double>(as)) / denom;
    }
    return (to_dollars(bid.price) + to_dollars(ask.price)) / 2.0;
}

// legacy BetterMarket: ties go to the internal market
bool oracle_is_external(const cycle_state& s, uint32_t j, side sd) {
    const market_state& in_ = s.markets[market_index(j, sd, false)];
    const market_state& ex = s.markets[market_index(j, sd, true)];
    if (in_.price == tachyon::NULL_PRICE) return true;
    if (ex.price == tachyon::NULL_PRICE) return false;
    return sd == side::bid ? ex.price > in_.price : ex.price < in_.price;
}

struct flat_books {
    std::vector<price_t> price;
    std::vector<qty_t> size;
};

void flatten(const cycle_state& s, flat_books& out) {
    const uint32_t n = MAX_CONTRACTS * SIDE_COUNT * 2;
    out.price.assign(n, tachyon::NULL_PRICE);
    out.size.assign(n, 0);
    for (uint32_t i = 0; i < n; ++i) {
        out.price[i] = s.markets[i].price;
        out.size[i] = s.markets[i].size;
    }
}

void diff_quotes(const fixture& f, const model& m, const cycle_state& s,
                 const oracle::oracle_model& om, const std::vector<price_t>& stf_oracle,
                 const flat_books& books, counters& price_c, counters& size_c, counters& lean_c,
                 counters& ext_price_c);

void test_differential(uint64_t cycles) {
    std::printf("differential vs reference oracle (%llu cycles)\n", (unsigned long long)cycles);
    fixture f;
    const model m = model::build(f.in);
    oracle::oracle_model om(f.in);

    cycle_state s{};
    s.init(m);
    quote_policy policy{};
    order_port port{};
    strategy strat(m, s, policy, port);

    std::vector<double> ltf = m.init_ltf;
    std::vector<double> prior(m.n_vectors, 0.0);
    std::vector<double> pos(m.n_vectors, 0.0);
    std::vector<price_t> stf_oracle;
    flat_books books;
    uint64_t last_time = 0;
    uint64_t now = 0;

    counters mid_c, stf_c, price_c, size_c, lean_c, ext_price_c;
    uint64_t skipped_cycles = 0;
    int stf_mismatch_prints = 0;
    rng random;

    std::vector<price_t> bid(m.n_all), ask(m.n_all);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        bid[c] = to_scaled(4.0) + static_cast<price_t>(c) * fixture::TICK;  // .00, .25, ...
        ask[c] = bid[c] + fixture::TICK;
    }

    // initial books: every pricing contract must have both sides before cycle 0,
    // otherwise the engine (correctly) has no midpoint yet.
    for (uint32_t c = 0; c < m.n_all; ++c) {
        strat.on_market_data(c, side::bid, true, bid[c], 25);
        strat.on_market_data(c, side::ask, true, ask[c], 25);
    }

    for (uint64_t cycle = 0; cycle < cycles; ++cycle) {
        now += 1000;
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (random.below(4) != 0) continue;
            const price_t drift = random.span(-3, 3) * fixture::TICK;
            bid[c] += drift;
            ask[c] += drift;
            if (ask[c] < bid[c]) ask[c] = bid[c] + fixture::TICK;
            const qty_t bid_size = static_cast<qty_t>(random.span(1, 60));
            const qty_t ask_size = static_cast<qty_t>(random.span(1, 60));
            strat.on_market_data(c, side::bid, true,
                                 bid_size == 0 ? tachyon::NULL_PRICE : bid[c], bid_size);
            strat.on_market_data(c, side::ask, true,
                                 ask_size == 0 ? tachyon::NULL_PRICE : ask[c], ask_size);
        }
        // NOTE: missing-side cycles are covered by test_missing_side(); injecting them
        // here would park garbage (~9.2e10) in the stateful EMA and make every later
        // cycle differ by a few ticks at 1e16 magnitude for no useful reason.

        strat.run_cycle(now);

        // A missing side makes the legacy midpoint `to_dollars(NULL_PRICE)` ~ 9.2e10,
        // i.e. garbage that both engines propagate (legacy behaviour, not a difference).
        // Those cycles cannot be compared tick-for-tick, so they are counted separately.
        bool degenerate = false;
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (s.markets[market_index(c, side::bid, true)].price == tachyon::NULL_PRICE ||
                s.markets[market_index(c, side::ask, true)].price == tachyon::NULL_PRICE) {
                degenerate = true;
            }
        }

        std::vector<double> mid(m.n_all, 0.0);
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (m.used_in_pricing[c]) {  // legacy updates mids for every weighted contract
                mid[c] = legacy_mid(f, s, c);
            }
        }
        oracle::fairs(om, mid, pos, ltf, prior, now - last_time, stf_oracle);
        last_time = now;
        if (degenerate) {
            ++skipped_cycles;
            continue;
        }
        for (uint32_t c = 0; c < m.n_all; ++c) {
            mid_c.add(std::fabs(mid[c] - s.mid[c]) < 1e-12);
        }
        for (uint32_t c = 0; c < m.n_all; ++c) {
            const bool same = stf_oracle[c] == s.stf[c];
            stf_c.add(same);
            if (!same && stf_mismatch_prints < 12) {
                ++stf_mismatch_prints;
                std::printf("    stf mismatch cycle=%llu c=%u eng=%lld oracle=%lld (diff %lld)\n",
                            (unsigned long long)cycle, c, (long long)s.stf[c],
                            (long long)stf_oracle[c],
                            (long long)(s.stf[c] - stf_oracle[c]));
            }
        }
        if (cycles <= 5) {
            std::printf("  [dbg cycle %llu]\n", (unsigned long long)cycle);
            for (uint32_t c = 0; c < m.n_all; ++c) {
                std::printf("    c%-2u mid=%.10f (eng %.10f) stf=%lld (oracle %lld)\n", c, mid[c],
                            s.mid[c], (long long)s.stf[c], (long long)stf_oracle[c]);
            }
        }
        flat_books books_local;
        flatten(s, books_local);
        books = std::move(books_local);
        diff_quotes(f, m, s, om, stf_oracle, books, price_c, size_c, lean_c, ext_price_c);
    }

    std::printf("  (%llu degenerate cycles skipped: missing market data -> garbage mids)\n",
                (unsigned long long)skipped_cycles);
    std::printf("  mid          : %8.5f%% identical (%llu/%llu)\n", 100.0 * (1.0 - mid_c.rate()),
                (unsigned long long)(mid_c.total - mid_c.mismatched),
                (unsigned long long)mid_c.total);
    std::printf("  stf          : %8.5f%% identical (%llu mismatched of %llu)\n",
                100.0 * (1.0 - stf_c.rate()), (unsigned long long)stf_c.mismatched,
                (unsigned long long)stf_c.total);
    const counters* rows[] = {&price_c, &size_c, &lean_c, &ext_price_c};
    const char* names[] = {"quote price  ", "quote size   ", "best lean    ", "ext price    "};
    for (int i = 0; i < 4; ++i) {
        std::printf("  %s : %8.5f%% identical (%llu mismatched of %llu)\n", names[i],
                    100.0 * (1.0 - rows[i]->rate()), (unsigned long long)rows[i]->mismatched,
                    (unsigned long long)rows[i]->total);
    }
    check(mid_c.rate() <= 1e-4, "mid parity >= 99.99%");
    check(skipped_cycles == 0, "no degenerate cycles leaked into the differential");
    check(stf_c.rate() <= 1e-4, "stf parity >= 99.99%");
    check(price_c.rate() <= 1e-4, "quote price parity >= 99.99%");
    check(size_c.rate() <= 1e-4, "quote size parity >= 99.99%");
    check(lean_c.rate() <= 1e-4, "best lean parity >= 99.99%");
    check(ext_price_c.rate() <= 1e-4, "external price parity >= 99.99%");
}

// legacy calculateSize (:2990-3004): capped by market/hedge_ratio and the quote size.
inline double legacy_lean_size(const fixture& /*f*/, const model& /*m*/, qty_t market_size,
                               double hedge_ratio, double max_quote_size) {
    if (hedge_ratio > 0) {
        const double possible = static_cast<double>(market_size) / hedge_ratio;
        return possible > max_quote_size ? max_quote_size : possible;
    }
    return max_quote_size;
}

// Compares one cycle of quotes against a full-scan, legacy-ordered oracle.
void diff_quotes(const fixture& f, const model& m, const cycle_state& s,
                 const oracle::oracle_model& om, const std::vector<price_t>& stf_oracle,
                 const flat_books& books, counters& price_c, counters& size_c, counters& lean_c,
                 counters& ext_price_c) {
    for (uint32_t c = 0; c < m.n_all; ++c) {
        if (!m.traded[c]) continue;  // traded is a flag: priced+quoted contracts sit lower
        for (side sd : {side::bid, side::ask}) {
            const quote& q = s.quote_of(c, sd);
            price_t best_price = tachyon::NULL_PRICE;
            uint32_t best_lean = 0;
            qty_t best_size = 0;
            for (uint32_t j = 0; j < m.n_all; ++j) {
                const size_t k = static_cast<size_t>(c) * m.n_all + j;
                if (f.in.leans[k] == 0 || f.in.hedge_ratio[k] < 0.0) continue;
                const bool external = oracle_is_external(s, j, sd);
                const price_t px = oracle::lean_price_legacy(om, stf_oracle, books.price,
                                                             books.size, c, sd, j, external);
                if (px == tachyon::NULL_PRICE) continue;
                const bool better = best_price == tachyon::NULL_PRICE ||
                                    (sd == side::bid ? px > best_price : px < best_price);
                if (!better) continue;
                best_price = px;
                best_lean = j;
                best_size = to_valid_qty(
                    static_cast<qty_t>(static_cast<double>(legacy_lean_size(
                                           f, m, books.size[market_index(j, sd, external)],
                                           f.in.hedge_ratio[k], f.in.quote_size[k])) +
                                       0.0001),
                    m.lot[c]);
            }
            if (best_price == tachyon::NULL_PRICE) {
                price_c.add(q.price == tachyon::NULL_PRICE);
                continue;
            }
            price_c.add(q.price == best_price);
            size_c.add(q.size == best_size);
            lean_c.add(q.lean == best_lean);
            if (!f.in.contracts[c].quote_external) continue;
            price_t club_price = tachyon::NULL_PRICE;
            for (uint32_t j = 0; j < m.n_all; ++j) {
                const size_t k = static_cast<size_t>(c) * m.n_all + j;
                if (f.in.leans[k] == 0 || f.in.hedge_ratio[k] < 0.0) continue;
                const bool external = oracle_is_external(s, j, sd);
                const price_t px = oracle::lean_price_legacy(om, stf_oracle, books.price,
                                                             books.size, c, sd, j, external);
                if (px == tachyon::NULL_PRICE) continue;
                const price_t rounded = oracle::round_to_tick_legacy(px, m.tick[c].divisor, sd);
                if (club_price == tachyon::NULL_PRICE) {
                    club_price = rounded;
                } else if (rounded != club_price) {
                    break;
                }
            }
            ext_price_c.add(q.external_price == club_price);
        }
    }
}

// Missing market data: both engines must agree on the (garbage) midpoint and the
// resulting fair values for that single cycle. Kept separate from the long
// differential because the stateful EMA would remember the garbage.
void test_missing_side() {
    std::printf("missing market data (single cycle)\n");
    fixture f;
    const model m = model::build(f.in);
    oracle::oracle_model om(f.in);
    cycle_state s{};
    s.init(m);
    quote_policy policy{};
    order_port port{};
    strategy strat(m, s, policy, port);

    for (uint32_t c = 0; c < m.n_all; ++c) {
        strat.on_market_data(c, side::bid, true, to_scaled(4.0), 25);
        strat.on_market_data(c, side::ask, true, to_scaled(4.25), 25);
    }
    strat.on_market_data(1, side::bid, true, tachyon::NULL_PRICE, 0);  // bid gone
    strat.run_cycle(1000);

    std::vector<double> mid(m.n_all, 0.0);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        if (!m.used_in_pricing[c]) continue;
        mid[c] = legacy_mid(f, s, c);
        check(std::fabs(mid[c] - s.mid[c]) < 1e-9, "garbage midpoint reproduced identically");
    }
    std::vector<double> ltf = m.init_ltf;
    std::vector<double> prior(m.n_vectors, 0.0);
    std::vector<double> pos(m.n_vectors, 0.0);
    std::vector<price_t> stf_oracle;
    oracle::fairs(om, mid, pos, ltf, prior, 1000, stf_oracle);
    bool all_same = true;
    for (uint32_t c = 0; c < m.n_all; ++c) {
        // With a missing side the midpoint is ~9.2e10 by construction (legacy
        // behaviour), so the fair value is meaningless: compare it relatively,
        // not in ticks (a 1-ulp difference is a few "ticks" at that magnitude).
        const double diff = std::fabs(static_cast<double>(stf_oracle[c] - s.stf[c]));
        const double scale = std::max(1.0, std::fabs(static_cast<double>(stf_oracle[c])));
        all_same = all_same && (diff / scale < 1e-12);
    }
    check(all_same, "fair values match with a missing side (relative)");
}

// ---------------------------------------------------------------------------
// 4. precomputed coefficient tables vs legacy inline divisions (§6.5)
//    The main fixture uses unit multipliers; here they are deliberately awkward.
// ---------------------------------------------------------------------------
void test_hedge_cost_scaling() {
    std::printf("hedge cost scaling: unity fast path + exact legacy fallback\n");
    const double mults[] = {1.0, 10.0, 100.0, 1000.0, 0.01, 0.001, 0.37, 12.5};
    counters scaling;
    uint64_t printed = 0;
    for (double lean_pm : mults) {
        for (double contract_pm : mults) {
            for (double lean_qm : mults) {
                for (double contract_qm : mults) {
                    const bool unity = lean_pm == 1.0 && contract_pm == 1.0 && lean_qm == 1.0 &&
                                       contract_qm == 1.0;
                    for (price_t cost = -2'000'000; cost <= 2'000'000; cost += 7'777) {
                        for (double ratio : {1.0, -1.0, 0.5, 2.0, 0.0}) {
                            const price_t engine =
                                unity ? lean_calculator::cost_unity(cost, ratio, 800'000)
                                      : lean_calculator::cost_exact(cost, ratio, lean_pm,
                                                                    contract_pm, lean_qm,
                                                                    contract_qm, 800'000);
                            // legacy expression order, transcribed from :2978
                            price_t legacy = static_cast<price_t>(
                                static_cast<double>(cost) * ratio * lean_pm / contract_pm *
                                lean_qm / contract_qm);
                            if (legacy < 0) {
                                const price_t max_cost = static_cast<price_t>(0.95 * 800'000.0);
                                if (-legacy > max_cost) legacy = -max_cost;
                            }
                            const bool same = engine == legacy;
                            scaling.add(same);
                            if (!same && printed < 6) {
                                ++printed;
                                std::printf("    diff: cost=%lld ratio=%g unity=%d eng=%lld legacy=%lld\n",
                                            (long long)cost, ratio, (int)unity,
                                            (long long)engine, (long long)legacy);
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("  %8.5f%% identical (%llu mismatched of %llu)\n", 100.0 * (1.0 - scaling.rate()),
                (unsigned long long)scaling.mismatched, (unsigned long long)scaling.total);
    check(scaling.mismatched == 0, "hedge cost path is bit-identical to legacy for all multipliers");
}

// ---------------------------------------------------------------------------
// 5. ledger wiring: reservations shrink lean sizes, hedge emission claims liquidity
// ---------------------------------------------------------------------------
void test_ledger_bridge() {
    std::printf("ledger bridge (reservations -> quote size, hedge -> reservation)\n");
    fixture f;
    // one lean contract with a small displayed size so sizing is market-bound
    const model m = model::build(f.in);
    cycle_state s{};
    s.init(m);

    liquidity_ledger ledger;
    ledger_bridge bridge(ledger, m.n_all);
    // wire the lean contract's books; source = EXTERNAL as these are external markets
    const market_key lean_bid = make_market_key(0, tachyon::om::order_side::BUY, EXTERNAL);
    const market_key lean_ask = make_market_key(0, tachyon::om::order_side::SELL, EXTERNAL);
    bridge.set_market(0, side::bid, true, lean_bid, LIQUIDITY_SOURCE_EXTERNAL);
    bridge.set_market(0, side::ask, true, lean_ask, LIQUIDITY_SOURCE_EXTERNAL);

    quote_policy policy = ledger_bridge::make_policy(bridge);
    order_port port = ledger_bridge::make_port(bridge);
    strategy strat(m, s, policy, port);

    for (uint32_t c = 0; c < m.n_pricing; ++c) {
        strat.on_market_data(c, side::bid, true, to_scaled(4.0), c == 0 ? 5 : 25);
        strat.on_market_data(c, side::ask, true, to_scaled(4.25), 25);
    }
    strat.run_cycle(1000);

    // quote size is min(market/hedge_ratio = 5, quote_size) for the lean that binds
    const qty_t baseline = s.quote_of(4, side::bid).size;
    check(baseline == 5, "quote size limited by the lean market size");

    // reserve 3 lots of hedge liquidity on that lean: available drops to 2
    const reservation_id r = ledger.reserve_hedge(lean_bid, LIQUIDITY_SOURCE_EXTERNAL, 7, 3);
    check(r.value != reservation_id::INVALID, "hedge reservation created");
    check(bridge.available(0, side::bid, true, 5) == 2, "bridge reports 5 - 3 available");

    strat.run_cycle(2000);
    const qty_t after = s.quote_of(4, side::bid).size;
    check(after < baseline, "reservation shrinks the quote size");

    // release and confirm the size comes back
    check(ledger.complete_by_hedge_id(7, lean_bid), "reservation completed");
    strat.run_cycle(3000);
    check(s.quote_of(4, side::bid).size == baseline, "quote size restored after release");

    // a hedge emitted by the engine claims liquidity through the port
    strat.on_fill(4, side::bid, 1);
    strat.run_cycle(4000);
    const uint32_t hedges = strat.hedge_id();
    if (hedges > 0) {
        check(bridge.active_reservations() > 0, "emitted hedge created a reservation");
    } else {
        std::printf("  (no hedge emitted this cycle; reservation path covered directly)\n");
    }
    const reservation_id direct =
        bridge.on_hedge(hedge_order{.contract = 0,
                                   .side_ = side::ask,
                                   .price = to_scaled(4.0),
                                   .size = 2,
                                   .external = true,
                                   .hedge_id = 99,
                                   .market_index = market_index(0, side::bid, true)});
    check(direct.value != reservation_id::INVALID, "bridge.on_hedge reserves through the ledger");
    check(bridge.complete_hedge(99, lean_bid), "bridge.complete_hedge releases it");
}

// ---------------------------------------------------------------------------
// 6. real config: parse config/backtest/model.csv, derive the model, run a cycle
// ---------------------------------------------------------------------------
void test_real_config(const std::string& path) {
    std::printf("real config (%s)\n", path.c_str());
    const rv_model_cfg cfg = rv_model_cfg::load(path);

    // --- parse ---
    std::printf("  parsed: %u vectors, %u pricing, %u traded, %u lean rows, %u pricing cfgs\n",
                (unsigned)cfg.vectors.size(), (unsigned)cfg.pricing_contracts.size(),
                (unsigned)cfg.rv_contracts.size(), (unsigned)cfg.leans.size(),
                (unsigned)cfg.pricing_configs.size());
    check(cfg.vectors.size() == 4, "4 vectors (f1..f4)");
    check(cfg.pricing_contracts.size() == 4, "4 pricing contracts (QMQ1..QMX1)");
    check(cfg.rv_contracts.size() == 6, "6 RV contracts (spreads)");
    check(cfg.leans.size() == 6, "6x6 lean matrix");
    check(cfg.pricing_contracts[0] == "QMQ1" && cfg.pricing_contracts[3] == "QMX1", "pricing header");
    check(cfg.vectors[1].ema && cfg.vectors[1].ema_length_secs == 60, "f2 EMA/60s");
    check(cfg.vectors[1].hedged, "f2 hedged");
    check(cfg.vectors[0].set_to_market && !cfg.vectors[2].set_to_market, "SetToMarket flags");
    check(cfg.pricing_matrix[0][0] == 0.25 && cfg.pricing_matrix[3][0] == -0.25, "pricing matrix");
    check(cfg.rv_contracts[0].name == "QMQ1-QMU1" && cfg.rv_contracts[0].quote_size == 50,
          "first RV contract + quote size");
    check(cfg.width_multiplier() == 2.0, "RVWidthMultiplier US column == 2");

    // --- derive ---
    const secdb_table secdb = load_secdb("config/backtest/secdb.cme.csv");
    check(!secdb.empty(), "secdb ticks loaded");
    const model_inputs in = build_model_inputs(cfg, secdb, cfg.width_multiplier(), 1.0);
    check(in.n_contracts == 10 && in.n_pricing == 4 && in.first_traded == 4, "contract layout");
    check(in.contracts[0].tick_size == to_scaled(0.025), "QMQ1 tick = 0.025");
    check(in.contracts[0].lot_size == 1, "lot from secdb order_increment_qty");
    check(in.contracts[0].venue == 0, "venue from secdb activate (CME = 0)");
    check(in.contracts[4].venue == 0 && in.contracts[4].lot_size == 1,
          "spread inherits venue/lot from its first leg");
    check(in.contracts[0].price_change_threshold == in.contracts[0].tick_size / 20,
          "threshold = 0.05 * tick");
    check(in.contracts[0].weight == 1.0 && in.contracts[1].weight == 0.0, "weights from PricingConfigs");
    // spread width 0.008 -> 800k, extra = width * (multiplier - 1) = 800k
    check(in.contracts[4].width == to_scaled(0.008), "spread width 0.008");
    check(in.contracts[4].extra_width == to_scaled(0.008), "extra width = width * (2 - 1)");
    check(in.contracts[4].leg_count == 2, "spread has two legs");
    check(in.legs[in.contracts[4].leg_begin].index == 0, "first leg is QMQ1");
    check(in.legs[in.contracts[4].leg_begin + 1].index == 1, "second leg is QMU1");
    check(in.legs[in.contracts[4].leg_begin].ratio == 1.0 &&
              in.legs[in.contracts[4].leg_begin + 1].ratio == -1.0,
          "leg ratios +1 / -1");
    // lean matrix: QMQ1-QMU1 leans on QMU1-QMV1 (row 0 col 1 == 1); the diagonal is 0
    check(in.leans[4 * 10 + 5] == 1, "lean QMQ1-QMU1 -> QMU1-QMV1");
    check(in.leans[4 * 10 + 4] == 0, "no self-lean on the LeanMatrix diagonal");
    check(in.leans[4 * 10 + 6] == 0, "no lean QMQ1-QMU1 -> QMQ1-QMV1");
    // hedge ratios: diagonal 0, cross pairs +/-1 for a two-leg spread of two spreads
    check(in.hedge_ratio[4 * 10 + 4] == 0.0, "diagonal hedge ratio 0");
    check(std::fabs(std::fabs(in.hedge_ratio[4 * 10 + 5]) - 1.0) < 1e-9, "cross hedge ratio +/-1");
    check(in.factor_hedged_index[4 * 10 + 4] == -1, "diagonal factor index -1");
    // quote sizes and commissions must be finite and non-zero somewhere
    double max_quote = 0.0;
    double max_comm = 0.0;
    for (size_t k = 0; k < in.quote_size.size(); ++k) {
        check(std::isfinite(in.quote_size[k]), "quote size finite");
        if (k / 10 >= 4 && k % 10 >= 4) max_quote = std::max(max_quote, in.quote_size[k]);
        max_comm = std::max(max_comm, std::fabs(static_cast<double>(in.commissions[k])));
    }
    check(max_quote > 0.0, "some quote size is positive");
    check(max_comm > 0.0, "some commission is non-zero");

    // --- build + run a cycle with real numbers ---
    // The config's per-vector `ltf` are persisted snapshots from a live session (the
    // strategy stores/loads them), so they are meaningless next to synthetic books.
    // Force set-to-market for this check: then stf must reproduce the fed mids.
    model_inputs run_inputs = in;
    for (auto& vi : run_inputs.vectors) {
        vi.set_to_market = true;
        vi.use_ema = false;
        vi.initial_ltf = 0.0;
    }
    const model m = model::build(run_inputs);
    cycle_state s{};
    s.init(m);
    quote_policy policy{};
    order_port port{};
    strategy strat(m, s, policy, port);
    // Consistent books: the spreads are priced from their legs, exactly as the model
    // assumes, so the weighted least-squares fit must round-trip with no residual.
    const price_t tick = m.tick[0].divisor;
    std::vector<price_t> outright_mid(m.n_pricing, 0);
    for (uint32_t c = 0; c < m.n_pricing; ++c) {
        const price_t px = to_scaled(60.0 + 0.025 * c);
        outright_mid[c] = px + tick / 2;  // midpoint of bid / ask below
        strat.on_market_data(c, side::bid, true, px, 25);
        strat.on_market_data(c, side::ask, true, px + tick, 25);
    }
    std::vector<price_t> spread_mid(m.n_all, 0);
    for (uint32_t c = m.first_traded; c < m.n_all; ++c) {
        double fair = 0.0;
        for (uint32_t k = m.leg_begin[c]; k < m.leg_begin[c + 1]; ++k) {
            fair += static_cast<double>(outright_mid[m.leg_index[k]]) * m.leg_ratio[k];
        }
        spread_mid[c] = static_cast<price_t>(fair);
        strat.on_market_data(c, side::bid, true, spread_mid[c] - tick / 2, 25);
        strat.on_market_data(c, side::ask, true, spread_mid[c] + tick / 2, 25);
    }
    strat.run_cycle(1000);

    // with unit multipliers and set-to-market, ODM -> P -> risk^T is a round trip
    price_t worst_ticks = 0;
    for (uint32_t c = 0; c < m.n_pricing; ++c) {
        worst_ticks = std::max(worst_ticks, std::abs(s.stf[c] - outright_mid[c]) / tick);
    }
    check(worst_ticks == 0, "stf reproduces the fed mids exactly on consistent books");

    price_t quoted = 0;
    bool sane = true;
    for (uint32_t c = m.first_traded; c < m.n_all; ++c) {
        for (side sd : {side::bid, side::ask}) {
            const quote& q = s.quote_of(c, sd);
            if (q.price == tachyon::NULL_PRICE) continue;
            ++quoted;
            const double diff = std::fabs(to_dollars(q.price - spread_mid[c]));
            if (diff > 0.05) sane = false;  // within 5c of the leg-implied fair value
        }
    }
    check(sane, "spread quotes land near the leg-implied fair value");
    check(quoted > 0, "at least one spread is quoted");
    std::printf("  round-trip worst = %lld ticks, quoted sides = %lld, r0j: %.2f %.2f %.2f %.2f %.2f %.2f\n",
                (long long)worst_ticks, (long long)quoted, in.hedge_ratio[40], in.hedge_ratio[41],
                in.hedge_ratio[42], in.hedge_ratio[43], in.hedge_ratio[44], in.hedge_ratio[45]);
}

// ---------------------------------------------------------------------------
// 6b. a contract listed in both matrices is one priced+quoted contract (PLAN §12)
// ---------------------------------------------------------------------------
void test_priced_and_quoted_overlap() {
    std::printf("pricing+quoting overlap (config/2msgw: GEH1/ZTH1 in both matrices)\n");
    const rv_model_cfg cfg = rv_model_cfg::load("config/2msgw/2msgw_model.csv");
    const secdb_table secdb = load_secdb("config/2msgw/secdb.cme.csv");
    const model_inputs in = build_model_inputs(cfg, secdb, cfg.width_multiplier(), 1.0);

    check(in.n_contracts == 2, "the overlap merges into 2 contracts, not 4");
    check(in.n_pricing == 2 && in.n_vectors == 2, "still a 2x2 square pricing model");
    check(in.first_traded == 2, "first_traded stays the first non-pricing index");
    check(in.contracts[0].traded && in.contracts[1].traded, "both contracts are quoted");
    check(in.contracts[0].weight == 1.0 && in.contracts[1].weight == 1.0,
          "both contracts keep their PricingConfigs weight");
    check(in.contracts[0].leg_count <= 1 && in.contracts[1].leg_count <= 1,
          "a priced+quoted contract is an outright, not a spread");
    check(in.hedge_ratio[0 * 2 + 1] != 0.0 && in.hedge_ratio[1 * 2 + 0] != 0.0,
          "hedge ratios between the two traded contracts are derived");
    check(in.leans[0 * 2 + 1] == 1 && in.leans[1 * 2 + 0] == 1, "lean matrix is off-diagonal");

    const model m = model::build(in);
    cycle_state s{};
    s.init(m);
    quote_policy policy{};  // no validate hook: exercise quoting, not the gate
    order_port port{};
    strategy strat(m, s, policy, port);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const price_t mid = to_scaled(100.0 + c);
        strat.on_market_data(c, side::bid, true, mid - m.tick[c].divisor, 500);
        strat.on_market_data(c, side::ask, true, mid + m.tick[c].divisor, 500);
    }
    strat.run_cycle(1000);

    // The regression this pins: compute_quotes() used to start at first_traded, so a
    // priced+quoted contract (both of them here) never got a quote at all.
    uint32_t quoted = 0;
    for (uint32_t c = 0; c < m.n_all; ++c) {
        for (side sd : {side::bid, side::ask}) {
            if (s.quote_of(c, sd).price != tachyon::NULL_PRICE) ++quoted;
        }
    }
    check(quoted == 4, "every side of every overlapping contract is quoted");
    check(s.stf[0] != 0 && s.stf[1] != 0, "stf is computed for the overlapping contracts");
    std::printf("  contracts=%u traded=%u quoted sides=%u\n", m.n_all,
                (unsigned)(m.traded[0] + m.traded[1]), quoted);
}

// ---------------------------------------------------------------------------
// 6c. spread legs come from secdb's `details` JSON (PLAN §12)
// ---------------------------------------------------------------------------
void test_secdb_details_legs() {
    std::printf("secdb details -> spread legs\n");

    // config/minimal: the PricingMatrix header carries ICE's `!` suffix while the spread
    // names do not, so the name splitter alone cannot resolve "BRN FMQ0022".
    const secdb_table secdb = load_secdb("config/minimal/secdb.ice.csv");
    check(secdb.size() == 6, "six secdb rows parsed (3 outrights + 3 spreads)");
    const auto spread = secdb.find("BRN FMQ0022-BRN FMU0022");
    check(spread != secdb.end(), "spread row present in secdb");
    if (spread != secdb.end()) {
        check(spread->second.legs.size() == 2, "the `details` column yields two legs");
        if (spread->second.legs.size() == 2) {
            check(spread->second.legs[0].sym == "BRN FMQ0022!", "leg 1 keeps ICE's `!` suffix");
            check(spread->second.legs[1].sym == "BRN FMU0022!", "leg 2 keeps ICE's `!` suffix");
            check(spread->second.legs[0].qty == 1 && spread->second.legs[1].qty == -1,
                  "leg ratios are the signed secdb quantities");
        }
        check(spread->second.tick == 0.01, "columns after `details` are not shifted");
        check(spread->second.order_increment_qty == 1, "order_increment_qty survives the JSON");
    }

    const rv_model_cfg cfg = rv_model_cfg::load("config/minimal/BRN_small_model.csv");
    const model_inputs in = build_model_inputs(cfg, secdb, cfg.width_multiplier(), 1.0);
    check(in.n_contracts == 6 && in.n_pricing == 3, "3 pricing + 3 traded contracts");
    const contract_input& ci = in.contracts[3];
    check(ci.leg_count == 2, "the spread resolved two legs from secdb");
    if (ci.leg_count == 2) {
        check(in.legs[ci.leg_begin].index == 0 && in.legs[ci.leg_begin + 1].index == 1,
              "legs resolve to the pricing columns (with the `!` alias)");
        check(in.legs[ci.leg_begin].ratio == 1.0 && in.legs[ci.leg_begin + 1].ratio == -1.0,
              "ratios +1/-1 from the secdb quantities");
    }

    // The unquoted export variant (secdb.ice.all.csv loses the outer quotes) must parse to
    // the same thing: the `details` commas may not shift the columns after it.
    const secdb_table all = load_secdb("config/minimal/secdb.ice.all.csv");
    const auto all_spread = all.find("BRN FMQ0022-BRN FMU0022");
    check(all_spread != all.end(), "unquoted export still yields the spread row");
    if (all_spread != all.end()) {
        check(all_spread->second.tick == 0.01, "unquoted export: tick column still aligned");
        check(all_spread->second.legs.size() == 2, "unquoted export: legs still parsed");
    }
}

// ---------------------------------------------------------------------------
// 7. every model CSV in the repo must parse and satisfy the square assumption (R3)
// ---------------------------------------------------------------------------
void test_all_configs() {
    std::printf("all model configs parse + are square (R3 audit)\n");
    const char* files[] = {
        "config/2msgw/2msgw_model.csv",
        "config/backtest/model.csv",
        "config/cross_minimal/model.smallcross.csv",
        "config/minimal/BRN_small_model.csv",
        "config/minimal_b3/b3_cert.csv",
        "config/minimal_cme/AS_model.csv",
        "config/minimal_cme/CME_small_model.csv",
        "config/minimal_cme_2/CME_small_model.csv",
        "config/minimal_cme_3/CME_small_model.csv",
        "config/minimal_eurex/EUREX_small_model.csv",
        "config/minimal_ice_2/BRN_small_model.csv",
    };
    for (const char* path : files) {
        rv_model_cfg cfg;
        try {
            cfg = rv_model_cfg::load(path);
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s: %s\n", path, e.what());
            check(false, std::string("parse ") + path);
            continue;
        }
        const bool square = cfg.vectors.size() == cfg.pricing_contracts.size();
        std::printf("  %-44s vectors=%2u pricing=%2u traded=%2u leans=%2u %s\n", path,
                    (unsigned)cfg.vectors.size(), (unsigned)cfg.pricing_contracts.size(),
                    (unsigned)cfg.rv_contracts.size(), (unsigned)cfg.leans.size(),
                    square ? "square" : "NOT SQUARE");
        check(square, std::string("square model: ") + path);
        check(!cfg.rv_contracts.empty(), std::string("has RV contracts: ") + path);
    }
}

// ---------------------------------------------------------------------------
// 8. hedge path end to end: fill -> residual -> tiling -> order emission
// ---------------------------------------------------------------------------
struct captured_orders {
    std::vector<hedge_order> hedges;
    uint32_t quotes_resent = 0;
    static void on_hedge(void* ctx, const hedge_order& order) {
        static_cast<captured_orders*>(ctx)->hedges.push_back(order);
    }
    static void on_quote(void* ctx, uint32_t) {
        ++static_cast<captured_orders*>(ctx)->quotes_resent;
    }
};

void test_hedge_path() {
    std::printf("hedge path end to end (fill -> hedge order)\n");
    fixture f;
    const model m = model::build(f.in);
    cycle_state s{};
    s.init(m);
    quote_policy policy{};
    captured_orders captured;
    order_port port{};
    port.send_hedge = &captured_orders::on_hedge;
    port.send_quote = &captured_orders::on_quote;
    port.ctx = &captured;
    strategy strat(m, s, policy, port);

    // rhone clip is a runtime input (legacy SetRhoneLimits). Force it to 0 the way a
    // host that never delivered limits would: any fill must trip S12 and disable.
    strat.set_max_clip(4, 0);
    strat.on_fill(4, side::bid, 1);
    strat.run_cycle(1000);
    check(!strat.enabled(), "fill without rhone limits disables RV (S12)");
    check(captured.hedges.empty(), "no hedge emitted without limits");

    // fresh engine with limits set: the fill must produce a hedge order
    cycle_state s2{};
    s2.init(m);
    captured_orders captured2;
    order_port port2{};
    port2.send_hedge = &captured_orders::on_hedge;
    port2.send_quote = &captured_orders::on_quote;
    port2.ctx = &captured2;
    strategy strat2(m, s2, policy, port2);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        strat2.set_max_clip(c, 100);
        const price_t px = to_scaled(4.0) + static_cast<price_t>(c) * fixture::TICK;
        strat2.on_market_data(c, side::bid, true, px, 25);
        strat2.on_market_data(c, side::ask, true, px + fixture::TICK, 25);
    }
    strat2.run_cycle(1000);              // establish quotes (residual uses quote.lean)
    strat2.on_fill(4, side::bid, 1);     // we bought 1 of spread 4 -> hedge the lean
    strat2.run_cycle(2000);
    check(strat2.enabled(), "RV stays enabled with a valid clip");
    check(!captured2.hedges.empty(), "fill produced at least one hedge order");
    if (!captured2.hedges.empty()) {
        const hedge_order& o = captured2.hedges.front();
        check(o.size > 0 && o.size <= 100, "hedge size within the clip");
        check(o.price != tachyon::NULL_PRICE, "hedge price is set");
        check(o.hedge_id == 1, "hedge ids start at 1 and increment");
        check(o.market_index == market_index(o.contract, side::bid, true) ||
                  o.market_index == market_index(o.contract, side::bid, false),
              "hedge carries the display market index");
        check(strat2.hedge_id() == captured2.hedges.size(),
              "one hedge id per emitted order");
        std::printf("  first hedge: contract=%u side=%s size=%llu id=%u\n", o.contract,
                    o.side_ == side::bid ? "bid" : "ask", (unsigned long long)o.size,
                    o.hedge_id);
    }
}

// ---------------------------------------------------------------------------
// 9. reservation invalidation: a replaced price level drops its reservations
//    (legacy Market::update flushes FAK adjustments; updateInternalMarket never does)
// ---------------------------------------------------------------------------
void test_reservation_invalidation() {
    std::printf("reservation invalidation on price change\n");
    fixture f;
    const model m = model::build(f.in);
    cycle_state s{};
    s.init(m);

    liquidity_ledger ledger;
    ledger_bridge bridge(ledger, m.n_all);
    const market_key bid_key = make_market_key(2, tachyon::om::order_side::BUY, EXTERNAL);
    const market_key ask_key = make_market_key(2, tachyon::om::order_side::SELL, EXTERNAL);
    bridge.set_market(2, side::bid, true, bid_key, LIQUIDITY_SOURCE_EXTERNAL);
    bridge.set_market(2, side::ask, true, ask_key, LIQUIDITY_SOURCE_EXTERNAL);

    quote_policy policy = ledger_bridge::make_policy(bridge);
    order_port port{};
    strategy strat(m, s, policy, port);

    strat.on_market_data(2, side::bid, true, to_scaled(4.0), 100);
    strat.on_market_data(2, side::ask, true, to_scaled(4.25), 100);
    check(bridge.available(2, side::bid, true, 100) == 100, "no reservations at first");

    // reserve 30 against the displayed bid
    const reservation_id r = ledger.reserve_hedge(bid_key, LIQUIDITY_SOURCE_EXTERNAL, 11, 30);
    check(r.value != reservation_id::INVALID, "hedge reservation taken");
    check(bridge.available(2, side::bid, true, 100) == 70, "reservation reduces available size");

    // a *size-only* update must NOT flush (legacy only flushes on price change)
    strat.on_market_data(2, side::bid, true, to_scaled(4.0), 90);
    check(bridge.available(2, side::bid, true, 90) == 60, "size-only update keeps the reservation");

    // a *price* change flushes it, exactly like legacy Market::update
    strat.on_market_data(2, side::bid, true, to_scaled(4.01), 90);
    check(bridge.available(2, side::bid, true, 90) == 90, "price change flushes the reservation");

    // internal markets never flush (legacy updateInternalMarket)
    const market_key internal_key = make_market_key(2, tachyon::om::order_side::BUY, INTERNAL);
    bridge.set_market(2, side::bid, false, internal_key, LIQUIDITY_SOURCE_RV_QUOTE);
    const reservation_id ri =
        ledger.reserve_hedge(internal_key, LIQUIDITY_SOURCE_RV_QUOTE, 12, 20);
    check(ri.value != reservation_id::INVALID, "internal reservation taken");
    strat.on_market_data(2, side::bid, false, to_scaled(3.99), 100);
    strat.on_market_data(2, side::bid, false, to_scaled(3.98), 100);
    check(ledger.reserved(internal_key) == 20, "internal price change does not flush");
}

}  // namespace


int main(int argc, char** argv) {
    const uint64_t cycles = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
    test_price_scale();
    test_fast_div();
    test_fast_div_limit();
    test_jpx_magnitude();
    test_model_init();
    test_missing_side();
    test_hedge_cost_scaling();
    test_ledger_bridge();
    test_real_config("config/backtest/model.csv");
    test_priced_and_quoted_overlap();
    test_secdb_details_legs();
    test_all_configs();
    test_hedge_path();
    test_reservation_invalidation();
    test_differential(cycles);
    std::printf("\n%u failure(s) (%llu cycles)\n", g_failures, (unsigned long long)cycles);
    return g_failures == 0 ? 0 : 1;
}
