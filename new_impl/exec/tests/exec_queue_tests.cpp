// Phase-0 gate for the new executor: the vocabulary, the request POD and the mailbox.
//
//   exec_queue_tests [iterations]
//
// Pinned to the legacy request plumbing:
//   * the (strategy, kind) collapse table comes from InternalMatchingEngine.h:1952-1971;
//   * reserve/publish/drain mirrors OrderRequestManager's reserve/publish ring
//     (RingBufferQueue.h:67-84) with one difference that is the point of the rewrite: the
//     consumer walks a *published prefix* instead of a per-key cursor the engine has to keep;
//   * the request payload is the union of OrderRequest::Quote and OrderRequest::Hedge
//     (Common.h:1151-1189).
//
// Both synchronisation modes are exercised by the same cases, so sync::spsc can never drift
// from sync::single in behaviour.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "limits.hpp"
#include "order_request.hpp"
#include "request_queue.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;  // note: exec::qty_t is signed, unlike trade::qty_t (D-EX7)

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

constexpr price_t PX(int64_t ticks) { return 25'000'000 * ticks; }  // 0.025 at 1e-9

order_request quote(uint32_t contract, side sd, int64_t ticks, qty_t size, uint32_t seq) {
    order_request r = make_quote(strategy_id::MM, contract, sd, PX(ticks), size, PX(ticks), size,
                                 true, false, 0);
    r.seq = seq;
    return r;
}

// ---------------------------------------------------------------------------
// The request POD: one cache line, and the builders set exactly the legacy fields.
// ---------------------------------------------------------------------------
void test_request_layout() {
    std::printf("request: layout and builders\n");

    check_eq(sizeof(order_request), CACHE_LINE, "order_request is one cache line");
    check_eq(offsetof(order_request, internal_price), 0, "internal_price first");
    check_eq(offsetof(order_request, flags), 63, "flags last");

    const order_request q =
        make_quote(strategy_id::RV, 7, side::ask, PX(100), 5, PX(101), 6, true, true, 42);
    check(q.strategy_id_value() == strategy_id::RV, "quote strategy");
    check(q.kind_value() == request_kind::quote, "quote kind");
    check(q.side_value() == side::ask, "quote side");
    check_eq(q.internal_price, PX(100), "quote internal price");
    check_eq(q.external_size, 6, "quote external size");
    check_eq(q.exec_token, 42, "quote exec token");
    check(q.persist(), "quote persist flag");
    check(q.lean_on_rv(), "quote leanOnRV flag");
    check(!q.pay_up() && !q.external(), "quote has no hedge flags");
    check(q.rests(), "MM/RV quotes rest internally");

    const order_request h =
        make_hedge(strategy_id::RV, 3, side::bid, PX(50), PX(49), 11, true, true, 9, 4, 77);
    check(h.kind_value() == request_kind::hedge, "hedge kind");
    check_eq(h.hedge_size, 11, "hedge size");
    check_eq(h.hedge_id, 9, "hedge id");
    check_eq(h.market_id, 4, "hedge market id");
    check_eq(h.exec_token, 77, "hedge exec token");
    check(h.external(), "hedge external flag");
    check(h.pay_up(), "hedge payUp flag");
    check(h.persist(), "hedges are persistent");
    check(!h.rests(), "RV hedge never rests (FAK)");

    // The key is the address: distinct keys must have distinct slots, and the packing must stay
    // inside SLOTS_PER_CONTRACT.
    bool seen[SLOTS_PER_CONTRACT] = {};
    for (uint32_t s = 0; s < to_int(strategy_id::__COUNT__); ++s) {
        for (uint32_t k = 0; k < KIND_COUNT; ++k) {
            for (uint32_t d = 0; d < SIDE_COUNT; ++d) {
                const uint32_t slot = slot_of(static_cast<strategy_id>(s),
                                              static_cast<request_kind>(k), static_cast<side>(d));
                check(slot < SLOTS_PER_CONTRACT, "slot inside the contract key space");
                check(!seen[slot], "slot reached only once");
                seen[slot] = true;
            }
        }
    }
}


