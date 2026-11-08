#include "order_queue.hpp"

namespace tachyon::trade::mm {

void order_queue::init() noexcept {
    free_head_ = NIL;
    size_ = 0;
    overflowed_ = false;
    level_count_.fill(0);
    ids_.fill(0);
    // Chain every slot onto the free list through `next`.
    for (uint32_t i = 0; i < MAX_ORDERS; ++i) {
        slots_[i] = slot{};
        slots_[i].next = (i + 1 < MAX_ORDERS) ? (i + 1) : NIL;
    }
    free_head_ = MAX_ORDERS == 0 ? NIL : 0;
}

// ---- slot arena -------------------------------------------------------------

uint32_t order_queue::alloc_slot() noexcept {
    if (free_head_ == NIL) return NIL;
    const uint32_t index = free_head_;
    free_head_ = slots_[index].next;
    slots_[index] = slot{};
    slots_[index].used = true;
    return index;
}

void order_queue::release_slot(uint32_t index) noexcept {
    slots_[index] = slot{};
    slots_[index].next = free_head_;
    free_head_ = index;
}

// ---- orderId table (open addressing, backward-shift deletion) ----------------

void order_queue::id_insert(uint32_t order_id, uint32_t slot_index) noexcept {
    uint32_t i = id_home(order_id);
    while (ids_[i] != 0) i = (i + 1) & (ID_CAPACITY - 1);
    ids_[i] = slot_index + 1;
}

void order_queue::id_erase(uint32_t order_id) noexcept {
    uint32_t i = id_home(order_id);
    while (ids_[i] != 0 && slots_[ids_[i] - 1].order_id != order_id) {
        i = (i + 1) & (ID_CAPACITY - 1);
    }
    if (ids_[i] == 0) return;  // not present

    // Knuth's algorithm D: pull the rest of the cluster back so no tombstone is needed.
    for (;;) {
        uint32_t j = (i + 1) & (ID_CAPACITY - 1);
        if (ids_[j] == 0) {
            ids_[i] = 0;
            return;
        }
        const uint32_t home = id_home(slots_[ids_[j] - 1].order_id);
        // The entry at j may move into the hole at i when i lies on its probe path.
        const bool movable = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
        if (!movable) {
            ids_[i] = 0;
            return;
        }
        ids_[i] = ids_[j];
        i = j;
    }
}

uint32_t order_queue::id_lookup(uint32_t order_id) const noexcept {
    uint32_t i = id_home(order_id);
    while (ids_[i] != 0) {
        if (slots_[ids_[i] - 1].order_id == order_id) return ids_[i] - 1;
        i = (i + 1) & (ID_CAPACITY - 1);
    }
    return NIL;
}

// ---- per-price levels -------------------------------------------------------

int32_t order_queue::level_find(side s, price_t price) const noexcept {
    const uint32_t side_index = to_index(s);
    const uint32_t n = level_count_[side_index];
    for (uint32_t i = 0; i < n; ++i) {
        if (levels_[side_index][i].price == price) return static_cast<int32_t>(i);
    }
    return -1;
}

uint32_t order_queue::level_ensure(side s, price_t price) noexcept {
    const int32_t found = level_find(s, price);
    if (found >= 0) return static_cast<uint32_t>(found);

    const uint32_t side_index = to_index(s);
    if (level_count_[side_index] >= MAX_LEVEL_SLOTS) {
        overflowed_ = true;
        return NIL;
    }
    const uint32_t index = level_count_[side_index]++;
    levels_[side_index][index] = price_level{};
    levels_[side_index][index].price = price;
    return index;
}

void order_queue::level_drop_empty(side s, uint32_t level_index) noexcept {
    const uint32_t side_index = to_index(s);
    uint32_t& count = level_count_[side_index];
    // Swap with the last entry; the lists are linked by slot index, so their order of iteration
    // is unaffected by reordering the level table itself.
    if (level_index + 1 != count) {
        levels_[side_index][level_index] = levels_[side_index][count - 1];
    }
    --count;
}

void order_queue::list_append(side s, uint32_t level_index, uint32_t slot_index) noexcept {
    price_level& lv = levels_[to_index(s)][level_index];
    slot& s_ptr = slots_[slot_index];
    s_ptr.prev = lv.tail;
    s_ptr.next = NIL;
    if (lv.tail == NIL) {
        lv.head = slot_index;
    } else {
        slots_[lv.tail].next = slot_index;
    }
    lv.tail = slot_index;
    ++lv.count;
}

void order_queue::list_remove(side s, uint32_t level_index, uint32_t slot_index) noexcept {
    price_level& lv = levels_[to_index(s)][level_index];
    slot& s_ptr = slots_[slot_index];
    if (s_ptr.prev == NIL) {
        lv.head = s_ptr.next;
    } else {
        slots_[s_ptr.prev].next = s_ptr.next;
    }
    if (s_ptr.next == NIL) {
        lv.tail = s_ptr.prev;
    } else {
        slots_[s_ptr.next].prev = s_ptr.prev;
    }
    --lv.count;
    if (lv.count == 0) level_drop_empty(s, level_index);
}

// ---- mutation (legacy onOrderUpdate / onQueuePositionUpdate) ----------------

bool order_queue::upsert(uint32_t order_id, price_t price, qty_t qty, qty_t qp, side s) noexcept {
    const uint32_t existing = id_lookup(order_id);

    if (existing == NIL) {
        // New order (:1585-1589).
        const uint32_t index = alloc_slot();
        if (index == NIL) {
            overflowed_ = true;
            return false;
        }
        const uint32_t level = level_ensure(s, price);
        if (level == NIL) {
            release_slot(index);
            return false;
        }
        slot& entry = slots_[index];
        entry.order_id = order_id;
        entry.price = price;
        entry.qty = qty;
        entry.qp = qp;
        entry.side = static_cast<uint8_t>(s);
        entry.next = NIL;
        entry.prev = NIL;
        list_append(s, level, index);
        id_insert(order_id, index);
        ++size_;
        return true;
    }

    slot& entry = slots_[existing];
    const side stored_side = static_cast<side>(entry.side);

    if (entry.price != price) {
        // Price changed: leave the old level, then either join the new one or die (:1591-1603).
        const int32_t old_level = level_find(stored_side, entry.price);
        if (old_level >= 0) list_remove(stored_side, static_cast<uint32_t>(old_level), existing);
        if (qty != 0) {
            const uint32_t new_level = level_ensure(stored_side, price);
            if (new_level == NIL) {
                // Cold path: the order has already been detached; keep the arena consistent and
                // report the failure rather than corrupt the book.
                id_erase(order_id);
                release_slot(existing);
                --size_;
                return false;
            }
            entry.price = price;
            entry.qty = qty;
            entry.qp = qp;
            entry.next = NIL;
            entry.prev = NIL;
            list_append(stored_side, new_level, existing);
        } else {
            id_erase(order_id);
            release_slot(existing);
            --size_;
        }
        return true;
    }

    if (qty == 0) {
        // Cancelled or filled (:1604-1607).
        const int32_t level = level_find(stored_side, entry.price);
        if (level >= 0) list_remove(stored_side, static_cast<uint32_t>(level), existing);
        id_erase(order_id);
        release_slot(existing);
        --size_;
        return true;
    }

    // Only the quantity moved: the list position is unchanged (:1608-1612).
    entry.qty = qty;
    entry.qp = qp;
    return true;
}

bool order_queue::set_queue_position(uint32_t order_id, qty_t qp) noexcept {
    const uint32_t index = id_lookup(order_id);
    if (index == NIL) return false;
    slot& entry = slots_[index];
    entry.qp = qp;
    if (entry.qp < NOT_SET) entry.qp = 0;  // legacy: "has negative qp, set qp to 0" (:1615-1622)
    return true;
}

// ---- accessors --------------------------------------------------------------

const order_queue::slot* order_queue::find(uint32_t order_id) const noexcept {
    const uint32_t index = id_lookup(order_id);
    return index == NIL ? nullptr : &slots_[index];
}

const order_queue::slot* order_queue::first(side s, price_t price) const noexcept {
    const int32_t level = level_find(s, price);
    if (level < 0) return nullptr;
    const price_level& lv = levels_[to_index(s)][static_cast<uint32_t>(level)];
    return lv.head == NIL ? nullptr : &slots_[lv.head];
}

const order_queue::slot* order_queue::last(side s, price_t price) const noexcept {
    const int32_t level = level_find(s, price);
    if (level < 0) return nullptr;
    const price_level& lv = levels_[to_index(s)][static_cast<uint32_t>(level)];
    return lv.tail == NIL ? nullptr : &slots_[lv.tail];
}

const order_queue::slot* order_queue::next(const slot& current) const noexcept {
    return current.next == NIL ? nullptr : &slots_[current.next];
}

uint32_t order_queue::count_at(side s, price_t price) const noexcept {
    const int32_t level = level_find(s, price);
    if (level < 0) return 0;
    return levels_[to_index(s)][static_cast<uint32_t>(level)].count;
}

}  // namespace tachyon::trade::mm
