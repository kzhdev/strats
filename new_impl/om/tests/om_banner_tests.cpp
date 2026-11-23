// Phase-7 gate: the external-quote gates (om/banners.hpp + om/bannered_gateway.hpp).
//
//   om_banner_tests [cycles]
//
// The gates are the part of external order management that decides whether an intent may leave. They
// are transcribed from the legacy engine, so each case below is one legacy branch, named after it:
//
//   * first quote in the market          IME:687-697  -> ban opened
//   * improvement inside an open ban     IME:656-678  -> dropped ("/not updating")
//   * pull inside an open ban            IME:656-678  -> forwarded, banner refreshed in place
//   * ban expired, improvement           IME:685-698  -> forwarded, ban re-opened
//   * outside MaxTicksOffTopLevel        IME:619-645  -> size zeroed, no banner motion
//   * hedge pay-up / ban / worsening     IME:569-607  -> bypass / filter / open ban
//   * cancel                             IME:559-561  -> resting quote dropped
//
// The last case is the one that matters for the cutover: with the gates configured off (zero ban
// periods, no max-off-top-level) the decorated gateway must be a byte-for-byte pass-through, so the
// OM layer can be introduced without changing a single emitted order and then enabled per contract.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bannered_gateway.hpp"
#include "executor.hpp"
#include "ports.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;
using namespace tachyon::trade::om;

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
constexpr uint64_t BAN = 1'000;

struct clock_ctx {
    uint64_t now = 0;
};
uint64_t read_clock(void* ctx) noexcept { return static_cast<clock_ctx*>(ctx)->now; }

// A gateway + the decorator around it, wired for one contract, with the clock in the test's hands.
struct rig {
    explicit rig(const gate_config& cfg, uint32_t contracts = 1) : gated(next, read_clock, &clock) {
        gated.configure(contracts);
        for (uint32_t c = 0; c < contracts; ++c) gated.set_config(c, cfg);
    }

    void quote(uint32_t c, side sd, price_t px, qty_t size) {
        exchange_order o;
        o.contract = c;
        o.strategy = strategy_id::MM;
        o.sd = sd;
        o.price = px;
        o.size = size;
        gated.send_quote(o);
    }
    void hedge(uint32_t c, side sd, price_t px, qty_t size, bool pay_up = false) {
        exchange_order o;
        o.contract = c;
        o.strategy = strategy_id::RV;
        o.sd = sd;
        o.price = px;
        o.size = size;
        o.hedge = true;
        o.pay_up = pay_up;
        gated.send_hedge(o);
    }

    recording_gateway next;
    clock_ctx clock;
    bannered_gateway gated;
};

// ---------------------------------------------------------------------------
// 1-4. the quote ban
// ---------------------------------------------------------------------------
void test_quote_ban() {
    std::printf("om: quote banner\n");

    gate_config cfg;
    cfg.quote_ban_period = BAN;
    rig r(cfg);

    // :687-697 -- no quote resting yet: forward it and open a ban.
    r.clock.now = 1'000;
    r.quote(0, side::bid, PX(100), 5);
    check_eq(static_cast<long long>(r.next.quotes.size()), 1, "first quote goes out");
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].price), PX(100),
             "the ban records the quote price");
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].expiry), 2'000,
             "expiry = now + ban period (IME:708)");

    // :656-678 -- inside the ban, an improvement is dropped.
    r.clock.now = 1'500;
    r.quote(0, side::bid, PX(101), 5);
    check_eq(static_cast<long long>(r.next.quotes.size()), 1, "an improvement inside the ban is dropped");
    check_eq(static_cast<long long>(r.gated.counters().quotes_suppressed), 1, "and counted as suppressed");
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].price), PX(100),
             "the dropped quote does not move the banner");

    // :656-678 -- a same-price size increase is also an improvement, so also dropped.
    r.quote(0, side::bid, PX(100), 6);
    check_eq(static_cast<long long>(r.next.quotes.size()), 1, "a same-price size increase is dropped too");

    // :663-670 -- a non-improving quote gets through and only refreshes the recorded quote.
    r.clock.now = 1'800;
    r.quote(0, side::bid, PX(100), 2);
    check_eq(static_cast<long long>(r.next.quotes.size()), 2, "a pull inside the ban gets through");
    check_eq(static_cast<long long>(r.next.quotes.back().size), 2, "with its own size");
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].expiry), 2'000,
             "refreshing does not extend the window");

    // :685-698 -- once expired, an improvement goes out and re-opens the ban.
    r.clock.now = 2'000;
    r.quote(0, side::bid, PX(102), 5);
    check_eq(static_cast<long long>(r.next.quotes.size()), 3, "after expiry the improvement goes out");
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].expiry), 3'000, "and opens a new ban");

    // The two sides are independent (legacy _quoteBanner[Side]).
    r.quote(0, side::ask, PX(200), 5);
    check_eq(static_cast<long long>(r.next.quotes.size()), 4, "the ask side has its own banner");
}

