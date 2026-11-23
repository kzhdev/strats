#pragma once

#include <array>
#include <cstdint>

#include "limits.hpp"
#include "order_request.hpp"
#include "types.hpp"

// The internal (implied) book of ONE contract: the replacement for
//
//   BidOrdersMap _bids;  AskOrdersMap _asks;                       std::map<price, Order*>
//   std::map<price, PriceLevel> _bidLevels, _askLevels;            external legs, heap nodes
//   Order _bidOrders[MAX_ORDER_ID], _askOrders[MAX_ORDER_ID];       the order objects
//
// (InternalMatchingEngine.h:2140-2153). One order per addressable stream is enough -- the key
// space is exactly SLOTS_PER_CONTRACT -- so the book is a flat array indexed by the request key
// and every lookup is an index computation. No node allocation, no pointer chasing, no
// rebalancing: the best price on a side is a six-element scan.
//
// Behaviour is transcribed from matchOrders (:1076-1230) and modifyOrder (:329-378) with two
// documented deviations:
//   D-EX11 price collisions: legacy `internalOrderMap.emplace(price, &order)` silently drops the
//           *second* order inserted at a price (the map keeps the first), so that order can never
//           be matched internally even though it still quotes externally. `first_wins`
//           reproduces it for the differential gate; `all_matchable` (the default) matches every
//           resting order, tie-broken by insertion order.
//   D-EX12 pay-up pre-pass (`matchPayUpOrder`, :972-1041) belongs to the deferred OM surface
//           (banners/pay-up), so `cross` does not run it.
namespace tachyon::trade::exec {

// Work counters (PLAN.md §4). Compiled in only with -DEXEC_PROFILE, so the shipped build carries no
// accounting at all; a profiling build counts the *slot visits* of every hot loop, which is
// deterministic and therefore a far better optimisation signal than wall-clock on a shared machine.
#ifdef EXEC_PROFILE
struct work_counters {
    uint64_t apply = 0;
    uint64_t level_find = 0;      // slot compares inside level_find
    uint64_t level_shift = 0;     // element moves inside level_insert / level_remove
    uint64_t next_slot = 0;       // slot visits inside best_in_map
    uint64_t best_for = 0;        // slot visits inside best_for / best_pair
    uint64_t cross_steps = 0;
    uint64_t emit = 0;            // level visits inside external()
    static inline work_counters& g() noexcept { static work_counters instance{}; return instance; }
    static void reset() noexcept { g() = work_counters{}; }
};
#define EXEC_COUNT(field, n) (::tachyon::trade::exec::work_counters::g().field += (n))
#else
#define EXEC_COUNT(field, n) ((void)0)
#endif

// Fills produced by one cross. Fixed capacity, no allocation, no indirection: the engine fans out
// from this buffer once per contract per cycle (legacy _internalFills + sendInternalFills).
struct fill_buffer {
    std::array<internal_fill, MAX_FILLS_PER_FLUSH> items{};
    uint32_t count = 0;
    bool overflowed = false;

    void add(const internal_fill& f) noexcept {
        if (count == MAX_FILLS_PER_FLUSH) {
            overflowed = true;
            return;
        }
        items[count++] = f;
    }
    void clear() noexcept {
        count = 0;
        overflowed = false;
    }
    const internal_fill* begin() const noexcept { return items.data(); }
    const internal_fill* end() const noexcept { return items.data() + count; }
};

// What one cross reports besides the fills (legacy matchOrders' return value plus the AS
// bookkeeping at :1109-1122 and :1213-1228).
struct cross_result {
    bool ok = true;              // false: a non-AS self match aborted the match (:1137-1138)
    bool as_self_match = false;  // an AS order crossed another AS order (:1140)
    qty_t as_matched_bid = 0;    // legacy asMatchedSize[BID]
    qty_t as_matched_ask = 0;    // legacy asMatchedSize[ASK]
    bool touched = false;        // any order changed
    bool aggressive_quote_reset = false;  // AS had no external level left (:1213-1223)
};

class implied_book {
public:
    // D-EX11.
    enum class price_collision : uint8_t {
        first_wins,     // legacy: only the first order inserted at a price is matchable
        all_matchable,  // every resting order participates; ties break by insertion order
    };

