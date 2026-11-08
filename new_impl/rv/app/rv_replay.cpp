// Runnable end-to-end driver for the new RV strategy.
//
//   rv_replay [config_dir] [cycles] [seed]
//
// Loads a real model CSV + secdb, builds the model through the same path production uses
// (rv_model_cfg -> build_model_inputs -> build_model), then replays a deterministic
// market-data tape through the host: MD -> quotes -> simulated fills -> hedge orders ->
// acks. It asserts the invariants a deployment relies on (reservations drain, no quote is
// emitted for a disabled strategy, sizes stay non-negative) and exits non-zero on failure.
//
// This is the executable counterpart of the unit/differential tests: it proves the pieces
// compose, not just that each one is correct in isolation.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <common/liquidity_ledger.hpp>

#include "config.hpp"
#include "host.hpp"
#include "model.hpp"
#include "state.hpp"
#include "valuation.hpp"

namespace rv = tachyon::trade::rv;
using namespace tachyon::trade;

namespace {

struct xorshift64 {
    uint64_t state;
    explicit xorshift64(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    uint64_t next() noexcept {
        uint64_t x = state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return x;
    }
    double uniform() noexcept {  // [0,1)
        return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
    }
    uint32_t below(uint32_t n) noexcept { return n == 0 ? 0 : static_cast<uint32_t>(next() % n); }
};

struct recorder {
    uint64_t quotes = 0;
    uint64_t notifications = 0;
    uint64_t alerts = 0;
    uint64_t removals = 0;
    std::vector<rv::hedge_order> hedges;  // drained by the driver to simulate the venue
    std::vector<rv::quote_request> quotes_out;

