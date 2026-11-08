// P0 gate for the new ArbieSpreader engine: the primitive layer, the config readers, the
// init-time derivations and the frozen model's indices.
//
//   as_tests
//
// No test framework, matching rv_tests / mm_tests. Every case is derived from the legacy text
// (src/ArbieSpreader.h/.cpp, src/Common.h), not from this implementation, so a shared bug
// cannot make the gate pass (rv PARITY_NOTES §5).

#include <cstdio>
#include <string>
#include <vector>

#include "config.hpp"
#include "model.hpp"
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

// ---------------------------------------------------------------------------
// 0. primitive layer
// ---------------------------------------------------------------------------
void test_primitives() {
    std::printf("primitives (1e9 scale, legacy 1e-8 quantisation)\n");

    check_eq(PRICE_SCALE, 1'000'000'000LL, "PRICE_SCALE is 1e9");
    check_eq(SCALE_RATIO, 10, "1e-9 is ten times 1e-8");
    check_eq(NULL_PRICE, tachyon::NULL_PRICE, "NULL_PRICE is tachyon's sentinel");

    // to_legacy_price must equal legacy ToRhonePrice >= 0 widened: (int64)(x*1e8)*10.
    // Legacy hf::price_t truncates at 1e-8 (ArbieSpreader.cpp:58, :48, _Path.cpp:11).
    int mismatches = 0;
    for (int i = -20000; i <= 20000; ++i) {
        const double dollars = static_cast<double>(i) / 1024.0 * 0.031;
        const long long legacy = static_cast<long long>(dollars * 1.0e8) * 10;
        if (to_legacy_price(dollars) != legacy) ++mismatches;
    }
    check_eq(mismatches, 0, "to_legacy_price == (int64)(x*1e8)*10 over 40001 values");
    check_eq(to_legacy_price(0.0015), 1'500'000LL, "to_legacy_price(0.0015) == 1.5e6");
    check_eq(to_legacy_price(2.5), 2'500'000'000LL, "to_legacy_price(2.5) == 2.5e9");
    check_eq(to_legacy_price(-0.0015), -1'500'000LL, "to_legacy_price truncates toward zero");

    // side-signed helpers, transcribed from Common.h:437-453
    check_eq(apply_price_offset<side::bid>(100, -30), 70, "BID price + (-offset)");
    check_eq(apply_price_offset<side::ask>(100, -30), 130, "ASK price - (-offset)");
    check_eq(apply_price_offset<side::bid>(100, 30), 130, "BID price + offset");
    check_eq(apply_price_offset<side::ask>(100, 30), 70, "ASK price - offset");
    check_eq(ticks_through_market<side::bid>(110, 100), 10, "BID ticks through market");
    check_eq(ticks_through_market<side::ask>(90, 100), 10, "ASK ticks through market");
    check(is_better_price<side::bid>(101, 100), "BID: higher is better");
    check(is_better_price<side::ask>(99, 100), "ASK: lower is better");
    check_eq(better_price<side::bid>(NULL_PRICE, 7), 7, "BetterPrice: NULL loses");

    // round_to_tick (BID floor / ASK ceil) against the legacy expression, every multiple
    // +-1 tick of nine tick sizes plus a strided sweep (Common.h:1199-1213).
    const long long ticks[] = {PRICE_SCALE / 4,      PRICE_SCALE / 40,   PRICE_SCALE / 100,
                               PRICE_SCALE / 1000,   PRICE_SCALE / 10000, PRICE_SCALE / 100000,
                               PRICE_SCALE / 64,     PRICE_SCALE / 2,    PRICE_SCALE,
                               PRICE_SCALE / 2000000};
    int tick_mismatches = 0;
    for (const long long tick : ticks) {
        const fast_div fd = make_fast_div(tick);
        for (long long k = -60; k <= 60; ++k) {
            for (long long d = -1; d <= 1; ++d) {
                const price_t price = k * tick + d;
                for (const side s : {side::bid, side::ask}) {
                    const price_t legacy = [&]() -> price_t {
                        const price_t remainder = price % static_cast<price_t>(tick);
                        if (!remainder) return price;
                        const price_t truncated = price - remainder;
                        if (s == side::bid) {
                            return remainder > 0 ? truncated
                                                 : truncated - static_cast<price_t>(tick);
                        }
                        return remainder > 0 ? truncated + static_cast<price_t>(tick) : truncated;
                    }();
                    if (round_to_tick(price, fd, s) != legacy) ++tick_mismatches;
                }
            }
        }
    }
    check_eq(tick_mismatches, 0, "round_to_tick == RoundToTickSize over the tick sweep");
}


// ---------------------------------------------------------------------------
// 1. config readers (real files)
// ---------------------------------------------------------------------------
as_model_cfg load_real_config() {
    return as_model_cfg::load("config/minimal_cme/AS_model.csv",
                              "config/minimal_cme/AS_paths.csv",
                              "config/minimal_cme/secdb.cme.csv", rv::shift_column::us);
}

void test_config() {
    std::printf("config (config/minimal_cme)\n");
    const as_model_cfg cfg = load_real_config();

    check_eq(static_cast<long long>(cfg.rv.pricing_contracts.size()), 4, "4 pricing contracts");
    check_eq(static_cast<long long>(cfg.rv.vectors.size()), 4, "4 vectors");
    check_eq(static_cast<long long>(cfg.as_contracts.size()), 2, "2 ASConfigs rows");

    // ASEdgeMultiplier row is `3,1,2,3,4` = Value, Asia, Europe, US, SafeMode. The shift column
    // is index shift+1: legacy fills GlobalConfig::_shifts from column 2 onward
    // (ViperConfig.h:738-741) and Shifts starts at Asia (Common.h:170).
    check(cfg.edge_multiplier == 3.0, "ASEdgeMultiplier US == 3");
    check(cfg.edge_multiplier_for(rv::shift_column::asia) == 1.0, "ASEdgeMultiplier Asia == 1");
    check(cfg.edge_multiplier_for(rv::shift_column::europe) == 2.0, "ASEdgeMultiplier Europe == 2");
    check(cfg.edge_multiplier_for(rv::shift_column::safe_mode) == 4.0,
          "ASEdgeMultiplier SafeMode == 4");

    const as_contract_cfg& off = cfg.as_contracts[0];
    check(off.name == "QMQ2-QMU2", "first ASConfigs row is QMQ2-QMU2");
    check(!off.enabled, "QMQ2-QMU2 is disabled");
    check(off.quote_external, "QMQ2-QMU2 quotes externally");
    check_eq(off.quote_size, 3, "QMQ2-QMU2 quoteSize 3");
    check_eq(static_cast<long long>(off.paths.size()), 4, "QMQ2-QMU2 has 4 paths");

    const as_contract_cfg& on = cfg.as_contracts[1];
    check(on.name == "QMQ2-QMV2", "second ASConfigs row is QMQ2-QMV2");
    check(on.enabled, "QMQ2-QMV2 is enabled");
    check_eq(on.quote_size, 7, "QMQ2-QMV2 quoteSize 7");
    check(on.edge == 0.0005, "QMQ2-QMV2 edge 0.0005");
    check(on.hedge_offset == 0.0004, "QMQ2-QMV2 hedgeOffset 0.0004");
    check(on.max_ticks_through_market == 100.0, "QMQ2-QMV2 maxTicksThroughMarket 100");
    check_eq(static_cast<long long>(on.paths.size()), 4, "QMQ2-QMV2 has 4 paths");

    const char* names[] = {"QU+UV", "QX-VX", "QV", "QU+UX-VX"};
    for (size_t i = 0; i < 4; ++i) {
        check(on.paths[i].name == names[i], std::string("path name ") + names[i]);
        check(on.paths[i].enabled, std::string("path enabled ") + names[i]);
        check(on.paths[i].commission == 0.01, std::string("path commission ") + names[i]);
    }
    check_eq(static_cast<long long>(on.paths[0].leans.size()), 2, "QU+UV leans on 2 contracts");
    check(on.paths[0].leans[0].contract == "QMU2-QMV2", "QU+UV lean 0 = QMU2-QMV2");
    check_eq(on.paths[0].leans[0].multiplier, 1, "QU+UV lean 0 multiplier +1");
    check(on.paths[0].leans[1].contract == "QMQ2-QMU2", "QU+UV lean 1 = QMQ2-QMU2");
    check_eq(static_cast<long long>(on.paths[3].leans.size()), 3, "QU+UX-VX leans on 3");
    check_eq(on.paths[3].leans[1].multiplier, -1, "QU+UX-VX lean 1 multiplier -1");
}

// ---------------------------------------------------------------------------
// 2. derivation + frozen model (real config)
// ---------------------------------------------------------------------------
void test_model(const as_model_cfg& cfg) {
    std::printf("model (contract ids, derived numbers, indices)\n");
    const model_inputs in = build_model_inputs(cfg);

    check_eq(in.n_contracts, 10, "10 contracts (4 outrights + 6 spreads)");
    check_eq(in.n_pricing, 4, "4 pricing contracts");
    check(in.names[0] == "QMQ2" && in.names[3] == "QMX2", "pricing contracts keep ids 0-3");
    check(in.names[4] == "QMQ2-QMU2", "first spread gets id 4");
    check(in.names[6] == "QMQ2-QMV2", "QMQ2-QMV2 is id 6");

    // legacy initializeContracts :52-62, quantised the way ToRhonePrice does (F-AS1)
    const as_contract_input& qv = in.contracts[1];
    check(qv.name == "QMQ2-QMV2", "derived contract is QMQ2-QMV2");
    check_eq(qv.edge, 1'500'000, "edge = ToRhonePrice(0.0005 * 3) = $0.0015");
    check_eq(qv.max_ticks_through_market, 2'500'000'000LL,
             "maxTicks = 100 * tick(0.025) = $2.5");
    check_eq(qv.hedge_offset_price, 10'000, "hedgeOffset = 0.0004 * tick(0.025) = $0.00001");
    check_eq(qv.quote_size, 7, "quoteSize 7");
    check_eq(in.ext_commission[6], 1'000'000, "asExtCommission 0.001 -> $0.001");
    check_eq(in.tick_size[6], 25'000'000, "spread inherits the 0.025 tick of its first leg");
    check_eq(qv.paths[0].commission, 10'000'000, "path commission 0.01 -> $0.01");
    check_eq(static_cast<long long>(qv.paths[0].leans.size()), 2, "QU+UV keeps 2 leans");
    check_eq(qv.paths[0].leans[0].contract, 5, "QU+UV lean resolves QMU2-QMV2 -> id 5");
    check_eq(qv.paths[0].leans[1].contract, 4, "QU+UV lean resolves QMQ2-QMU2 -> id 4");

    const model m = model::build(in);
    check_eq(m.n_path_defs, 8, "8 path definitions (2 contracts x 4 paths)");
    check_eq(m.n_path_slots, 16, "16 path slots (definitions x 2 sides)");
    check_eq(m.n_offsets, 0,
             "no switch paths in the shipped config: every path nets to zero fair exposure");

    check_eq(m.path_count(6, side::bid), 4, "QMQ2-QMV2 has 4 bid path slots");
    check_eq(m.path_count(6, side::ask), 4, "QMQ2-QMV2 has 4 ask path slots");
    check_eq(m.path_count(0, side::bid), 0, "a non-quoting contract has no path slots");

    uint32_t affected = 0;
    for (uint32_t key = 0; key + 1 < m.affected_begin.size(); ++key) {
        affected += m.affected_begin[key + 1] - m.affected_begin[key];
    }
    check_eq(affected, 32, "16 lean edges x 2 path sides = 32 affected slots");

    // inverse index correctness: every lean edge is reachable from its book's affected list
    int missing = 0;
    for (uint32_t d = 0; d < m.n_path_defs; ++d) {
        for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
            for (uint32_t s = 0; s < SIDE_COUNT; ++s) {
                const side path_side = static_cast<side>(s);
                const side lean_side = m.lean_mult[k] > 0 ? path_side : other(path_side);
                const uint32_t slot = model::path_slot(d, path_side);
                const uint32_t key = m.lean_contract[k] * SIDE_COUNT + to_index(lean_side);
                bool found = false;
                for (uint32_t i = m.affected_begin[key]; i < m.affected_begin[key + 1]; ++i) {
                    if (m.affected_slot[i] == slot) found = true;
                }
                if (!found) ++missing;
            }
        }
    }
    check_eq(missing, 0, "all 32 lean edges are reachable from their book");
}

// ---------------------------------------------------------------------------
// 3. short-term-fair offsets and switch paths (synthetic, hand-derived)
//
// 4 outrights O0..O3 (the pricing columns, identity pricing matrix), 3 spreads
//   S4 = O0-O1        S5 = O1-O2        S6 = O0-O2
// and an AS contract quoting S4 with two switch paths. Derivations are legacy
// initializeContracts (:69-212) worked by hand.
// ---------------------------------------------------------------------------
model_inputs make_synthetic_inputs() {
    model_inputs in;
    in.n_contracts = 7;
    in.n_pricing = 4;
    in.n_vectors = 4;
    in.names = {"O0", "O1", "O2", "O3", "S4", "S5", "S6"};
    in.tick_size.assign(7, 25'000'000);
    in.ext_commission.assign(7, 0);
    in.lot_size.assign(7, 1);
    in.venue.assign(7, 0);
    in.pricing_multiplier.assign(7, 1.0);
    in.quantity_multiplier.assign(7, 1.0);

    in.leg_begin = {0, 0, 0, 0, 0, 2, 4, 6};  // S4:[0,2) S5:[2,4) S6:[4,6)
    in.leg_index = {0, 1, 1, 2, 0, 2};
    in.leg_ratio = {1, -1, 1, -1, 1, -1};

    in.range.assign(4, 1.0);
    in.max_position.assign(4, 1.0);
    in.pricing_matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    in.risk = in.pricing_matrix;  // (I^-1)^T == I

    as_contract_input c;
    c.name = "S4";
    c.enabled = true;
    c.quote_size = 5;
    c.edge = 1'000'000;
    c.max_ticks_through_market = 1'000'000'000;
    c.hedge_offset_price = 0;

    path_input switch_path;  // leans S6 (+1): -S6 + S4 = -O1 + O2 = -S5
    switch_path.name = "switch";
    switch_path.enabled = true;
    switch_path.leans = {{6, 1}};

    path_input mixed_path;  // leans O2 (+1): -O2 + S4 = O0 - O1 - O2
    mixed_path.name = "mixed";
    mixed_path.enabled = true;
    mixed_path.leans = {{2, 1}};

    c.paths = {switch_path, mixed_path};
    in.contracts = {c};
    return in;
}

void test_offsets() {
    std::printf("stf offsets (switch paths, spread substitution, fair move)\n");
    const model m = model::build(make_synthetic_inputs());

    check_eq(m.n_path_defs, 2, "2 synthetic paths");
    check_eq(m.n_offsets, 2, "both synthetic paths are switch paths");

    // path 0: pre-replacement terms {O1:-1, O2:+1}; S5 = O1-O2 is present with a consistent
    // ratio -1, so the legs collapse into the spread's own fair (replaceSTFsWithSpreadSTF).
    const uint32_t o0 = m.path_offset[0];
    check_eq(m.offset_term_begin[o0 + 1] - m.offset_term_begin[o0], 1,
             "switch path 0 collapses to one term");
    check_eq(m.offset_term_contract[m.offset_term_begin[o0]], 5, "term of path 0 is S5");
    check_eq(m.offset_term_ratio[m.offset_term_begin[o0]], -1, "S5 ratio is -1");
    check(m.offset_id[o0] == "-1*11*2", "legacy id string of path 0 (pre-replacement)");

    // path 1: {O0:+1, O1:-1, O2:-1}; S4's legs are present with a consistent ratio +1, so the
    // legs collapse and the offset keeps the ascending contract order: O2 (id 2) then S4 (id 4)
    const uint32_t o1 = m.path_offset[1];
    check_eq(m.offset_term_begin[o1 + 1] - m.offset_term_begin[o1], 2, "path 1 has two terms");
    check_eq(m.offset_term_contract[m.offset_term_begin[o1]], 2, "path 1 term 0 is O2");
    check_eq(m.offset_term_ratio[m.offset_term_begin[o1]], -1, "O2 ratio -1");
    check_eq(m.offset_term_contract[m.offset_term_begin[o1] + 1], 4, "path 1 term 1 is S4");
    check_eq(m.offset_term_ratio[m.offset_term_begin[o1] + 1], 1, "S4 ratio +1");
    check(m.offset_id[o1] == "1*0-1*1-1*2", "legacy id string of path 1 (pre-replacement)");

    // calculateRVSTFMove: risk is the identity, range = maxPos = 1, so
    //   factor_move = -net_pos, contract_move = factor_move, stf_move = sum(ratio * move),
    //   rv_stf_move = ToRhonePrice(stf_move / 2).
    // path 0: net_pos = {0,-1,+1,0} -> moves {0,+1,-1,0} -> S5 = +1 - (-1) = 2 -> -1.0
    check_eq(m.path_rv_stf_move[0], -1'000'000'000LL, "path 0 fair move = -$1.00 per unit");
    // path 1: net_pos = {+1,-1,-1,0} -> moves {-1,+1,+1,0}; S4 = -1-1 = -2, O2 = +1
    //         stf_move = 1*(-2) + (-1)*(+1) = -3 -> -1.5
    check_eq(m.path_rv_stf_move[1], -1'500'000'000LL, "path 1 fair move = -$1.50 per unit");

    // offset -> path slots fan-out (both sides of each path)
    check_eq(m.offset_path_begin[o0 + 1] - m.offset_path_begin[o0], 2, "offset 0 drives 2 slots");
    check_eq(m.offset_path_begin[o1 + 1] - m.offset_path_begin[o1], 2, "offset 1 drives 2 slots");
}


// ---------------------------------------------------------------------------
// 4. legacy quirks and rejections
// ---------------------------------------------------------------------------
void test_legacy_quirks() {
    std::printf("legacy quirks (pinned so a refactor cannot silently 'fix' them)\n");

    // ArbieSpreader.cpp:61-62 multiplies the config double by the tick size *in price units*,
    // i.e. hedgeOffsetPrice/maxTicksThroughMarket are the dollar products, not dollar x
    // 1e-8-integer. Written out explicitly here.
    const double hedge_offset = 0.0004;
    const double tick_dollars = 0.025;
    check_eq(to_legacy_price(hedge_offset * tick_dollars), 10'000,
             "hedgeOffsetPrice(0.0004, tick 0.025) == $0.00001");
    check_eq(to_legacy_price(100.0 * tick_dollars), 2'500'000'000LL,
             "maxTicksThroughMarket(100 ticks, tick 0.025) == $2.5");

    // With a gold/1e-9 scale the same expressions must not lose the tick: this is the failure
    // mode a "clean up" of the legacy unit mismatch would introduce.
    check(to_legacy_price(hedge_offset * tick_dollars) != to_legacy_price(hedge_offset),
          "hedge offset is scaled by the tick");
}

void test_rejections() {
    std::printf("rejections (init-time failures, not runtime ones)\n");

    int thrown = 0;
    auto expect_throw = [&thrown](model_inputs in, const char* what) {
        try {
            model::build(in);
        } catch (const std::exception& e) {
            ++thrown;
            check(std::string(e.what()).find("as::model::build") != std::string::npos,
                  std::string("message is actionable: ") + what);
            return;
        }
        check(false, std::string("expected a throw: ") + what);
    };

    {   // too many paths on one contract
        model_inputs in = make_synthetic_inputs();
        in.contracts[0].paths.resize(MAX_PATHS_PER_CONTRACT + 1);
        expect_throw(in, "paths per contract cap");
    }
    {   // a lean on a contract that does not exist
        model_inputs in = make_synthetic_inputs();
        in.contracts[0].paths[0].leans[0].contract = in.n_contracts;
        expect_throw(in, "unknown lean contract");
    }
    {   // a leg that does not precede its spread (legacy aggregates in ascending order)
        model_inputs in = make_synthetic_inputs();
        in.leg_index[0] = 6;  // S4 would have leg S6
        expect_throw(in, "leg ordering");
    }
    {   // non-square model: the risk operator inverts the pricing matrix
        model_inputs in = make_synthetic_inputs();
        in.n_vectors = 3;
        expect_throw(in, "non-square model");
    }
    {   // duplicate ASConfigs row
        model_inputs in = make_synthetic_inputs();
        in.contracts.push_back(in.contracts[0]);
        expect_throw(in, "duplicate ASConfigs row");
    }
    check_eq(thrown, 5, "every malformed input is rejected at init");
}

// ---------------------------------------------------------------------------
// 5. alternative configs still parse (the caps are shape-agnostic)
// ---------------------------------------------------------------------------
void test_other_configs() {
    std::printf("other configs (shape-independence of the readers and caps)\n");
    struct alternative {
        const char* label;
        const char* model;
        const char* paths;
        const char* secdb;
    };
    // config/minimal's model file predates the `enabled` column; it must still load.
    const alternative all[] = {
        {"minimal_cme", "config/minimal_cme/AS_model.csv", "config/minimal_cme/AS_paths.csv",
         "config/minimal_cme/secdb.cme.csv"},
        {"minimal", "config/minimal/BRN_small_model.csv", "config/minimal/paths.csv",
         "config/minimal/secdb.ice.csv"},
    };
    for (const alternative& a : all) {
        try {
            const as_model_cfg cfg = as_model_cfg::load(a.model, a.paths, a.secdb);
            const model m = model::build(build_model_inputs(cfg));
            std::printf("  %-12s contracts=%2u paths=%2u slots=%2u offsets=%2u\n", a.label,
                        m.n_all, m.n_path_defs, m.n_path_slots, m.n_offsets);
            check(m.n_all > 0 && m.n_path_defs > 0,
                  std::string("config ") + a.label + " builds a non-empty model");
        } catch (const std::exception& e) {
            check(false, std::string("config ") + a.label + " failed to load: " + e.what());
        }
    }
}

}  // namespace

int main() {
    test_primitives();
    test_config();
    test_model(load_real_config());
    test_offsets();
    test_legacy_quirks();
    test_rejections();
    test_other_configs();

    if (g_failures == 0) {
        std::printf("\nall AS phase-0 checks passed\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}