    struct order {
        price_t price = NULL_PRICE;           // internal price (legacy _price)
        qty_t size = 0;                       // internal size (legacy _size)
        price_t external_price = NULL_PRICE;  // legacy _externalPrice
        qty_t external_size = 0;              // legacy _externalSize
        uint32_t insert_seq = 0;              // arrival order, used for the tie-break
        uint32_t exec_token = 0;              // legacy _orderExecutionToken, for staleness
        uint8_t strategy = 0;
        uint8_t kind = 0;
        uint8_t sd = 0;
        bool live = false;         // in the internal map with size > 0
        // Legacy's `internalOrderMap` membership is *not* the same as "has size": an order whose
        // internal size was exhausted while its external leg was being matched stays in the map
        // until the next cross reaches it (:936-940, :1158-1211). The engine reproduces that so the
        // published best and the insert-collision rule follow the legacy state exactly (D-EX16).
        bool in_map = false;
        bool shadowed = false;     // legacy price-map collision, not matchable (D-EX11)
        bool in_external = false;  // part of the aggregated external leg (quote streams only)
        // Index of the level this order sits in, so leaving it needs no search. Kept valid by
        // `level_drop`, which rewrites the indices of the orders it relocates.
        uint8_t level_index = 0;
        bool pay_up = false;
        bool lean_on_rv = false;
        bool persist = false;

        constexpr strategy_id strategy_value() const noexcept {
            return static_cast<strategy_id>(strategy);
        }
        constexpr request_kind kind_value() const noexcept {
            return static_cast<request_kind>(kind);
        }
        constexpr side side_value() const noexcept { return static_cast<side>(sd); }
        constexpr bool matchable() const noexcept { return in_map && !shadowed && size > 0; }
        // What the legacy map iteration sees: the entry exists, size may be zero.
        constexpr bool in_internal_map() const noexcept { return in_map && !shadowed; }
        constexpr bool has_external() const noexcept {
            return external_price != NULL_PRICE && external_size > 0;
        }
        constexpr liquidity_source source() const noexcept {
            return traits_of(strategy_value(), kind_value()).source;
        }
    };

    implied_book() noexcept { init(price_collision::all_matchable); }
    explicit implied_book(price_collision policy) noexcept { init(policy); }

    void init(price_collision policy, uint32_t contract = INVALID_CONTRACT) noexcept;
    void clear() noexcept;

    uint32_t contract() const noexcept { return contract_; }
    void set_contract(uint32_t contract) noexcept { contract_ = contract; }

    // Legacy modifyOrder (:329-378): leave the book, take the new fields, re-enter if there is
    // an internal price and size, and refresh the external-leg bookkeeping.
    void apply(const order_request& request) noexcept;

    // Legacy removeOrder / removeOrderByStrategy (:428-439): take one stream -- or a whole
    // strategy on both sides -- out of the book and out of the external leg.
    void remove(request_key key) noexcept;
    void remove_strategy(strategy_id strategy) noexcept;

    // Legacy matchOrders (:1076-1230). Fills are appended to `out`; the AS bookkeeping is
    // reported through the result. `ok == false` means a non-AS self match and the caller must
    // alert (:1127-1138).
    cross_result cross(strategy_id aggressor, fill_buffer& out) noexcept;

    // Legacy tryMatch (:881-957): an FAK aggressor that never rests. Walks the opposite side from
    // the best price while `limit` is still acceptable, fills at each resting price, and reports
    // the volume-weighted price of what it took so the caller can report the aggressor's own fill
    // (legacy sendFAKHedgeInternalFillToFront, :553-560). Own-strategy orders are skipped and
    // counted, exactly like the legacy self-match loop.
    struct aggressive_result {
        qty_t filled = 0;
        qty_t remaining = 0;
        price_t vwap = NULL_PRICE;  // legacy fillPrice = cost / filled (:960-970)
        uint32_t self_matches = 0;
        bool as_self_match = false;
        bool touched = false;
    };
    aggressive_result aggress(side aggressor_sd, strategy_id strategy, price_t limit, qty_t size,
                              fill_buffer& out) noexcept;

    // Legacy getBestMarket (:1249-1272): the best order on a side that the subscriber may see --
    // never its own liquidity, never an order that leans on RV. Defined inline below: it is on the
    // flush path and must inline into the engine.
    internal_top best_for(strategy_id subscriber, side sd) const noexcept;
    // Both sides in one pass over the book (two scans become one), for the publish step.
    void best_pair(strategy_id subscriber, internal_top& bid, internal_top& ask) const noexcept;

    // Legacy getPriceAndSize (:835-852) in one pass: the aggregated external leg plus the strategy
    // that owns the order at its top.
    struct external_snapshot {
        price_t price = NULL_PRICE;
        qty_t size = 0;
        strategy_id strategy = strategy_id::MM;
    };
    external_snapshot external(side sd) const noexcept;
    // Legacy getPersistedPrice (:854-869): the best price a quote asked to persist.
    price_t persisted_price(side sd) const noexcept;

    const order* find(request_key key) const noexcept { return &orders_[key.slot()]; }
    const order* find(strategy_id s, request_kind k, side sd) const noexcept {
        return find(request_key{s, k, sd});
    }
    const std::array<order, SLOTS_PER_CONTRACT>& orders() const noexcept { return orders_; }

    bool has_matchable(side sd) const noexcept;
    // Test hooks: the membership mask and the cached level indices must agree with the structures
    // after every operation.
    bool mask_consistent() const noexcept;
    bool level_indices_consistent() const noexcept;

