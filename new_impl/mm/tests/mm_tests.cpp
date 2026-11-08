// Standalone test binary for the new MarketMaker engine (no test framework, matching the
// style of new_impl/rv/tests/rv_tests.cpp).
//
//   mm_tests
//
// Phase-1 coverage (PLAN.md §7): the price scale and the to_mm_price conversion rule (D2),
// exact tick division against a transcription of the legacy expression, lot flooring, the
// derived config arithmetic of model::build, the legacy validation rules, and a parse of the
// real config/backtest files.
//
// Run from the repository root (ctest sets this) so `config/...` resolves.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "config.hpp"
#include "limits.hpp"
#include "model.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade;
using namespace tachyon::trade::mm;

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

void expect_throw(const std::function<void()>& fn, const std::string& what) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    ++g_failures;
    std::printf("  FAIL  %s (no exception)\n", what.c_str());
}

// Common.h:1196 RoundToTickSize, transcribed, as the reference for round_to_tick().
price_t legacy_round(price_t price, price_t tick, side s) {
    const price_t remainder = price % tick;
    if (!remainder) return price;
    const price_t truncated = price - remainder;
    if (s == side::bid) {
        return remainder > 0 ? truncated : truncated - tick;
    }
    return remainder > 0 ? truncated + tick : truncated;
}

// The conversion the naive approach would use, kept here to prove it is *not* what we do.
price_t naive_scaled(double dollars) {
    return static_cast<price_t>(dollars * static_cast<double>(PRICE_SCALE));
}

