#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

#include "limits.hpp"
#include "order_request.hpp"

// The mailbox: one bounded ring per strategy, producer = the strategy, consumer = the executor.
//
// Semantics, in one line: `reserve()` hands out a slot, `publish()` makes it visible, and
// `drain()` visits every published entry in producer order and advances the consumer cursor.
//
// Two synchronisation modes, chosen at compile time (PLAN.md D-EX4):
//   sync_mode::single -- everything runs on one thread (live single-thread loop, backtest,
//                        replay). No atomics, no fences: the cursors are plain words, so a
//                        push is a 64-byte store plus two increments.
//   sync_mode::spsc   -- one producer thread and one consumer thread (one thread per strategy).
//                        Cursors are cache-line separated atomics, publish is a release store,
//                        drain reads with an acquire load. Still wait-free for the producer.
//
// The consumer never removes entries, it only advances `tail`, so a drain is a pure read walk
// and cannot allocate, lock, or block. When a producer finds the ring full it drops the request
// and counts it (`dropped()`): back-pressure is a cold path the caller must treat as a fault
// (the executor alerts), exactly like the legacy ring's overflow.
namespace tachyon::trade::exec {

enum class sync_mode : uint8_t { single, spsc };

namespace detail {

// The published cursor. Plain in single-threaded mode, atomic in spsc mode.
template <sync_mode Mode>
struct cursor {
    uint64_t value = 0;
};

template <>
struct cursor<sync_mode::spsc> {
    std::atomic<uint64_t> value{0};
};

}  // namespace detail

// The ring is a template on its payload so both directions share one implementation (and one set of
// tests): strategy -> engine carries `order_request`, engine -> strategy carries `feedback_event`.
template <class T, sync_mode Mode = sync_mode::single>
class spsc_ring {
public:
    static constexpr bool concurrent = Mode == sync_mode::spsc;

    spsc_ring() noexcept = default;
    explicit spsc_ring(uint32_t capacity) { init(capacity); }

    ~spsc_ring() { delete[] slots_; }

    spsc_ring(const spsc_ring&) = delete;
    spsc_ring& operator=(const spsc_ring&) = delete;
    spsc_ring(spsc_ring&&) = delete;
    spsc_ring& operator=(spsc_ring&&) = delete;

    // One allocation for the life of the process. `capacity` must be a power of two.
    void init(uint32_t capacity) {
        assert(capacity && !(capacity & (capacity - 1)) && "capacity must be a power of two");
        assert(capacity >= 2);
        assert(slots_ == nullptr && "already initialised");
        slots_ = new T[capacity]();
        capacity_ = capacity;
        mask_ = capacity - 1;
    }

    // ---- inspection -------------------------------------------------------
    uint32_t capacity() const noexcept { return capacity_; }
    uint64_t pushed() const noexcept { return producer_.pushed; }
    uint64_t drained() const noexcept { return consumer_.drained; }
    uint64_t dropped() const noexcept { return producer_.dropped; }
    // Number of entries published but not yet drained.
    uint64_t size() const noexcept { return load_head() - load_tail(); }
    bool empty() const noexcept { return load_head() == load_tail(); }

    uint64_t head() const noexcept { return load_head(); }
    uint64_t tail() const noexcept { return load_tail(); }

    // ---- producer side ----------------------------------------------------
    // A slot to fill in place, or nullptr when the ring is full (cold path, counted).
    T* reserve() noexcept {
        const uint64_t tail = load_tail();
        if (producer_.local - tail >= capacity_) {
            ++producer_.dropped;
            return nullptr;
        }
        return &slots_[producer_.local & mask_];
    }

    // Makes the slot handed out by the previous reserve() visible to the consumer.
    void publish() noexcept {
        const uint64_t next = producer_.local + 1;
        if constexpr (concurrent) {
            producer_.published.value.store(next, std::memory_order_release);
        } else {
            producer_.published.value = next;
        }
        producer_.local = next;
        ++producer_.pushed;
    }

    bool try_push(const T& request) noexcept {
        T* slot = reserve();
        if (slot == nullptr) return false;
        *slot = request;
        publish();
        return true;
    }

    // ---- consumer side ----------------------------------------------------
    // Visits every published entry in producer order and returns how many were visited.
    // `visit(const order_request&)` must not re-enter the queue.
    template <class Visitor>
    uint32_t drain(Visitor&& visit) noexcept {
        const uint64_t head = load_head();
        uint64_t tail = consumer_.local;
        if (tail == head) return 0;

        const uint32_t visited = static_cast<uint32_t>(head - tail);
        do {
            visit(static_cast<const T&>(slots_[tail & mask_]));
            ++tail;
        } while (tail != head);

        consumer_.local = tail;
        if constexpr (concurrent) {
            consumer_.published.value.store(tail, std::memory_order_relaxed);
        } else {
            consumer_.published.value = tail;
        }
        consumer_.drained += visited;
        return visited;
    }

    // Random access by absolute cursor (tests and traces): entry `i` is the i-th request ever
    // published. Entries at or past head() have not been published yet.
    const T& at(uint64_t cursor) const noexcept { return slots_[cursor & mask_]; }

private:
    struct alignas(CACHE_LINE) producer_side {
        detail::cursor<Mode> published;  // written here, read by the consumer
        uint64_t local = 0;              // producer-private copy of `published`
        uint64_t pushed = 0;
        uint64_t dropped = 0;
    };

    struct alignas(CACHE_LINE) consumer_side {
        detail::cursor<Mode> published;  // written here, read by the producer
        uint64_t local = 0;              // consumer-private copy of `published`
        uint64_t drained = 0;
    };

    // The producer reads the consumer's cursor relaxed: a stale value only makes it believe the
    // ring is fuller than it is, never emptier.
    uint64_t load_tail() const noexcept {
        if constexpr (concurrent) return consumer_.published.value.load(std::memory_order_relaxed);
        return consumer_.published.value;
    }

    // The consumer must observe the entry writes that happen-before the producer's release store.
    uint64_t load_head() const noexcept {
        if constexpr (concurrent) return producer_.published.value.load(std::memory_order_acquire);
        return producer_.published.value;
    }

    producer_side producer_{};
    consumer_side consumer_{};
    T* slots_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
};

// The strategy -> engine mailbox, which is what the tests and the executor name.
template <sync_mode Mode = sync_mode::single>
using request_queue = spsc_ring<order_request, Mode>;

}  // namespace tachyon::trade::exec