    // Which sides changed since the last clear_dirty(): the engine emits and publishes only those,
    // which halves the per-flush scans in the common case of one side moving.
    uint32_t dirty() const noexcept { return dirty_mask_; }
    void clear_dirty() noexcept { dirty_mask_ = 0; }
    void mark_dirty() noexcept { dirty_mask_ = 3; }

    // Legacy (:1213-1223): an AS self match clears the AS persisted level when it has no
    // external order left, so the IME does not leave a crossed order on the exchange.
    void reset_persisted(strategy_id s, side sd) noexcept {
        persisted_[to_int(s)][to_index(sd)] = NULL_PRICE;
    }

    // ---- the external levels (legacy PriceLevel) -------------------------
    // Legacy keeps, per external price, a std::vector<Order*> built by an insertion rule
    // ("insert before the first entry with a smaller external size", Common.h:1140-1148) that is
    // never re-sorted afterwards, and reports the *head's* current external size. Two orders at one
    // price therefore report the head as the list was left by the insertions, not the largest size
    // today. Reproducing that list is the only way the residual size matches bit for bit.
    struct level_entry {
        price_t price = NULL_PRICE;
        uint8_t count = 0;
        uint8_t slots[MAX_INTERNAL_ORDERS];  // slot indices, in legacy list order
    };

    int32_t level_find(side sd, price_t price) const noexcept;  // index or -1
    uint32_t level_ensure(side sd, price_t price) noexcept;     // index, or UINT32_MAX when full
    void level_add(order& o) noexcept;
    void level_insert(order& o, uint32_t level_index) noexcept;  // into a known level
    // Removes the order from its level; returns the level index when it still exists (so a re-quote
    // at the same price does not need a second lookup), or -1.
    int32_t level_remove(order& o) noexcept;
    void level_drop(side sd, uint32_t level_index) noexcept;

    // Membership bitmask per side, kept in step with `order::in_map`. The best/serve walks then visit
    // only occupied slots (typically 1-3) instead of all six indices -- measured: `best_in_map` went
    // from 36 slot visits per contract-cycle to 12.
    // The mask stores one bit per *side-relative* position (0..5), so a walk can turn a bit straight
    // back into a slot index with `bit * SIDE_COUNT + side`.
    uint32_t slot_index(const order& o) const noexcept {
        return static_cast<uint32_t>(&o - orders_.data());
    }
    static uint16_t mask_bit(uint32_t slot) noexcept {
        return static_cast<uint16_t>(1u << (slot / SIDE_COUNT));
    }
    void mark_in_map(order& o) noexcept {
        o.in_map = true;
        in_map_mask_[to_index(o.side_value())] |= mask_bit(slot_index(o));
    }
    void unmark_in_map(order& o) noexcept {
        o.in_map = false;
        in_map_mask_[to_index(o.side_value())] &= static_cast<uint16_t>(~mask_bit(slot_index(o)));
    }

    std::array<std::array<level_entry, SLOTS_PER_CONTRACT>, SIDE_COUNT> levels_{};
    std::array<uint8_t, SIDE_COUNT> level_count_{};
    std::array<uint16_t, SIDE_COUNT> in_map_mask_{};

private:
    // Best matchable order slot on a side, ignoring the slots in `excluded` (a bitmask).
    // INVALID_SLOT when the side has nothing left to match.
    uint32_t best_slot(side sd, uint32_t excluded) const noexcept;
    // Best slot that is in the legacy internal map (size may be zero): the walk  does.
    uint32_t best_in_map(side sd, uint32_t excluded) const noexcept;

    static constexpr uint32_t INVALID_SLOT = 0xFFFFFFFFu;

    // Legacy doExternalReductionAfterInternalFill (:1043-1056).
    void reduce_external(order& o, qty_t fill) noexcept;

    // True when an order already sits in the internal map at `price`, which is what makes a
    // second insert at the same price fail in legacy (D-EX11).
    bool occupied_at(price_t price, side sd, uint32_t self_slot) const noexcept;