// ---------------------------------------------------------------------------
// The collapse table -- transcribed from InternalMatchingEngine.h:1952-1971.
// ---------------------------------------------------------------------------
void test_traits_table() {
    std::printf("request: per-key behaviour table\n");

    // collapse-to-last: MM hedge (legacy order id 0), MM quote (1), RV quote (3)
    check(is_last_wins(strategy_id::MM, request_kind::hedge), "MM hedge collapses");
    check(is_last_wins(strategy_id::MM, request_kind::quote), "MM quote collapses");
    check(is_last_wins(strategy_id::RV, request_kind::quote), "RV quote collapses");

    // one at a time: RV hedge (2), AS hedge (4), AS quote (5)
    check(!is_last_wins(strategy_id::RV, request_kind::hedge), "RV hedge is FIFO");
    check(!is_last_wins(strategy_id::AS, request_kind::hedge), "AS hedge is FIFO");
    check(!is_last_wins(strategy_id::AS, request_kind::quote),
          "AS quote is FIFO (aggressive quote adjustment)");

    // resting vs FAK
    check(rests_internally(strategy_id::MM, request_kind::quote), "MM quote rests");
    check(rests_internally(strategy_id::MM, request_kind::hedge), "MM hedge rests");
    check(rests_internally(strategy_id::RV, request_kind::quote), "RV quote rests");
    check(rests_internally(strategy_id::AS, request_kind::quote), "AS quote rests");
    check(!rests_internally(strategy_id::RV, request_kind::hedge), "RV hedge is FAK");
    check(!rests_internally(strategy_id::AS, request_kind::hedge), "AS hedge is FAK");

    // FAK streams cross immediately; resting streams are staged for the contract's cross.
    check(traits_of(strategy_id::RV, request_kind::hedge).mode == apply_mode::cross_now,
          "RV hedge crosses now");
    check(traits_of(strategy_id::AS, request_kind::hedge).mode == apply_mode::cross_now,
          "AS hedge crosses now");
    check(traits_of(strategy_id::AS, request_kind::quote).mode == apply_mode::rest,
          "AS quote is staged");

    // published sources (common/types.hpp liquidity_source)
    check(traits_of(strategy_id::MM, request_kind::quote).source ==
              liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE,
          "MM quote source");
    check(traits_of(strategy_id::MM, request_kind::hedge).source ==
              liquidity_source::LIQUIDITY_SOURCE_MM_HEDGE,
          "MM hedge source");
    check(traits_of(strategy_id::RV, request_kind::quote).source ==
              liquidity_source::LIQUIDITY_SOURCE_RV_QUOTE,
          "RV quote source");
    check(traits_of(strategy_id::AS, request_kind::quote).source ==
              liquidity_source::LIQUIDITY_SOURCE_AS_QUOTE,
          "AS quote source");
    check(traits_of(strategy_id::RV, request_kind::hedge).source ==
              liquidity_source::__LIQUIDITY_SOURCE_END__,
          "RV hedge publishes no source");

    check_eq(RESTING_SLOTS, 6, "six resting streams per contract");
    check_eq(MAX_INTERNAL_ORDERS, 6, "so at most six internal orders per side");
}


// ---------------------------------------------------------------------------
// Mailbox semantics, once per synchronisation mode.
// ---------------------------------------------------------------------------
template <sync_mode Mode>
void test_queue_fifo(const char* label) {
    std::printf("queue[%s]: FIFO across wraps\n", label);

    request_queue<Mode> q(64);
    check_eq(q.capacity(), 64, "capacity reported");
    check(q.empty(), "fresh queue is empty");

    // Push/drain far past the capacity so the ring wraps many times.
    uint32_t seq = 0;
    uint32_t drained = 0;
    bool order_ok = true;
    for (int round = 0; round < 100; ++round) {
        for (uint32_t i = 0; i < 37; ++i) {  // 37 is not a divisor of 64: every wrap differs
            check(q.try_push(quote(i % 4, side::bid, 100, 3, seq)) || false,
                  "push below capacity succeeds");
            ++seq;
        }
        q.drain([&](const order_request& r) {
            order_ok &= (r.seq == drained);
            ++drained;
        });
    }
    check(order_ok, "drain visits requests in producer order");
    check_eq(drained, 3700, "nothing lost");
    check_eq(q.dropped(), 0, "no drops when the consumer keeps up");
    check(q.empty(), "queue empty after the last drain");
    check_eq(static_cast<long long>(q.drained()), 3700, "drained counter");
}