// ---------------------------------------------------------------------------
// 5. max ticks off top level
// ---------------------------------------------------------------------------
void test_max_ticks_off_top_level() {
    std::printf("om: max ticks off top level\n");

    gate_config cfg;
    cfg.max_ticks_off_top_level = 2 * 25'000'000;  // two ticks, in price units
    rig r(cfg);
    r.gated.set_market_top(0, side::bid, market_top{PX(103), 5});
    r.gated.set_market_top(0, side::ask, market_top{PX(105), 5});

    // IME:624 -- bid more than max ticks away from the top bid: keep the price, zero the size.
    r.quote(0, side::bid, PX(100), 7);
    check_eq(static_cast<long long>(r.next.quotes.size()), 1, "a degraded quote still goes out");
    check_eq(static_cast<long long>(r.next.quotes.back().size), 0, "with size zeroed (IME:625)");
    check_eq(static_cast<long long>(r.next.quotes.back().price), PX(100), "and the price kept");
    check_eq(static_cast<long long>(r.gated.counters().quotes_degraded), 1, "counted as degraded");
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].expiry), 0,
             "a degraded quote opens no ban (IME:662)");

    // Exactly at the limit is inside: the check is strict (<).
    r.quote(0, side::bid, PX(101), 7);
    check_eq(static_cast<long long>(r.next.quotes.back().size), 7, "at the limit the size is kept");

    // IME:629 -- the ask mirror.
    r.quote(0, side::ask, PX(108), 4);
    check_eq(static_cast<long long>(r.next.quotes.back().size), 0, "the ask mirror degrades too");
    r.quote(0, side::ask, PX(106), 4);
    check_eq(static_cast<long long>(r.next.quotes.back().size), 4, "and keeps the size inside the limit");

    // IME:624 -- no top (NULL_PRICE) means no check.
    gate_config off;
    rig r2(off);
    r2.quote(0, side::bid, PX(1), 5);
    check_eq(static_cast<long long>(r2.next.quotes.back().size), 5, "an unknown top disables the check");
}

// ---------------------------------------------------------------------------
// 6. the hedge ban
// ---------------------------------------------------------------------------
void test_hedge_ban() {
    std::printf("om: hedge banner\n");

    gate_config cfg;
    cfg.hedge_ban_period = BAN;
    rig r(cfg);

    // IME:601-604 -- no resting hedge: forward and open a ban.
    r.clock.now = 1'000;
    r.hedge(0, side::bid, PX(100), 5);
    check_eq(static_cast<long long>(r.next.hedges.size()), 1, "first hedge goes out");
    check_eq(static_cast<long long>(r.gated.state(0).hedge_ban[0].expiry), 2'000, "and opens a ban");

    // IME:580-588 -- under the ban a worse hedge is filtered, a non-worse one passes.
    r.clock.now = 1'500;
    r.hedge(0, side::bid, PX(99), 5);
    check_eq(static_cast<long long>(r.next.hedges.size()), 1, "a worse hedge under the ban is suppressed");
    check_eq(static_cast<long long>(r.gated.counters().hedges_suppressed), 1, "and counted");
    r.hedge(0, side::bid, PX(101), 5);
    check_eq(static_cast<long long>(r.next.hedges.size()), 2, "a better hedge under the ban passes");

    // IME:581 -- a pull passes even when the banner records size, and updates in place.
    r.hedge(0, side::bid, PX(98), 0);
    check_eq(static_cast<long long>(r.next.hedges.size()), 3, "a hedge pull passes (IME:581)");
    check_eq(static_cast<long long>(r.gated.state(0).hedge_ban[0].expiry), 2'000,
             "and does not extend the window");

    // IME:572-576 -- pay-up bypasses the ban entirely.
    rig p(cfg);
    p.clock.now = 1'000;
    p.hedge(0, side::bid, PX(100), 5);
    p.clock.now = 1'100;
    p.hedge(0, side::bid, PX(90), 5, true);
    check_eq(static_cast<long long>(p.next.hedges.size()), 2, "a pay-up bypasses the ban");
    check_eq(static_cast<long long>(p.gated.state(0).hedge_ban[0].price), PX(90),
             "and records its own price (IME:575)");

    // IME:593-597 -- not banned, and the new hedge is worse than the resting one: open a ban.
    rig q(cfg);
    q.clock.now = 1'000;
    q.hedge(0, side::bid, PX(100), 5);
    q.clock.now = 2'000;
    q.hedge(0, side::bid, PX(99), 5);
    check_eq(static_cast<long long>(q.next.hedges.size()), 2, "the worse hedge still goes out");
    check_eq(static_cast<long long>(q.gated.state(0).hedge_ban[0].expiry), 3'000,
             "and opens a ban (IME:597)");
}

// ---------------------------------------------------------------------------
// 7. cancel drops the resting quote
// ---------------------------------------------------------------------------
void test_cancel_clears_resting() {
    std::printf("om: cancel clears the resting quote\n");

    gate_config cfg;
    cfg.quote_ban_period = BAN;
    rig r(cfg);
    r.clock.now = 1'000;
    r.quote(0, side::bid, PX(100), 5);
    r.clock.now = 2'000;  // ban expired
    r.quote(0, side::bid, PX(100), 5);
    // IME:685 -- not an improvement over what rests, so the banner is expired and not re-opened.
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].expiry), 0,
             "a same-price same-size quote is no improvement: the banner expires, unarmed");

    r.gated.cancel(0, strategy_id::MM, false);
    check_eq(static_cast<long long>(r.gated.state(0).resting_quote_price[0]), NULL_PRICE,
             "cancel drops the resting price (IME:559-561)");
    r.quote(0, side::bid, PX(100), 5);
    check_eq(static_cast<long long>(r.gated.state(0).quote_ban[0].expiry), 3'000,
             "so the next quote is a first quote again");
}

