// Cross-thread stress for the mailbox (PLAN.md D-EX4).
//
//   exec_spsc_stress [requests]
//
// One producer thread, one consumer thread, no locks. The producer stamps every request with a
// strictly increasing sequence number and the consumer verifies that it sees all of them, in
// order, exactly once. This is the check that a release/acquire pairing on the published cursors
// is actually correct -- and the target for a ThreadSanitizer run:
//
//   cmake -S new_impl -B new_impl/build-tsan -DCMAKE_CXX_FLAGS=-fsanitize=thread
//   cmake --build new_impl/build-tsan --target exec_spsc_stress
//   ./new_impl/build-tsan/exec_spsc_stress 2000000

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "order_request.hpp"
#include "request_queue.hpp"
#include "types.hpp"

namespace {

using namespace tachyon::trade::exec;  // note: exec::qty_t is signed, unlike trade::qty_t (D-EX7)

constexpr uint32_t CAPACITY = 256;  // deliberately small: forces wraps and the full path

}  // namespace

int main(int argc, char** argv) {
    const uint64_t total = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
    const uint32_t capacity = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : CAPACITY;

    request_queue<sync_mode::spsc> queue(capacity);

    std::atomic<bool> producer_done{false};
    std::atomic<uint64_t> pushed{0};
    std::atomic<uint64_t> dropped{0};
    uint64_t received = 0;
    uint64_t order_violations = 0;
    uint64_t expected = 0;
    bool overflow_seen = false;

    std::thread producer([&] {
        for (uint64_t i = 0; i < total;) {
            order_request* slot = queue.reserve();
            if (slot == nullptr) {
                ++dropped;
                continue;  // spin: the consumer is behind, which is allowed
            }
            *slot = make_quote(strategy_id::MM, static_cast<uint32_t>(i % 16), side::bid,
                               25'000'000, 1, 25'000'000, 1, true, false, 0);
            slot->seq = static_cast<uint32_t>(i);
            queue.publish();
            ++pushed;
            ++i;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        for (;;) {
            const uint32_t n = queue.drain([&](const order_request& r) {
                // Requests are pushed in a single producer order, so the consumer must see a
                // dense, strictly increasing sequence.
                if (r.seq != (expected & 0xFFFFFFFFu)) ++order_violations;
                ++expected;
                ++received;
            });
            if (queue.dropped() > 0) overflow_seen = true;
            if (n == 0 && producer_done.load(std::memory_order_acquire) && queue.empty()) break;
        }
    });

    producer.join();
    consumer.join();

    std::printf("exec spsc stress: %llu requests, capacity %u\n", (unsigned long long)total,
                capacity);
    std::printf("  pushed            %llu\n", (unsigned long long)pushed.load());
    std::printf("  received          %llu\n", (unsigned long long)received);
    std::printf("  producer spins    %llu\n", (unsigned long long)dropped.load());
    std::printf("  order violations  %llu\n", (unsigned long long)order_violations);
    std::printf("  full-path hits    %s\n", overflow_seen ? "yes" : "no");

    bool ok = true;
    if (received != total) {
        std::printf("FAIL: received %llu of %llu\n", (unsigned long long)received,
                    (unsigned long long)total);
        ok = false;
    }
    if (order_violations != 0) {
        std::printf("FAIL: %llu ordering violations\n", (unsigned long long)order_violations);
        ok = false;
    }
    if (!overflow_seen) {
        // Not a failure, but it means the full path was never exercised in this run.
        std::printf("note: the ring never filled; raise the request count or lower capacity\n");
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