    std::array<order, SLOTS_PER_CONTRACT> orders_{};
    price_collision policy_ = price_collision::all_matchable;
    // Legacy _topLevelsByStrategyBySide[strategy][side]: the price a quote asked to keep
    // (persistPrice ? externalPrice : NULL), kept even when its size is zero.
    std::array<std::array<price_t, SIDE_COUNT>, STRATEGY_COUNT> persisted_{};
    uint32_t contract_ = INVALID_CONTRACT;
    uint32_t next_seq_ = 1;
    uint32_t dirty_mask_ = 3;  // both sides start dirty: the first flush must publish and quote
};

// ---------------------------------------------------------------------------
// Hot queries, defined inline: they run once per contract per flush (and the publish step runs
// them for every subscriber), so they must inline into implied_engine.cpp rather than cross a
// translation unit boundary.
// ---------------------------------------------------------------------------
inline uint32_t implied_book::best_slot(side sd, uint32_t excluded) const noexcept {
    uint32_t best = INVALID_SLOT;
    for (uint32_t slot = to_index(sd); slot < SLOTS_PER_CONTRACT; slot += SIDE_COUNT) {
        if (excluded & (1u << slot)) continue;
        const order& o = orders_[slot];
        if (!o.matchable()) continue;
        if (best == INVALID_SLOT) {
            best = slot;
            continue;
        }
        const order& current = orders_[best];
        if (is_better_price(o.price, current.price, sd)) {
            best = slot;
        } else if (o.price == current.price && o.insert_seq < current.insert_seq) {
            // Equal price: the earliest arrival wins, which is what the legacy price-keyed map
            // does implicitly (it keeps the first order inserted at a price).
            best = slot;
        }
    }
    return best;
}

inline uint32_t implied_book::best_in_map(side sd, uint32_t excluded) const noexcept {
    uint32_t best = INVALID_SLOT;
    price_t best_price = NULL_PRICE;
    uint32_t best_seq = 0;
    // Only the slots the mask says are populated are visited (typically one to three).
    // The mask is side-relative while `excluded` is an absolute-slot mask, so the exclusion is tested
    // per candidate rather than folded into the bit walk.
    const uint32_t sd_index = to_index(sd);
    uint32_t candidates = in_map_mask_[sd_index];
    EXEC_COUNT(next_slot, static_cast<uint64_t>(__builtin_popcount(candidates)));
    while (candidates != 0) {
        const uint32_t slot =
            static_cast<uint32_t>(__builtin_ctz(candidates)) * SIDE_COUNT + sd_index;
        candidates &= candidates - 1;
        if (excluded & (1u << slot)) continue;
        const order& o = orders_[slot];
        if (!o.in_internal_map()) continue;  // shadowed slots stay in the mask
        // The map is ordered by price, and a second insertion at a price cannot happen (emplace), so
        // the price decides; the sequence only breaks a tie the map can no longer produce.
        if (best == INVALID_SLOT || is_better_price(o.price, best_price, sd) ||
            (o.price == best_price && o.insert_seq < best_seq)) {
            best = slot;
            best_price = o.price;
            best_seq = o.insert_seq;
        }
    }
    return best;
}

inline bool implied_book::mask_consistent() const noexcept {
    for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
        const order& o = orders_[slot];
        const bool in_mask = (in_map_mask_[to_index(o.side_value())] & mask_bit(slot)) != 0;
        if (in_mask != o.in_map) return false;
    }
    return true;
}

inline bool implied_book::level_indices_consistent() const noexcept {
    for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
        const uint32_t level_count = level_count_[si];
        for (uint32_t li = 0; li < level_count; ++li) {
            const level_entry& level = levels_[si][li];
            for (uint32_t i = 0; i < level.count; ++i) {
                const order& o = orders_[level.slots[i]];
                if (o.level_index != li) return false;
                if (o.external_price != level.price) return false;
            }
        }
    }
    return true;
}

inline bool implied_book::has_matchable(side sd) const noexcept {
    for (uint32_t slot = to_index(sd); slot < SLOTS_PER_CONTRACT; slot += SIDE_COUNT) {
        if (orders_[slot].matchable()) return true;
    }
    return false;
}

inline internal_top implied_book::best_for(strategy_id subscriber, side sd) const noexcept {
    internal_top top;
    // "Have a candidate" is tracked separately from `top.empty()`: legacy publishes a dead entry
    // (a price with no size) as the best, so emptiness of the *size* cannot mean "nothing found yet".
    bool have = false;
    const uint32_t sd_index = to_index(sd);
    uint32_t candidates = in_map_mask_[sd_index];
    EXEC_COUNT(best_for, static_cast<uint64_t>(__builtin_popcount(candidates)));
    while (candidates != 0) {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(candidates)) * SIDE_COUNT + sd_index;
        candidates &= candidates - 1;
        const order& o = orders_[slot];
        if (!o.in_internal_map()) continue;
        if (o.strategy_value() == subscriber) continue;  // never publish a strategy its own order
        if (o.lean_on_rv) continue;  // ":1257 skip the AS order that leans on RV"
        if (!have || is_better_price(o.price, top.price, sd)) {
            have = true;
            top.price = o.price;
            top.size = o.size;
            top.source = o.source();
        }
    }
    return top;
}

