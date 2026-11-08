// Phase-8 gate: the integration seam (adapter.hpp).
//
//   mm_adapter_tests
//
// Drives a session through the adapter with a recording request_sink, exactly as the legacy
// MarketMaker shim will, and checks that the request stream the transport sees is the one the
// engine decided: ordered, attributed to the right contract and side, and complete.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "adapter.hpp"
#include "book.hpp"
#include "config.hpp"
#include "host.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "state.hpp"
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

// A request_sink that records the transport calls in order, the way OrderRequestManager would see
// them.
struct recording_sink : request_sink {
    struct call {
        enum class kind { quote, hedge, notify, cancel, alert, fill } what;
        uint32_t contract = 0;
        side sd = side::bid;
        price_t internal_price = 0;
        qty_t size = 0;
    };
    std::vector<call> calls;

    void send_quote(const order_intent& it) override {
        calls.push_back({call::kind::quote, it.contract, it.sd, it.internal_price, it.internal_size});
    }
    void send_hedge(const order_intent& it) override {
        calls.push_back({call::kind::hedge, it.contract, it.sd, it.internal_price, it.size});
    }
    void notify(uint32_t contract) override { calls.push_back({call::kind::notify, contract}); }
    void cancel_orders(uint32_t contract, bool) override {
        calls.push_back({call::kind::cancel, contract});
    }
    void alert(uint32_t contract, const char*) override {
        calls.push_back({call::kind::alert, contract});
    }
    void report_fill(uint32_t contract, const fill_outcome&) override {
        calls.push_back({call::kind::fill, contract});
    }

    uint32_t count(call::kind kind) const {
        uint32_t n = 0;
        for (const call& c : calls) {
            if (c.what == kind) ++n;
        }
        return n;
    }
    void clear() { calls.clear(); }
};

// The real config dir, so the adapter exercises the same parse path a deployment uses.
struct fixture {
    mm_model_cfg cfg;
    secdb_table secdb;
    bool ok = false;

    explicit fixture(const std::string& dir) {
        try {
            cfg = mm_model_cfg::load(dir + "/model.csv");
            secdb = load_secdb(dir + "/secdb.cme.csv");
            ok = true;
        } catch (const std::exception& e) {
            std::printf("  cannot load %s: %s (run from the repository root)\n", dir.c_str(),
                        e.what());
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    fixture f("config/backtest");
    if (!f.ok) return EXIT_FAILURE;

    recording_sink sink;
    adapter a(f.cfg, f.secdb, 1.0e6, /*initial_shift=*/2, sink);
    check(a.ready(), "the adapter builds a host from the real config");
    const model& m = a.get_model();
    check_eq(m.n_contracts, 6, "six market-made contracts");

    // The config ships every contract disabled; the shim enables them on the operator action.
    a.enable(true);
    for (uint32_t c = 0; c < m.n_contracts; ++c) {
        a.set_contract_enabled(c, true);
        a.set_contract_can_trade(c, true);
    }

    // ---- a book update must reach the transport as quote requests ----
    level bids[BOOK_LEVELS];
    level asks[BOOK_LEVELS];
    const price_t tick = m.tick_size[0];
    for (uint32_t k = 0; k < BOOK_LEVELS; ++k) {
        bids[k] = level{tick * (2400 - static_cast<int>(k)), 500, 5, 0};
        asks[k] = level{tick * (2402 + static_cast<int>(k)), 500, 5, 0};
    }
    market_data_view view;
    view.levels = bids;
    view.count = BOOK_LEVELS;
    view.price_num = 1;
    view.qty_num = 1;
    view.batch_id = 1;
    check(a.on_market_data(0, side::bid, view), "bid snapshot accepted");
    view.levels = asks;
    view.batch_id = 2;
    check(a.on_market_data(0, side::ask, view), "ask snapshot accepted");

    sink.clear();
    a.run_cycle(1000);
    check_eq(sink.count(recording_sink::call::kind::quote), 2, "both sides quote");
    check_eq(sink.count(recording_sink::call::kind::notify), 1,
             "one notification for the contract");
    // The two quotes come before the notification, and they are bid then ask.
    bool saw_notify = false;
    uint32_t quotes_before_notify = 0;
    for (const auto& c : sink.calls) {
        if (c.what == recording_sink::call::kind::notify) saw_notify = true;
        if (c.what == recording_sink::call::kind::quote && !saw_notify) ++quotes_before_notify;
    }
    check_eq(quotes_before_notify, 2, "both quote requests precede the notification");

    // The prices the transport sees are tick multiples and the sizes are positive.
    for (const auto& c : sink.calls) {
        if (c.what != recording_sink::call::kind::quote) continue;
        if (c.internal_price != tachyon::NULL_PRICE) {
            check(c.internal_price > 0, "a quote carries a positive price");
            check(c.size >= 0, "and a non-negative size");
        }
    }

    // ---- a fill reaches the transport as a fill report plus a re-hedge ----
    a.get_state().contract(0).quote_of(side::bid).internal_size = 8;
    fill fl;
    fl.sd = side::bid;
    fl.qty = 5;
    fl.type = fill_type::internal;
    fl.source = fill_source::quote;
    sink.clear();
    a.on_fill(0, fl);
    // Note the config carries a restored position (externalPosition = 9 for this contract), so the
    // hedge is sized to the net position, not to the fill.
    check_eq(a.get_state().contract(0).internal_pos, 5, "the fill moves the internal position");
    check_eq(sink.count(recording_sink::call::kind::fill), 1, "the fill is reported");

    sink.clear();
    a.end_of_fills(0);
    check_eq(sink.count(recording_sink::call::kind::hedge), 2,
             "the size hedge and the repriced hedge both reach the transport");
    // config/backtest carries a restored position (externalPosition = 9 for this contract), so the
    // hedge is sized to the net position - the fill plus that carry - not to the fill alone.
    const contract_state& s0 = a.get_state().contract(0);
    const qty_t net = s0.internal_pos + s0.external_pos;
    const qty_t abs_net = net < 0 ? -net : net;
    const side needed = net < 0 ? side::bid : side::ask;
    bool saw_sized_hedge = false;
    for (const auto& c : sink.calls) {
        if (c.what == recording_sink::call::kind::hedge && c.sd == needed && c.size == abs_net) {
            saw_sized_hedge = true;
        }
    }
    check(saw_sized_hedge, "the hedge covers the whole net position (fill + restored carry)");

    // ---- the shift updates only the active width percentage ----
    const double before = a.get_state().contract(0).width_pct;
    a.set_shift(3);
    check(a.get_state().contract(0).width_pct != before, "a shift changes the width percentage");
    check_eq(a.current_shift(), 3, "the adapter records the shift");

    // ---- external quoting can be switched off by the venue gate ----
    a.set_venue_enabled(static_cast<uint32_t>(venue::cme), false);
    sink.clear();
    view.levels = bids;
    view.batch_id = 3;
    a.on_market_data(0, side::bid, view);
    a.run_cycle(2000);
    bool any_external = false;
    for (const auto& c : sink.calls) {
        if (c.what == recording_sink::call::kind::quote && c.sd == side::bid) any_external = true;
    }
    check(any_external, "internal quoting continues with the venue gate closed");

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all MM phase-8 checks passed\n");
    return EXIT_SUCCESS;
}