// ---------------------------------------------------------------------------
// 0. price scale invariants (D2)
// ---------------------------------------------------------------------------
void test_price_scale() {
    std::printf("price scale (1e9 = %lld, legacy 1e8 = %lld)\n", (long long)PRICE_SCALE,
                (long long)LEGACY_PRICE_SCALE);
    check(PRICE_SCALE == 1'000'000'000, "PRICE_SCALE is 1e9");
    check(LEGACY_PRICE_SCALE == 100'000'000, "legacy scale is 1e8");
    check(LEGACY_TO_MM == 10, "legacy -> engine is exactly one decade");
    check(to_mm_price(1.0) == PRICE_SCALE, "to_mm_price($1) == PRICE_SCALE");
    check(to_mm_price(0.025) == 25'000'000, "to_mm_price(0.025) == 2.5e7");
    check(to_dollars(25'000'000) == 0.025, "to_dollars round trip");
}

// to_mm_price must equal (legacy truncated value) * 10 for every config value, which is what
// makes scaling-invariant arithmetic decision-identical to the legacy 1e-8 engine.
void test_to_mm_price_is_ten_times_legacy() {
    std::printf("to_mm_price == 10 * legacy truncation\n");

    // Values that appear in config/*/model.csv MMConfigs, plus decimal traps.
    const double values[] = {0.0,   1e-05, 0.0001,       0.0025,       0.005, 0.008, 0.01,
                             0.025, 0.05,  0.556,        0.65477777777, 1.0,   3.0,   12.5,
                             60.0};
    for (double v : values) {
        const price_t legacy = static_cast<price_t>(v * static_cast<double>(LEGACY_PRICE_SCALE));
        check_eq(to_mm_price(v), legacy * LEGACY_TO_MM, "to_mm_price(" + std::to_string(v) + ")");
    }

    // 0.65477777777 is the real MMWorsenPayUpCondPct of the first MM contract and the
    // documented trap: the legacy 1e-8 product truncates to 65'477'777, so the engine value
    // must be 654'777'770 while a naive 1e9 conversion gives 654'777'777.
    check_eq(to_mm_price(0.65477777777), 654'777'770, "to_mm_price reproduces legacy truncation");
    check_eq(naive_scaled(0.65477777777), 654'777'777, "naive 1e9 conversion truncates later");
    check(naive_scaled(0.65477777777) != to_mm_price(0.65477777777),
          "the x10 rule matters for >8-decimal config values (why D2 uses it)");
    // A clean decimal agrees either way, which is why the trap is easy to miss.
    check_eq(to_mm_price(0.025), naive_scaled(0.025), "clean decimals agree under both rules");

    // Sweep 200k dense values: the rule holds for every one of them.
    uint64_t mismatches = 0;
    for (int64_t i = 1; i <= 200'000; ++i) {
        const double v = static_cast<double>(i) * 1e-9;  // 1e-9 .. 2e-4
        const price_t legacy = static_cast<price_t>(v * static_cast<double>(LEGACY_PRICE_SCALE));
        if (to_mm_price(v) != legacy * LEGACY_TO_MM) ++mismatches;
    }
    check_eq(static_cast<long long>(mismatches), 0, "200k-value sweep is exact");
}

// ---------------------------------------------------------------------------
// 1. tick rounding: fast_div path, constant-dispatch path, transcribed legacy expression
// ---------------------------------------------------------------------------
void test_tick_rounding() {
    std::printf("tick rounding (legacy expression == constant dispatch == fast_div)\n");

    const price_t ticks[] = {25'000'000,  // 0.025 (the config/backtest tick)
                             1'000'000,   // 0.01
                             100'000,     // 0.001
                             10'000,      // 0.0001
                             5'000'000,   // 0.05
                             2'500'000,   // 0.0025
                             781'250,     // 1/128
                             100'000'000, // 1.0
                             3'000'003};  // not in the constant list: exercises floor_divmod

    uint64_t mismatches = 0;
    uint64_t cases = 0;
    for (price_t tick : ticks) {
        const fast_div fd = make_fast_div(tick);
        // Boundary neighbourhoods plus a strided sweep, including negative prices.
        std::vector<price_t> prices;
        for (int64_t k = -40; k <= 40; ++k) {
            prices.push_back(k * tick);
            prices.push_back(k * tick + 1);
            prices.push_back(k * tick - 1);
        }
        for (price_t p = -100LL * tick; p <= 100LL * tick; p += tick / 3 + 1) prices.push_back(p);

        for (price_t p : prices) {
            for (side s : {side::bid, side::ask}) {
                ++cases;
                const price_t want = legacy_round(p, tick, s);
                if (round_to_tick(p, fd, s) != want) ++mismatches;
            }
        }
    }
    check_eq(static_cast<long long>(mismatches), 0,
             "round_to_tick matches legacy over " + std::to_string(cases) + " cases");
    std::printf("  %llu cases across %zu tick sizes\n", (unsigned long long)cases,
                sizeof(ticks) / sizeof(ticks[0]));

    // Spot checks that pin the sign convention independently of the sweep.
    const fast_div t25 = make_fast_div(25'000'000);
    check_eq(round_to_tick(75'000'001, t25, side::bid), 75'000'000, "bid floors");
    check_eq(round_to_tick(75'000'001, t25, side::ask), 100'000'000, "ask ceils");
    check_eq(round_to_tick(-1, t25, side::bid), -25'000'000, "negative bid floors away from zero");
    check_eq(round_to_tick(-1, t25, side::ask), 0, "negative ask ceils toward zero");
    check_eq(round_to_tick(75'000'000, t25, side::ask), 75'000'000, "exact multiple unchanged");
}

void test_valid_qty() {
    std::printf("lot flooring (ConfigContract::toValidQty)\n");
    check_eq(to_valid_qty(8, 1), 8, "lot 1 is a no-op");
    check_eq(to_valid_qty(8, 5), 5, "8 with lot 5 floors to 5");
    check_eq(to_valid_qty(10, 5), 10, "exact multiple unchanged");
    check_eq(to_valid_qty(4, 5), 0, "below one lot floors to 0");
    check_eq(to_valid_qty(0, 5), 0, "zero stays zero");
}

// ---------------------------------------------------------------------------
// 2. model::build — the derived arithmetic of SimpleMarketMaker::initialize (:192-236)
// ---------------------------------------------------------------------------
contract_input make_input(const std::string& name = "TEST") {
    contract_input c;
    c.name = name;
    c.tick_size = to_mm_price(0.025);
    c.lot = 5;
    c.venue = 0;
    c.enabled = true;
    c.quote_external = true;
    c.quote_size = 8;
    c.quote_gap = 5;
    c.pct_tob_max_quote = 0.2;
    c.width_pct = {3.0, 2.0, 0.1, 3.0};
    c.hedge_offset = 1.0;
    c.join_size = 0;
    c.leave_size = 2;
    c.payup_qty = 1;
    c.payup_orders = 1;
    c.payup_multiple = 0.01;
    c.max_pos_internal = 1500;
    c.max_pos_external = 350;
    c.commission = 0.00001;
    c.min_internal_price_change = 0.0001;
    c.est_level_ema = 3.0;
    c.est_level_ema_seconds = 5000;
    c.est_level_pct_ema = 0.8;
    c.max_ticks_wide = -1;
    c.worsen_payup_pct = 0.65477777777;
    c.queue_importance = 3;
    c.internal_position = 0;
    c.external_position = 9;
    return c;
}

void test_model_derivation() {
    std::printf("model::build derived values\n");

    const double frequency = 1.0e6;  // cycles/second; keeps seconds*1e9*freq inside uint64
    model_inputs in;
    in.contracts.push_back(make_input());
    in.cycles_per_second = frequency;
    in.current_shift = 3;
    const model m = model::build(in);

    check_eq(m.n_contracts, 1, "one contract");
    check_eq(m.initial_shift, 3, "initial shift recorded");
    check_eq(m.tick_size[0], 25'000'000, "tick_size == to_mm_price(0.025)");
    check_eq(m.tick[0].divisor, 25'000'000, "fast_div divisor");
    check_eq(m.lot[0], 5, "lot");
    check_eq(m.quote_size[0], 5, "quoteSize is lot-floored (8 -> 5)");
    check_eq(m.valid_qty(0, 8), 5, "valid_qty uses the contract lot");
    check_eq(m.commission[0], to_mm_price(0.00001), "commission at the legacy scale x10");
    check_eq(m.commission[0], 10'000, "commission(1e-05) == 1000 legacy * 10");
    check_eq(m.min_internal_price_change[0], to_mm_price(0.0001), "minInternalPriceChange x10");
    check_eq(m.hedge_offset_price[0], 25'000'000, "hedgeOffset(1 tick) == tick size");
    check_eq(m.max_ticks_wide[0], std::numeric_limits<int64_t>::max(),
             "maxTicksWide < 0 means unbounded");
    check_eq(m.max_ticks_wide_2x[0], std::numeric_limits<int64_t>::max(),
             "2x maxTicksWide also unbounded");
    check_eq(m.worsen_payup_amount[0], static_cast<price_t>(0.65477777777 * 25000000.0),
             "worsenPayup amount == pct * tick (truncated)");
    check_eq(m.est_level_ema_period[0],
             static_cast<uint64_t>(5000.0 * 1000000000.0 * frequency),
             "EMA period == GetCyclesFromSeconds(5000, freq)");
    check_eq(m.queue_importance[0], 3, "queue importance kept");
    check_eq(m.init_internal_position[0], 0, "initial internal position");
    check_eq(m.init_external_position[0], 9, "initial external position");
    check(m.width_pct_of(0, 2) == 0.1, "US width column");

    // round(): the commission is folded in before the tick rounding, per side.
    check_eq(m.round(0, 77'000'000, side::bid), 75'000'000, "bid: (price-commission) floored");
    check_eq(m.round(0, 77'000'000, side::ask), 100'000'000, "ask: (price+commission) ceiled");
    for (price_t p = 0; p < 200'000'000; p += 3'000'001) {
        check_eq(m.round(0, p, side::bid),
                 legacy_round(p - m.commission[0], m.tick_size[0], side::bid),
                 "round(bid) == legacy RoundToTickSize(price - commission)");
        check_eq(m.round(0, p, side::ask),
                 legacy_round(p + m.commission[0], m.tick_size[0], side::ask),
                 "round(ask) == legacy RoundToTickSize(price + commission)");
    }

    // A bounded max-ticks-wide still produces the tick product and its double.
    model_inputs bounded;
    contract_input c = make_input();
    c.max_ticks_wide = 4;
    bounded.contracts.push_back(c);
    bounded.cycles_per_second = frequency;
    const model mb = model::build(bounded);
    check_eq(mb.max_ticks_wide[0], 100'000'000, "4 ticks * 0.025");
    check_eq(mb.max_ticks_wide_2x[0], 200'000'000, "2x of the above");
}

void test_model_validation() {
    std::printf("model::build validation (legacy rules)\n");

    // A zero (lot-floored) quote size is fatal (SimpleMarketMaker.h:205-207).
    {
        model_inputs in;
        contract_input c = make_input("LOTQ");
        c.quote_size = 4;  // < lot 5
        in.contracts.push_back(c);
        in.cycles_per_second = 1e6;
        expect_throw([&] { model::build(in); }, "quoteSize flooring to 0 throws");
    }
    // ViperConfig.h:901: a non-positive or ~zero worsen-payup percentage is rejected.
    {
        model_inputs in;
        contract_input c = make_input("WORSEN0");
        c.worsen_payup_pct = 0.0;
        in.contracts.push_back(c);
        in.cycles_per_second = 1e6;
        expect_throw([&] { model::build(in); }, "worsenPayup 0 throws");
    }
    {
        model_inputs in;
        contract_input c = make_input("WORSENZERO");
        c.worsen_payup_pct = 1e-9;  // IsZero()
        in.contracts.push_back(c);
        in.cycles_per_second = 1e6;
        expect_throw([&] { model::build(in); }, "worsenPayup ~0 throws");
    }
    // A non-positive tick cannot be used for tick rounding.
    {
        model_inputs in;
        contract_input c = make_input("TICK0");
        c.tick_size = 0;
        in.contracts.push_back(c);
        in.cycles_per_second = 1e6;
        expect_throw([&] { model::build(in); }, "zero tick throws");
    }
    // ViperConfig.h:905: queue importance above 10 is silently zeroed, not an error.
    {
        model_inputs in;
        contract_input c = make_input("QI11");
        c.queue_importance = 11;
        in.contracts.push_back(c);
        in.cycles_per_second = 1e6;
        const model m = model::build(in);
        check_eq(m.queue_importance[0], 0, "queue importance > 10 is zeroed");
    }
    // Caps: exceeding MAX_CONTRACTS is an init-time error, never an allocation.
    {
        model_inputs in;
        in.contracts.assign(MAX_CONTRACTS + 1, make_input());
        in.cycles_per_second = 1e6;
        expect_throw([&] { model::build(in); }, "over MAX_CONTRACTS throws");
    }
}

// ---------------------------------------------------------------------------
// 3. the real config/backtest files
// ---------------------------------------------------------------------------
void test_real_config() {
    std::printf("config/backtest parse (MMConfigs + secdb)\n");
    const std::string model_csv = "config/backtest/model.csv";
    const std::string secdb_csv = "config/backtest/secdb.cme.csv";

    mm_model_cfg cfg;
    secdb_table secdb;
    try {
        cfg = mm_model_cfg::load(model_csv);
        secdb = load_secdb(secdb_csv);
    } catch (const std::exception& e) {
        ++g_failures;
        std::printf("  FAIL  cannot load %s / %s: %s (run from the repository root)\n",
                    model_csv.c_str(), secdb_csv.c_str(), e.what());
        return;
    }

    check_eq(static_cast<long long>(cfg.contracts.size()), 6, "6 MM contracts in the model");
    if (cfg.contracts.empty()) return;

    const mm_contract_cfg& first = cfg.contracts.front();
    check(first.name == "QMQ1-QMU1", "first row is QMQ1-QMU1");
    check(!first.enabled, "enabled FALSE");
    check(first.quote_external, "quoteExternal TRUE");
    check_eq(first.quote_size, 8, "quoteSize");
    check_eq(first.quote_gap, 5, "quoteGap");
    check(first.pct_tob_max_quote == 0.2, "quotePctTob");
    check(first.width_pct[0] == 3.0 && first.width_pct[1] == 2.0 && first.width_pct[2] == 0.1 &&
              first.width_pct[3] == 3.0,
          "per-shift widths Asia/Euro/US/SafeMode");
    check(first.hedge_offset == 1.0, "hedgeOffset");
    check_eq(first.join_size, 0, "joinSize");
    check_eq(first.leave_size, 2, "leaveSize");
    check_eq(first.payup_qty, 1, "payupQty");
    check_eq(first.payup_orders, 1, "payupOrders");
    check(first.payup_multiple == 0.01, "payupMultiple");
    check_eq(first.max_pos_internal, 1500, "maxPosInternal");
    check_eq(first.max_pos_external, 350, "maxPosExternal");
    check(first.commission == 1e-05, "commission");
    check(first.min_internal_price_change == 0.0001, "minInternalPriceChange");
    check(first.est_level_ema == 3.0, "EstLevelEMA");
    check_eq(first.est_level_ema_seconds, 5000, "EstLevelEMASeconds");
    check(first.est_level_pct_ema == 0.8, "EstLevel%EMA");
    check_eq(first.max_ticks_wide, -1, "maxTicksWide");
    check(first.worsen_payup_pct == 0.65477777777, "MMWorsenPayUpCondPct");
    check_eq(first.queue_importance, 3, "queueImportance");
    check_eq(first.internal_position, 0, "internalPosition");
    check_eq(first.external_position, 9, "externalPosition");

    // secdb: including the quoted `details` JSON that sits before order_increment_qty.
    const auto outright = secdb.find("QMQ1");
    check(outright != secdb.end(), "secdb has QMQ1");
    if (outright != secdb.end()) {
        check(outright->second.tick == 0.025, "QMQ1 tick 0.025");
        check_eq(outright->second.order_increment_qty, 1, "QMQ1 lot 1");
        check(outright->second.venue_ == venue::cme, "QMQ1 venue CME");
    }
    const auto spread = secdb.find("QMQ1-QMU1");
    check(spread != secdb.end(), "secdb has the spread row");
    if (spread != secdb.end()) {
        check(spread->second.tick == 0.025,
              "spread tick 0.025 (the details JSON did not shift columns)");
    }

    // cfg + secdb -> inputs -> model
    const model_inputs inputs = build_model_inputs(cfg, secdb, 1.0e6, 2);
    check_eq(static_cast<long long>(inputs.contracts.size()), 6, "6 resolved inputs");
    check_eq(inputs.contracts.front().tick_size, 25'000'000, "tick converted at the x10 rule");
    check_eq(inputs.contracts.front().lot, 1, "lot from secdb");

    const model m = model::build(inputs);
    check_eq(m.n_contracts, 6, "model has 6 contracts");
    check_eq(m.tick[0].divisor, 25'000'000, "model tick divisor");
    check_eq(m.commission[0], 10'000, "model commission x10");
    check_eq(m.quote_size[0], 8, "model quote size (lot 1)");
    check_eq(m.max_ticks_wide[0], std::numeric_limits<int64_t>::max(),
             "model maxTicksWide unbounded");
    check(m.initial_shift == 2, "model records the US shift");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_price_scale();
    test_to_mm_price_is_ten_times_legacy();
    test_tick_rounding();
    test_valid_qty();
    test_model_derivation();
    test_model_validation();
    test_real_config();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all MM phase-1 checks passed\n");
    return EXIT_SUCCESS;
}