// ---------------------------------------------------------------------------
// 8. the gates configured off are a byte-for-byte pass-through
// ---------------------------------------------------------------------------
void tape(executor& exec, uint32_t cycles) {
    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        const int64_t drift = cycle % 3;
        for (uint32_t c = 0; c < 2; ++c) {
            exec.submit(strategy_id::MM, make_quote(strategy_id::MM, c, side::bid, PX(100 + drift), 5,
                                                    PX(100 + drift), 5, false));
            exec.submit(strategy_id::MM, make_quote(strategy_id::MM, c, side::ask, PX(104 + drift), 5,
                                                    PX(104 + drift), 5, false));
        }
        exec.run_cycle(cycle);
    }
}

void test_disabled_gate_is_transparent(uint32_t cycles) {
    std::printf("om: a disabled gate forwards every intent unchanged\n");

    executor_config config;
    config.contracts = 2;
    config.queue_capacity = 512;

    recording_gateway plain;
    executor bare;
    bare.init(config, &plain);
    tape(bare, cycles);

    clock_ctx clock;
    gate_config off;  // ban periods 0, max_ticks_off_top_level -1: everything disabled
    recording_gateway behind;
    bannered_gateway gated(behind, read_clock, &clock);
    gated.configure(config.contracts);
    for (uint32_t c = 0; c < config.contracts; ++c) gated.set_config(c, off);
    executor decorated;
    decorated.init(config, &gated);
    tape(decorated, cycles);

    check(!plain.quotes.empty(), "the tape emitted quotes at all");
    check_eq(static_cast<long long>(behind.quotes.size()),
             static_cast<long long>(plain.quotes.size()), "the same number of quotes reached the venue");
    bool identical = behind.quotes.size() == plain.quotes.size();
    for (size_t i = 0; identical && i < plain.quotes.size(); ++i) {
        identical = plain.quotes[i].contract == behind.quotes[i].contract &&
                    plain.quotes[i].sd == behind.quotes[i].sd &&
                    plain.quotes[i].price == behind.quotes[i].price &&
                    plain.quotes[i].size == behind.quotes[i].size;
    }
    check(identical, "and every one of them identically");
    check_eq(static_cast<long long>(gated.counters().quotes_suppressed), 0, "nothing suppressed");
    check_eq(static_cast<long long>(gated.counters().quotes_degraded), 0, "nothing degraded");
    std::printf("  %-30s %8zu quotes\n", "tape length", plain.quotes.size());
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t cycles = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 200;

    test_quote_ban();
    test_max_ticks_off_top_level();
    test_hedge_ban();
    test_cancel_clears_resting();
    test_disabled_gate_is_transparent(cycles);

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-7 checks passed\n");
    return EXIT_SUCCESS;
}
