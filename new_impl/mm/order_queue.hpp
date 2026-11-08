#pragma once

#include <array>
#include <cstdint>

#include "limits.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// Resting-order book for one contract: the replacement for the legacy
//
//   std::unordered_map<uint32_t, OrderInfo>              _orders;
//   std::unordered_map<price, std::vector<OrderInfo*>>   _priceOrders[2];
//
// (SimpleMarketMaker.h:1745-1746). Those two containers allocate on every order and
// queue-position update, chase pointers through the heap, and are searched with `find()` inside
// the quote-price path (:796).
//
// Three preallocated structures replace them:
//   slots_   flat arena of resting orders, recycled through an intrusive free list
//   ids_     open-addressed orderId -> slot index with backward-shift deletion (no tombstones,
//            so lookups never degrade and nothing is ever rehashed)
//   levels_  per side, a *packed* list of live price levels with head/tail/count; in practice
//            one to four entries (the quote level, the hedge level, spray levels), so a linear
//            scan is faster than hashing and has no deletion edge cases
//
// The per-price list preserves insertion order because the legacy code depends on it: the
// quoting engine walks the orders at a price in that order and then inspects `orders.back()`
// (:814-851).
class order_queue {
public:
    static constexpr uint32_t NIL = 0xFFFFFFFFu;
    // om::strategy_order_t::queue_pos_t::NOT_SET (neorhone/order_mgr/StrategyOrder.h:74), which
    // the strategy uses as "the exchange has not told us our queue position yet" (:816).
    static constexpr qty_t NOT_SET = -1;

    struct slot {
        price_t price = tachyon::NULL_PRICE;
        qty_t qty = 0;
        qty_t qp = NOT_SET;
        uint32_t order_id = 0;
        // Next order at the same price, in insertion order. Also reused as the free-list link
        // while the slot is unused.
        uint32_t next = NIL;
        uint32_t prev = NIL;
        uint8_t side = 0;
        bool used = false;
    };

    void init() noexcept;

    // A freshly constructed queue is usable: the arena and the id table are initialised in the
    // constructor so a forgotten init() cannot silently reject every order.
    order_queue() noexcept { init(); }

    // Legacy onOrderUpdate (:1562-1609), branch for branch:
    //   unknown id            -> insert and append to its price list
    //   price changed, q>0    -> move to the end of the new price list
    //   price changed, q==0   -> filled: erase
    //   same price, q==0      -> cancelled or filled: erase
    //   same price, q>0       -> update qty and queue position only (list position unchanged)
    // Returns false only when a cap is exhausted (cold path: the caller disables the contract
    // and raises an alert — never an allocation).
    bool upsert(uint32_t order_id, price_t price, qty_t qty, qty_t qp, side s) noexcept;

    // Legacy onQueuePositionUpdate (:1611-1642). A queue position of NOT_SET (-1) is preserved;
    // anything below it is clamped to 0, exactly as the legacy comparison does.
    bool set_queue_position(uint32_t order_id, qty_t qp) noexcept;

    const slot* find(uint32_t order_id) const noexcept;
    // First and last order at a price (legacy `orders.front()` / `orders.back()`); nullptr when
    // the price has no resting orders, which is the case legacy treats as "no orders placed
    // here yet" (:797).
    const slot* first(side s, price_t price) const noexcept;
    const slot* last(side s, price_t price) const noexcept;
    const slot* next(const slot& current) const noexcept;
    uint32_t count_at(side s, price_t price) const noexcept;

    uint32_t size() const noexcept { return size_; }
    bool full() const noexcept { return free_head_ == NIL; }
    // Set once a capacity was hit; the caller must treat the contract as untrustworthy.
    bool overflowed() const noexcept { return overflowed_; }

private:
    struct price_level {
        price_t price = tachyon::NULL_PRICE;
        uint32_t head = NIL;
        uint32_t tail = NIL;
        uint32_t count = 0;
    };

    static constexpr uint32_t ID_CAPACITY = 1024;  // power of two, >= 2 * MAX_ORDERS

    static uint32_t id_home(uint32_t order_id) noexcept {
        return (order_id * 2654435761u) & (ID_CAPACITY - 1);  // Knuth multiplicative
    }

    uint32_t alloc_slot() noexcept;
    void release_slot(uint32_t index) noexcept;

    void id_insert(uint32_t order_id, uint32_t slot_index) noexcept;
    void id_erase(uint32_t order_id) noexcept;
    uint32_t id_lookup(uint32_t order_id) const noexcept;  // NIL when absent

    int32_t level_find(side s, price_t price) const noexcept;   // index or -1
    uint32_t level_ensure(side s, price_t price) noexcept;      // index or NIL when full
    void level_drop_empty(side s, uint32_t level_index) noexcept;
    void list_append(side s, uint32_t level_index, uint32_t slot_index) noexcept;
    void list_remove(side s, uint32_t level_index, uint32_t slot_index) noexcept;

    std::array<slot, MAX_ORDERS> slots_{};
    std::array<uint32_t, ID_CAPACITY> ids_{};  // slot index + 1; 0 == empty
    std::array<std::array<price_level, MAX_LEVEL_SLOTS>, SIDE_COUNT> levels_{};
    std::array<uint32_t, SIDE_COUNT> level_count_{};
    uint32_t free_head_ = NIL;
    uint32_t size_ = 0;
    bool overflowed_ = false;
};

}  // namespace tachyon::trade::mm