template <sync_mode Mode>
void test_queue_backpressure(const char* label) {
    std::printf("queue[%s]: back-pressure and recovery\n", label);

    request_queue<Mode> q(8);
    for (uint32_t i = 0; i < 8; ++i) {
        check(q.try_push(quote(0, side::ask, 100, 1, i)), "push to capacity");
    }
    check_eq(q.size(), 8, "ring is full");
    check(q.reserve() == nullptr, "reserve returns nullptr when full");
    check_eq(q.dropped(), 1, "a full ring counts the drop");
    check(!q.try_push(quote(0, side::ask, 100, 1, 99)), "try_push fails when full");
    check_eq(q.dropped(), 2, "every refused request is counted exactly once");

    uint32_t drained = 0;
    q.drain([&](const order_request&) { ++drained; });
    check_eq(drained, 8, "draining the whole ring");
    check_eq(q.dropped(), 2, "drops are not invented by draining");

    // Recovery: the ring accepts exactly `capacity` fresh entries again.
    for (uint32_t i = 0; i < 8; ++i) {
        check(q.try_push(quote(0, side::ask, 100, 1, 100 + i)), "push after a drain");
    }
    check_eq(q.size(), 8, "full again");
}

template <sync_mode Mode>
void test_queue_in_place(const char* label) {
    std::printf("queue[%s]: in-place reserve, partial drains\n", label);

    request_queue<Mode> q(4);
    order_request* slot = q.reserve();
    check(slot != nullptr, "reserve hands out a slot");
    *slot = quote(2, side::bid, 105, 7, 1);
    q.publish();
    check_eq(q.size(), 1, "published entry is visible");

    // A second batch, drained away from the first: the consumer keeps its place.
    q.try_push(quote(2, side::bid, 106, 8, 2));
    uint32_t seen = 0;
    const uint32_t n = q.drain([&](const order_request& r) {
        check_eq(r.seq, ++seen, "entries arrive in order");
        check_eq(r.contract, 2, "contract survives the round trip");
    });
    check_eq(n, 2, "both entries drained");

    // The slot handed out after a wrap must be reusable and fully overwritten.
    for (int i = 0; i < 6; ++i) {
        q.try_push(quote(3, side::ask, 110, 1, 10 + i));
        q.drain([](const order_request&) {});
    }
    check(q.empty(), "queue drains to empty after many wraps");
    check_eq(q.at(q.head() - 1).seq, 15, "at() sees the last published entry");
}


// ---------------------------------------------------------------------------
// The two synchronisation modes must be behaviourally indistinguishable. The spsc ring is the
// production path when each strategy has its own thread, so a divergence here would be a
// production-only defect.
// ---------------------------------------------------------------------------
void test_mode_equivalence() {
    std::printf("queue: sync::single == sync::spsc\n");

    request_queue<sync_mode::single> single(32);
    request_queue<sync_mode::spsc> spsc(32);

    bool same = true;
    uint32_t seq = 0;
    for (int round = 0; round < 50; ++round) {
        const uint32_t batch = 1 + static_cast<uint32_t>(round % 11);
        for (uint32_t i = 0; i < batch; ++i) {
            const order_request r =
                (i & 1) ? make_hedge(strategy_id::AS, seq % 5, side::ask, PX(300), PX(299), 4, true,
                                     false, seq, seq, seq)
                        : make_quote(strategy_id::RV, seq % 5, side::bid, PX(200), 2, PX(201), 2,
                                     true, false, seq);
            const bool a = single.try_push(r);
            const bool b = spsc.try_push(r);
            same &= (a == b);
            ++seq;
        }
        uint64_t a_seq = 0, b_seq = 0;
        std::vector<uint8_t> a_bytes, b_bytes;
        single.drain([&](const order_request& r) {
            ++a_seq;
            const auto* p = reinterpret_cast<const uint8_t*>(&r);
            a_bytes.insert(a_bytes.end(), p, p + sizeof(order_request));
        });
        spsc.drain([&](const order_request& r) {
            ++b_seq;
            const auto* p = reinterpret_cast<const uint8_t*>(&r);
            b_bytes.insert(b_bytes.end(), p, p + sizeof(order_request));
        });
        same &= (a_seq == b_seq) && (a_bytes == b_bytes);
    }
    check(same, "both modes produce the same accept/drop decisions and the same bytes");
    check_eq(single.dropped(), spsc.dropped(), "same drop count");
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_request_layout();
    test_traits_table();
    test_queue_fifo<sync_mode::single>("single");
    test_queue_fifo<sync_mode::spsc>("spsc");
    test_queue_backpressure<sync_mode::single>("single");
    test_queue_backpressure<sync_mode::spsc>("spsc");
    test_queue_in_place<sync_mode::single>("single");
    test_queue_in_place<sync_mode::spsc>("spsc");
    test_mode_equivalence();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all exec phase-0 checks passed\n");
    return EXIT_SUCCESS;
}