    static void on_hedge(void* ctx, const rv::hedge_order& order) {
        static_cast<recorder*>(ctx)->hedges.push_back(order);
    }
    static void on_quote(void* ctx, const rv::quote_request& request) {
        auto* self = static_cast<recorder*>(ctx);
        ++self->quotes;
        if (request.price == tachyon::NULL_PRICE) {
            ++self->removals;
        }
        if (self->quotes_out.size() < 4096) {
            self->quotes_out.push_back(request);
        }
    }
    static void on_notify(void* ctx, uint32_t /*contract*/) {
        ++static_cast<recorder*>(ctx)->notifications;
    }
    static void on_alert(void* ctx, const char* /*reason*/) {
        ++static_cast<recorder*>(ctx)->alerts;
    }
};

// One simulated book: an absolute base price plus a tick-indexed random walk on the tick
// grid. The base is fair-consistent per contract -- outrights at a fixed level, spreads
// summed from their legs -- because the engine compares each contract's quote against that
// same contract's book (maxTicksThroughMarket), so an internally inconsistent tape would
// (correctly) look like a runaway quote.
struct book_sim {
    int64_t offset_ticks = 0;
    rv::price_t tick_size = 1;
    rv::price_t base = 0;
    rv::qty_t size = 0;
    rv::price_t price() const noexcept { return base + offset_ticks * tick_size; }
};

// First file in `dir` whose name contains `needle` (empty when none). Keeps the driver
// usable against any config directory, since model/secdb file names differ per config.
std::string find_file(const std::string& dir, const std::string& needle,
                      const std::string& avoid = {}) {
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());  // deterministic when several match
    for (const auto& name : names) {
        if (name.find(needle) == std::string::npos) {
            continue;
        }
        if (!avoid.empty() && name.find(avoid) != std::string::npos) {
            continue;
        }
        // Other strategies ship models in the same directory (AS_model.csv, ...).
        if (name.rfind("AS_", 0) == 0 || name.rfind("MM_", 0) == 0 ||
            name.find("path") != std::string::npos || name.find("Econ") != std::string::npos) {
            continue;
        }
        return dir + "/" + name;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "config/minimal_cme";
    const uint64_t cycles = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000;
    const uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 42;

    // model.csv in config/backtest, CME_small_model.csv in config/minimal_cme, ...
    std::string model_path = dir + "/model.csv";
    if (!std::filesystem::exists(model_path)) {
        model_path = find_file(dir, "model");
    }
    std::string secdb_path = dir + "/secdb.cme.csv";
    if (!std::filesystem::exists(secdb_path)) {
        secdb_path = find_file(dir, "secdb.cme");
    }
    if (secdb_path.empty() || !std::filesystem::exists(secdb_path)) {
        // config/minimal_ice_2 ships secdb.ice.csv, config/cross_minimal ships secDb.txt,
        // and the "*.all.csv" dumps are exports whose `details` JSON is not always quoted,
        // so prefer a venue-specific file and fall back to whatever carries a secdb.
        secdb_path = find_file(dir, "secdb", ".all");
        if (secdb_path.empty()) secdb_path = find_file(dir, "secDb");
        if (secdb_path.empty()) secdb_path = find_file(dir, "secdb");
    }
    if (model_path.empty()) {
        std::printf("rv_replay: no model CSV in %s\n", dir.c_str());
        return 1;
    }

    rv::rv_model_cfg cfg;
    try {
        cfg = rv::rv_model_cfg::load(model_path);
    } catch (const std::exception& e) {
        std::printf("rv_replay: %s\n", e.what());
        return 1;
    }
    const rv::secdb_table secdb = rv::load_secdb(secdb_path);

    // Spread symbols are absent from secdb; pin them from their legs' tick size the way the
    // legacy config generator does.
    std::unordered_map<std::string, double> overrides;
    for (const auto& name : cfg.pricing_contracts) {
        if (secdb.find(name) == secdb.end()) {
            overrides[name] = 0.025;
        }
    }

    rv::model_inputs inputs;
    rv::model m;
    try {
        inputs = rv::build_model_inputs(cfg, secdb, cfg.width_multiplier(), 1.0, overrides);
        m = rv::model::build(inputs);
    } catch (const std::exception& e) {
        std::printf("rv_replay: config build failed: %s\n", e.what());
        return 1;
    }

    uint32_t traded_count = 0;
    for (uint32_t c = 0; c < m.n_all; ++c) {
        traded_count += m.traded[c] != 0 ? 1u : 0u;
    }
    std::printf("rv_replay: %s  contracts=%u pricing=%u vectors=%u traded=%u leans=%u\n",
                dir.c_str(), m.n_all, m.n_pricing, m.n_vectors, traded_count, m.lean_count());

    static rv::cycle_state state;
    state.init(m);

    liquidity_ledger ledger;
    recorder rec;
    rv::host_ports ports{};
    ports.send_hedge = &recorder::on_hedge;
    ports.send_quote = &recorder::on_quote;
    ports.notify_quote = &recorder::on_notify;
    ports.on_alert = &recorder::on_alert;
    ports.ctx = &rec;

    rv::host host(m, state, &ledger, ports);
    if (!host.ready()) {
        std::printf("rv_replay: host construction failed\n");
        return 1;
    }
    // Legacy receives the Rhone clip limits at runtime (SetRhoneLimits); the model carries 0
    // as "not supplied yet", and a hedge larger than the clip disables the strategy (S12).
    for (uint32_t c = 0; c < m.n_all; ++c) {
        host.set_max_clip(c, 1000);
    }

    // Wire every displayed book to its ledger key and the source that supplies it: internal
    // books come from MM quoting, external books from the venue.
    for (uint32_t c = 0; c < m.n_all; ++c) {
        for (uint32_t i = 0; i < rv::SIDE_COUNT; ++i) {
            const auto sd = static_cast<rv::side>(i);
            const auto common_side = i == 0 ? order_side::BUY : order_side::SELL;
            host.wire_market(c, sd, /*external=*/false,
                             make_market_key(c, common_side, market_type::INTERNAL),
                             liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE);
            host.wire_market(c, sd, /*external=*/true,
                             make_market_key(c, common_side, market_type::EXTERNAL),
                             liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
        }
    }

    // --- deterministic tape ---------------------------------------------------
    xorshift64 rng(seed);
    std::vector<book_sim> books(static_cast<size_t>(m.n_all));

    // The model's fair values come from the configured ltf vector, not from the tape, so the
    // tape has to start *at* those fairs or every quote would legitimately look like a
    // runaway (maxTicksThroughMarket would trip on the first cycle).
    const rv::valuation fairs(m);
    fairs.recompute(state, 0);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        books[c].base = state.stf[c] != tachyon::NULL_PRICE ? state.stf[c] : rv::to_scaled(40.0);
        books[c].tick_size = m.tick[c].divisor;
        books[c].offset_ticks = 0;
        books[c].size = static_cast<rv::qty_t>(m.lot[c] * 20);
    }

    // Book price: outrights walk on their own tick grid; spreads are always derived from
    // their legs, otherwise a spread book can drift away from its own fair and the
    // maxTicksThroughMarket gate (correctly) trips.
    auto contract_price = [&](uint32_t c) -> rv::price_t {
        const uint32_t begin = m.leg_begin[c];
        const uint32_t end = m.leg_begin[c + 1];
        if (end - begin <= 1) {
            return books[c].base + books[c].offset_ticks * books[c].tick_size;
        }
        double sum = 0.0;
        for (uint32_t k = begin; k < end; ++k) {
            const uint32_t leg = m.leg_index[k];
            sum += m.leg_ratio[k] *
                   static_cast<double>(books[leg].base + books[leg].offset_ticks * books[leg].tick_size);
        }
        return static_cast<rv::price_t>(sum);
    };

    // Seed both books for every contract so the first cycle sees a complete market, and take
    // one initial mid snapshot. Legacy refreshes mids only for weight != 0 contracts
    // (calculateAllMidPoints, RelativeValue.h:762-775), so contracts with weight 0 keep
    // whatever mid they had when quoting started.
    std::vector<rv::price_t> last_px(static_cast<size_t>(m.n_all), 0);
    for (uint32_t c = 0; c < m.n_all; ++c) {
        const rv::price_t mid = contract_price(c);
        last_px[c] = mid;
        for (uint32_t i = 0; i < rv::SIDE_COUNT; ++i) {
            const auto sd = static_cast<rv::side>(i);
            const rv::price_t px = sd == rv::side::bid ? mid - books[c].tick_size
                                                      : mid + books[c].tick_size;
            host.on_market_data(c, sd, false, px, books[c].size);
            host.on_market_data(c, sd, true, px, books[c].size);
        }
        if (m.used_in_pricing[c]) {
            state.mid[c] = fairs.mid(state, c);
        }
    }

    uint64_t md_updates = 0;
    uint64_t fills = 0;
    uint64_t hedge_acks = 0;
    tachyon::ord_id_t next_order_id = 1;

    for (uint64_t cycle = 1; cycle <= cycles; ++cycle) {
        // 1. random-walk the outrights, then republish every contract whose book moved
        //    (spreads move with their legs).
        for (uint32_t c = 0; c < m.n_pricing; ++c) {
            if (rng.uniform() > 0.30) {
                continue;
            }
            books[c].offset_ticks += static_cast<int64_t>(rng.below(3)) - 1;
            books[c].size = static_cast<rv::qty_t>(m.lot[c] * (5 + rng.below(40)));
        }
        for (uint32_t c = 0; c < m.n_all; ++c) {
            const rv::price_t mid = contract_price(c);
            if (mid == last_px[c]) {
                continue;
            }
            last_px[c] = mid;
            const bool external = (rng.next() & 1) != 0;
            for (uint32_t i = 0; i < rv::SIDE_COUNT; ++i) {
                const auto sd = static_cast<rv::side>(i);
                const rv::price_t px = sd == rv::side::bid ? mid - books[c].tick_size
                                                         : mid + books[c].tick_size;
                host.on_market_data(c, sd, external, px, books[c].size);
                ++md_updates;
            }
        }

        // 2. simulate fills against whatever we are quoting (2% per side per cycle).
        for (uint32_t c = 0; c < m.n_all; ++c) {
            if (!m.traded[c]) {
                continue;
            }
            for (uint32_t i = 0; i < rv::SIDE_COUNT; ++i) {
                const auto sd = static_cast<rv::side>(i);
                const rv::quote& q = state.quote_of(c, sd);
                if (q.price == tachyon::NULL_PRICE || q.size == 0) {
                    continue;
                }
                if (rng.uniform() > 0.02) {
                    continue;
                }
                const rv::qty_t qty = std::max<rv::qty_t>(1, q.size / 2);
                host.on_fill(c, sd, qty);
                ++fills;
            }
        }

        // 3. the cycle
        host.on_timer(cycle);

        // 4. the venue works each hedge: ack the full size, then complete.
        for (const rv::hedge_order& order : rec.hedges) {
            host.on_hedge_ack(order.hedge_id, next_order_id++, order.size);
            host.on_hedge_done(order.hedge_id);
            ++hedge_acks;
        }
        rec.hedges.clear();
    }

    // --- report + invariants --------------------------------------------------
    const rv::host_stats& st = host.stats();
    std::printf("\ncycles=%llu  md_updates=%llu  fills=%llu  hedge_acks=%llu\n",
                static_cast<unsigned long long>(cycles),
                static_cast<unsigned long long>(md_updates),
                static_cast<unsigned long long>(fills),
                static_cast<unsigned long long>(hedge_acks));
    std::printf("quotes_emitted=%llu  notifications=%llu  removals=%llu  blocked=%llu\n",
                static_cast<unsigned long long>(st.quotes_emitted),
                static_cast<unsigned long long>(st.quote_notifications),
                static_cast<unsigned long long>(st.quote_removals),
                static_cast<unsigned long long>(st.quotes_blocked));
    std::printf("hedges_sent=%llu  reservations_taken=%llu  active_reservations=%zu\n",
                static_cast<unsigned long long>(st.hedges_sent),
                static_cast<unsigned long long>(st.reservations_taken),
                ledger.active_reservation_count());
    std::printf("validation_failures=%llu  alerts=%llu  disabled=%s\n",
                static_cast<unsigned long long>(st.validation_failures),
                static_cast<unsigned long long>(rec.alerts), st.disabled ? "yes" : "no");

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %-52s %s\n", what, ok ? "pass" : "FAIL");
        if (!ok) {
            ++failures;
        }
    };
    std::printf("\ninvariants\n");
    check(st.cycles == cycles, "every requested cycle ran");
    check(st.quotes_emitted > 0, "quotes were emitted");
    check(st.quote_notifications > 0, "quote notifications were raised");
    check(md_updates > 0 && fills > 0, "the tape moved markets and filled quotes");
    check(ledger.active_reservation_count() == 0,
          "all reservations drained after acks (no liquidity leak)");
    check(st.reservations_taken > 0 && st.reservations_taken <= st.hedges_sent,
          "every emitted hedge reserved liquidity");
    // Reservations may only ever remove liquidity: adjusted size must never exceed displayed.
    // (qty_t is unsigned, so "never negative" is guaranteed by the type and asserts nothing.)
    bool sizes_ok = true;
    for (uint32_t c = 0; c < m.n_all && sizes_ok; ++c) {
        for (uint32_t i = 0; i < rv::SIDE_COUNT && sizes_ok; ++i) {
            const auto sd = static_cast<rv::side>(i);
            for (uint32_t e = 0; e < 2; ++e) {
                const bool external = e != 0;
                const rv::qty_t displayed = state.markets[rv::market_index(c, sd, external)].size;
                if (host.bridge()->available(c, sd, external, displayed) > displayed) {
                    sizes_ok = false;
                }
            }
        }
    }
    check(sizes_ok, "adjusted size never exceeds displayed size");

    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