inline void implied_book::best_pair(strategy_id subscriber, internal_top& bid,
                                    internal_top& ask) const noexcept {
    bid = internal_top{};
    ask = internal_top{};
    bool have_bid = false;
    bool have_ask = false;
    // Both sides in one walk over the two side-relative masks: bit k of mask[s] is slot
    // `k * SIDE_COUNT + s`, and the pairs of bits (k, k) describe one (strategy, kind).
    uint32_t candidates = in_map_mask_[0] | in_map_mask_[1];
    EXEC_COUNT(best_for, static_cast<uint64_t>(__builtin_popcount(candidates)));
    while (candidates != 0) {
        const uint32_t bit = static_cast<uint32_t>(__builtin_ctz(candidates));
        candidates &= candidates - 1;
        for (uint32_t si = 0; si < SIDE_COUNT; ++si) {
            if ((in_map_mask_[si] & (1u << bit)) == 0) continue;
            const uint32_t slot = bit * SIDE_COUNT + si;
            const order& o = orders_[slot];
            if (!o.in_internal_map()) continue;
            if (o.strategy_value() == subscriber) continue;
            if (o.lean_on_rv) continue;
            const side sd = o.side_value();
            internal_top& top = sd == side::bid ? bid : ask;
            bool& have = sd == side::bid ? have_bid : have_ask;
            if (!have || is_better_price(o.price, top.price, sd)) {
                have = true;
                top.price = o.price;
                top.size = o.size;
                top.source = o.source();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The external level list, transcribed from legacy PriceLevel + addToPriceLevel.
// ---------------------------------------------------------------------------
inline int32_t implied_book::level_find(side sd, price_t price) const noexcept {
    const auto& side_levels = levels_[to_index(sd)];
    const uint32_t count = level_count_[to_index(sd)];
    EXEC_COUNT(level_find, count);
    for (uint32_t i = 0; i < count; ++i) {
        if (side_levels[i].price == price) return static_cast<int32_t>(i);
    }
    return -1;
}

inline uint32_t implied_book::level_ensure(side sd, price_t price) noexcept {
    const int32_t found = level_find(sd, price);
    if (found >= 0) return static_cast<uint32_t>(found);
    const uint32_t count = level_count_[to_index(sd)];
    if (count == SLOTS_PER_CONTRACT) return 0xFFFFFFFFu;  // cannot happen: <= 6 orders a side
    level_entry& level = levels_[to_index(sd)][count];
    level.price = price;
    level.count = 0;
    level_count_[to_index(sd)] = static_cast<uint8_t>(count + 1);
    return count;
}

inline void implied_book::level_drop(side sd, uint32_t level_index) noexcept {
    const uint32_t index = to_index(sd);
    const uint32_t last = static_cast<uint32_t>(level_count_[index]) - 1;
    if (level_index != last) {
        levels_[index][level_index] = levels_[index][last];
        // The relocated level's orders must remember where they went.
        const level_entry& moved = levels_[index][level_index];
        for (uint32_t i = 0; i < moved.count; ++i) {
            orders_[moved.slots[i]].level_index = static_cast<uint8_t>(level_index);
        }
    }
    levels_[index][last] = level_entry{};
    level_count_[index] = static_cast<uint8_t>(last);
}

inline void implied_book::level_add(order& o) noexcept {
    const side sd = o.side_value();
    const uint32_t level_index = level_ensure(sd, o.external_price);
    if (level_index == 0xFFFFFFFFu) return;
    level_insert(o, level_index);
}

inline void implied_book::level_insert(order& o, uint32_t level_index) noexcept {
    const side sd = o.side_value();
    const uint32_t slot = static_cast<uint32_t>(&o - orders_.data());
    level_entry& level = levels_[to_index(sd)][level_index];

    // Legacy PriceLevel::add: insert before the first entry whose *current* external size is smaller,
    // otherwise append. The list is never re-sorted afterwards.
    uint32_t at = level.count;
    for (uint32_t i = 0; i < level.count; ++i) {
        if (o.external_size > orders_[level.slots[i]].external_size) {
            at = i;
            break;
        }
    }
    EXEC_COUNT(level_shift, level.count - at);
    for (uint32_t i = level.count; i > at; --i) level.slots[i] = level.slots[i - 1];
    level.slots[at] = static_cast<uint8_t>(slot);
    ++level.count;
    o.in_external = true;
    o.level_index = static_cast<uint8_t>(level_index);
}

inline int32_t implied_book::level_remove(order& o) noexcept {
    if (!o.in_external) return -1;
    const side sd = o.side_value();
    const uint32_t slot = static_cast<uint32_t>(&o - orders_.data());
    // The cached index is used when it still points at this order's level; the price check makes a
    // stale cache a slow path rather than a bug.
    int32_t found = -1;
    if (o.level_index < level_count_[to_index(sd)] &&
        levels_[to_index(sd)][o.level_index].price == o.external_price) {
        found = static_cast<int32_t>(o.level_index);
    } else {
        found = level_find(sd, o.external_price);
    }
    if (found < 0) {
        o.in_external = false;
        return -1;
    }
    const uint32_t level_index = static_cast<uint32_t>(found);
    level_entry& level = levels_[to_index(sd)][level_index];
    for (uint32_t i = 0; i < level.count; ++i) {
        if (level.slots[i] != slot) continue;
        EXEC_COUNT(level_shift, level.count - i - 1);
        for (uint32_t j = i; j + 1 < level.count; ++j) level.slots[j] = level.slots[j + 1];
        --level.count;
        break;
    }
    o.in_external = false;
    if (level.count == 0) {
        level_drop(sd, level_index);
        return -1;
    }
    return found;
}

inline implied_book::external_snapshot implied_book::external(side sd) const noexcept {
    external_snapshot result;
    // The best level on the side, and the size of the order at the head of its list -- which is what
    // legacy PriceLevel::getSize reports (Common.h:1104-1106).
    const auto& side_levels = levels_[to_index(sd)];
    const uint32_t count = level_count_[to_index(sd)];
    EXEC_COUNT(emit, count);
    int32_t best = -1;
    for (uint32_t i = 0; i < count; ++i) {
        const level_entry& level = side_levels[i];
        if (level.count == 0) continue;
        if (best < 0 || is_better_price(level.price, side_levels[best].price, sd)) {
            best = static_cast<int32_t>(i);
        }
    }
    if (best < 0) return result;
    const level_entry& level = side_levels[best];
    const order& head = orders_[level.slots[0]];
    result.price = level.price;
    result.size = head.external_size;
    result.strategy = head.strategy_value();
    return result;
}

inline price_t implied_book::persisted_price(side sd) const noexcept {
    price_t result = NULL_PRICE;
    for (uint32_t s = 0; s < STRATEGY_COUNT; ++s) {
        const price_t price = persisted_[s][to_index(sd)];
        if (price == NULL_PRICE) continue;
        result = better_price(result, price, sd);
    }
    return result;
}

// ---------------------------------------------------------------------------
// The book is header-only on purpose: `apply`, `cross` and `aggress` are called once per
// request on the cycle path, and inlining them into implied_engine.cpp removes two calls and
// three reload barriers per contract per cycle. implied_book.cpp keeps the invariant gate.
// ---------------------------------------------------------------------------
inline void implied_book::init(price_collision policy, uint32_t contract) noexcept {
    policy_ = policy;
    contract_ = contract;
    clear();
}

inline void implied_book::clear() noexcept {
    for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
        const uint8_t s = static_cast<uint8_t>(slot / (KIND_COUNT * SIDE_COUNT));
        const uint8_t k = static_cast<uint8_t>((slot / SIDE_COUNT) % KIND_COUNT);
        const uint8_t d = static_cast<uint8_t>(slot % SIDE_COUNT);
        orders_[slot] = order{};
        orders_[slot].strategy = s;
        orders_[slot].kind = k;
        orders_[slot].sd = d;
    }
    for (auto& per_side : persisted_) per_side = {NULL_PRICE, NULL_PRICE};
    for (auto& side_levels : levels_) side_levels = {};
    level_count_ = {};
    in_map_mask_ = {};
    next_seq_ = 1;
    dirty_mask_ = 3;
}

inline bool implied_book::occupied_at(price_t price, side sd, uint32_t self_slot) const noexcept {
    for (uint32_t slot = to_index(sd); slot < SLOTS_PER_CONTRACT; slot += SIDE_COUNT) {
        if (slot == self_slot) continue;
        const order& o = orders_[slot];
        // Only an order that is actually in the legacy map blocks a new insert: a shadowed order
        // is not in the map, so a third order at the same price is blocked by the first one.
        if (o.in_internal_map() && o.price == price) return true;
    }
    return false;
}

inline void implied_book::apply(const order_request& request) noexcept {
    EXEC_COUNT(apply, 1);
    const request_key key = request.key();
    order& o = orders_[key.slot()];

    // Legacy removeOrder<Side>(order): leave the internal map and the external level before the
    // new fields are installed, so a modify is always insert-fresh.
    o.live = false;
    unmark_in_map(o);
    o.shadowed = false;
    // The level it leaves is reused if the external price is unchanged, so a re-quote costs one lookup
    // instead of two (legacy removes and re-inserts; the resulting list order is the same).
    const int32_t previous_level = level_remove(o);

    o.price = request.internal_price;
    o.external_price = request.external_price;
    o.exec_token = request.exec_token;
    o.pay_up = request.pay_up();
    o.lean_on_rv = request.lean_on_rv();
    o.persist = request.persist();
    o.insert_seq = next_seq_++;

    // Legacy modifyOrder is called with the hedge size in *both* the internal and the external
    // slot (:2018-2019), so a hedge quotes the same size at both prices.
    const bool hedge = key.kind == request_kind::hedge;
    o.size = hedge ? request.hedge_size : request.internal_size;
    o.external_size = hedge ? request.hedge_size : request.external_size;

    if (o.price != NULL_PRICE && o.size != 0) {
        o.live = true;
        mark_in_map(o);
        if (policy_ == price_collision::first_wins) {
            o.shadowed = occupied_at(o.price, key.sd, key.slot());
        }
    }

    if (!hedge) {
        // Legacy modifyOrder (:360-361) and (:363-369).
        persisted_[to_int(key.strategy)][to_index(key.sd)] =
            o.persist ? o.external_price : NULL_PRICE;
        if (o.external_price != NULL_PRICE && o.external_size != 0) {
            const uint32_t index = to_index(key.sd);
            if (previous_level >= 0 &&
                levels_[index][previous_level].price == o.external_price) {
                level_insert(o, static_cast<uint32_t>(previous_level));
            } else {
                level_add(o);
            }
        }
    }
    dirty_mask_ |= 1u << to_index(key.sd);
}

inline void implied_book::remove(request_key key) noexcept {
    order& o = orders_[key.slot()];
    o.live = false;
    unmark_in_map(o);
    o.shadowed = false;
    level_remove(o);
    o.price = NULL_PRICE;
    o.size = 0;
    o.external_price = NULL_PRICE;
    o.external_size = 0;
    dirty_mask_ |= 1u << to_index(key.sd);
}

inline void implied_book::remove_strategy(strategy_id strategy) noexcept {
    for (uint32_t kind = 0; kind < KIND_COUNT; ++kind) {
        for (uint32_t sd = 0; sd < SIDE_COUNT; ++sd) {
            remove(request_key{strategy, static_cast<request_kind>(kind), static_cast<side>(sd)});
        }
    }
}


inline void implied_book::reduce_external(order& o, qty_t fill) noexcept {
    if (o.external_price == NULL_PRICE) return;
    const qty_t next = std::max<qty_t>(o.external_size - fill, 0);
    o.external_size = next;
    // Legacy removes the order from the quote level once its external size is exhausted
    // (:1049-1053); hedges are not on a level, so they keep the (zero) size.
    if (next == 0 && o.kind_value() != request_kind::hedge) level_remove(o);
}

inline cross_result implied_book::cross(strategy_id aggressor, fill_buffer& out) noexcept {
    cross_result result;

    // Legacy walks the price map from the top (`orderMap.begin()`), so a dead entry - an order whose
    // internal size was exhausted while its external leg was being matched - is reached and erased
    // *inside* the loop (:936-940), and its self-match test still runs before the zero-size fill.
    // Walking the map directly (rather than skipping to the first live order) is what makes the abort
    // flag identical. Zero-size fills are not recorded (D-EX17).
    // `best_in_map` is itself the "is the map empty" test (it walks membership, not size), so the
    // early out costs one scan instead of two -- and, like legacy (:1079-1087), it happens before
    // anything is erased.
    uint32_t excluded_bid = 0;
    uint32_t excluded_ask = 0;
    uint32_t bid_slot = best_in_map(side::bid, excluded_bid);
    uint32_t ask_slot = best_in_map(side::ask, excluded_ask);
    if (bid_slot == INVALID_SLOT || ask_slot == INVALID_SLOT) return result;

    order* bid_order = &orders_[bid_slot];
    order* ask_order = &orders_[ask_slot];
    price_t bid_top = bid_order->price;
    price_t ask_top = ask_order->price;
    qty_t bid_size = bid_order->size;
    qty_t ask_size = ask_order->size;
    // Legacy matchBidExternal / matchAskExternal: once an order's internal size is exhausted, its
    // own external price and size keep participating in the cross (:1158-1180).
    bool bid_external = false;
    bool ask_external = false;

    EXEC_COUNT(cross_steps, 1);
    constexpr uint32_t max_steps = 4 * SLOTS_PER_CONTRACT;
    for (uint32_t step = 0; step < max_steps && bid_top >= ask_top; ++step) {
        const strategy_id bid_strategy = bid_order->strategy_value();
        const strategy_id ask_strategy = ask_order->strategy_value();

        if (bid_strategy == ask_strategy) {
            if (bid_strategy != strategy_id::AS) {
                // Legacy aborts the whole match and disables external quoting (:1127-1138).
                result.ok = false;
                return result;
            }
            result.as_self_match = true;  // "ArbieSpreader self match is allowed" (:1129)
        }

        // Legacy fill price (:1143): the passive side's price, i.e. the ask when the bid is the
        // aggressor and the bid otherwise.
        const price_t fill_price = bid_strategy == aggressor ? ask_top : bid_top;
        const qty_t fill_size = std::min(bid_size, ask_size);

        if (aggressor == strategy_id::AS && fill_size > 0) {
            if (bid_strategy == strategy_id::AS) result.as_matched_bid += fill_size;
            if (ask_strategy == strategy_id::AS) result.as_matched_ask += fill_size;
        }

        if (fill_size > 0) {
            out.add(internal_fill{contract_, bid_strategy, bid_order->kind_value(),
                                  bid_order->side_value(), fill_price, fill_size});
            out.add(internal_fill{contract_, ask_strategy, ask_order->kind_value(),
                                  ask_order->side_value(), fill_price, fill_size});
        }
        reduce_external(*bid_order, fill_size);
        reduce_external(*ask_order, fill_size);

        bid_size -= fill_size;
        ask_size -= fill_size;
        result.touched = true;
        dirty_mask_ |= 3;  // both sides of the book moved

        if (!bid_external) {
            bid_order->size -= fill_size;
            if (bid_order->size <= 0) {
                bid_order->size = 0;
                bid_order->live = false;
                if (bid_order->has_external()) {
                    bid_external = true;
                    bid_top = bid_order->external_price;
                    bid_size = bid_order->external_size;
                }
            }
        }

        if (!ask_external) {
            ask_order->size -= fill_size;
            if (ask_order->size <= 0) {
                ask_order->size = 0;
                ask_order->live = false;
                if (ask_order->has_external()) {
                    ask_external = true;
                    ask_top = ask_order->external_price;
                    ask_size = ask_order->external_size;
                }
            }
        }

        if (bid_size <= 0) {
            unmark_in_map(*bid_order);       // legacy erases the price level from the map
            excluded_bid |= 1u << bid_slot;
            bid_external = false;
            bid_slot = best_in_map(side::bid, excluded_bid);
            if (bid_slot == INVALID_SLOT) break;
            bid_order = &orders_[bid_slot];
            bid_top = bid_order->price;
            bid_size = bid_order->size;
        }

        if (ask_size <= 0) {
            unmark_in_map(*ask_order);
            excluded_ask |= 1u << ask_slot;
            ask_external = false;
            ask_slot = best_in_map(side::ask, excluded_ask);
            if (ask_slot == INVALID_SLOT) break;
            ask_order = &orders_[ask_slot];
            ask_top = ask_order->price;
            ask_size = ask_order->size;
        }
    }

    if (bid_external) {
        // Legacy (:1206-1211): an order whose internal size was exhausted while its external leg was
        // still being matched leaves the internal map when the cross ends -- the external leg is a
        // quote from then on, not internal liquidity.
        unmark_in_map(*bid_order);
    }
    if (ask_external) {
        unmark_in_map(*ask_order);
    }

    if (result.as_self_match) {
        // Legacy (:1213-1223): with no external level left, clear the AS persisted top so the
        // engine cannot leave a crossed order resting on the exchange.
        if (external(side::bid).price == NULL_PRICE) reset_persisted(strategy_id::AS, side::bid);
        if (external(side::ask).price == NULL_PRICE) reset_persisted(strategy_id::AS, side::ask);
        result.aggressive_quote_reset = true;
    }

    return result;
}







inline implied_book::aggressive_result implied_book::aggress(side aggressor_sd, strategy_id strategy,
                                                      price_t limit, qty_t size,
                                                      fill_buffer& out) noexcept {
    aggressive_result result;
    result.remaining = size;

    const side passive_sd = other(aggressor_sd);
    uint32_t excluded = 0;
    qty_t cost = 0;

    while (result.remaining > 0) {
        const uint32_t slot = best_in_map(passive_sd, excluded);
        if (slot == INVALID_SLOT) break;
        order& passive = orders_[slot];

        // Legacy IsPriceBetterOrEqual<Side>(price, order->_price) (:895). It is checked before the
        // self-match test, and before the dead-entry test, because everything below mirrors the
        // order of the legacy branches.
        if (!is_better_or_equal(limit, passive.price, aggressor_sd)) break;

        if (passive.strategy_value() == strategy) {
            // Legacy self-match skip (:896-922): AS counts and is disabled at the threshold,
            // everyone else just skips the order. A dead own entry is skipped and *kept* (:917-921).
            ++result.self_matches;
            if (strategy == strategy_id::AS) result.as_self_match = true;
            excluded |= 1u << slot;
            continue;
        }

        if (passive.size <= 0) {
            // Legacy pushes a zero-size fill here and then erases the entry (:929-938); the engine
            // erases it without inventing a trade (D-EX17).
            unmark_in_map(passive);
            excluded |= 1u << slot;
            continue;
        }

        const qty_t fill = std::min(passive.size, result.remaining);
        if (fill <= 0) {
            excluded |= 1u << slot;
            continue;
        }

        out.add(internal_fill{contract_, passive.strategy_value(), passive.kind_value(),
                              passive.side_value(), passive.price, fill});
        reduce_external(passive, fill);
        passive.size -= fill;
        if (passive.size <= 0) {
            passive.size = 0;
            passive.live = false;
            unmark_in_map(passive);
            excluded |= 1u << slot;
        }

        dirty_mask_ |= 3;
        cost += passive.price * fill;
        result.filled += fill;
        result.remaining -= fill;
        result.touched = true;
    }

    if (result.filled > 0) result.vwap = cost / result.filled;
    return result;
}

}  // namespace tachyon::trade::exec
