// State-layer tests for the new MarketMaker engine: the book and the resting-order queue.
//
//   mm_state_tests
//
// Phase-2 gate (PLAN.md §6). The order-queue cases are pinned to
// SimpleMarketMaker::onOrderUpdate / onQueuePositionUpdate (:1562-1642) and the book cases to
// getPriceLevelIndex (:407-422), calculateMidPoint's level usage (:911-919) and the
// change-index bookkeeping of MarketDataConsumer.h:129-222.
//
// The queue is additionally exercised by a randomised property test against an independent
// transcription of the legacy containers (an unordered_map plus per-level vectors), which is
// what catches an open-addressing or free-list defect that hand-written cases would miss.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "book.hpp"
#include "limits.hpp"
#include "order_queue.hpp"
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

constexpr price_t TICK = 25'000'000;  // 0.025 at 1e-9
price_t px(int ticks) { return TICK * ticks; }

level make_level(int ticks, qty_t qty, uint32_t orders, qty_t implied = 0) {
    level l;
    l.price = px(ticks);
    l.qty = qty;
    l.order_count = orders;
    l.implied_untracked = implied;
    return l;
}

// ---------------------------------------------------------------------------
// book: change indices (MarketDataConsumer.h:129-152)
// ---------------------------------------------------------------------------
void test_book_change_indices() {
    std::printf("book: change indices\n");

    book b;
    b.init(side::bid);
    check(!b.qty_changed(), "fresh book reports no change");
    check_eq(b.qty_change_index(), book::NO_CHANGE, "qty index starts at NO_CHANGE");

    // 1-based feed numbers become 0-based indices; 0 means "this field did not change".
    check(b.note_change(0, 3), "valid update accepted");
    check_eq(b.qty_change_index(), 2, "qtyNum 3 -> index 2");
    check_eq(b.price_change_index(), book::NO_CHANGE, "priceNum 0 -> no price change");

    // Only the smallest index since the last reset survives.
    check(b.note_change(0, 5), "second update accepted");
    check_eq(b.qty_change_index(), 2, "a deeper change does not raise the index");
    check(b.note_change(0, 1), "third update accepted");
    check_eq(b.qty_change_index(), 0, "a change at the top lowers it to 0");
    check(b.top_changed(), "top_changed() when the qty index is 0");

    b.clear_change_indices();
    check(!b.qty_changed(), "reset clears the index");

    // Level 10 is the last valid one; 11 is out of range and rejects the whole update.
    check(b.note_change(0, BOOK_LEVELS), "level 10 is valid");
    check_eq(b.qty_change_index(), 9, "level 10 -> index 9");
    b.clear_change_indices();
    check(!b.note_change(0, BOOK_LEVELS + 1), "level 11 is rejected");
    check_eq(b.qty_change_index(), book::NO_CHANGE, "rejected update does not record an index");
    check(!b.note_change(BOOK_LEVELS + 1, 0), "out-of-range price index is rejected too");
}

// ---------------------------------------------------------------------------
// book: level lookup and tracked quantity (getPriceLevelIndex, :407-422 + :795)
// ---------------------------------------------------------------------------
void test_book_level_lookup() {
    std::printf("book: level lookup\n");

    level bids[3] = {make_level(100, 40, 4, 5), make_level(99, 30, 3), make_level(98, 20, 2)};
    book b;
    b.init(side::bid);
    b.apply_snapshot(bids, 3);

    check(b.has_price(), "book has a price");
    check_eq(b.top().price, px(100), "top is the highest bid");
    check_eq(b.level_index(px(100)), 0, "best price -> level 0");
    check_eq(b.level_index(px(99)), 1, "second level");
    check_eq(b.level_index(px(98)), 2, "third level");
    check_eq(b.level_index(px(97)), -1, "absent price -> -1");
    check_eq(b.level_index(px(101)), -1, "better-than-top price -> -1");
    check_eq(b.level_index(tachyon::NULL_PRICE), -1, "NULL price -> -1");

    // tracked_qty subtracts the implied quantity our orders cannot be behind (:795).
    check_eq(b.tracked_qty(0), 35, "40 displayed - 5 implied");
    check_eq(b.tracked_qty(1), 30, "no implied quantity on level 1");

    // Levels beyond the snapshot are NULL, and the scan stops at the first NULL.
    level asks[2] = {make_level(101, 10, 1), make_level(102, 12, 1)};
    book a;
    a.init(side::ask);
    a.apply_snapshot(asks, 2);
    check_eq(a.level_index(px(101)), 0, "best ask -> level 0");
    check_eq(a.level_index(px(100)), -1, "better-than-top ask -> -1");
    check_eq(a.level_index(px(103)), -1, "beyond the snapshot -> -1");

    // Empty book: every lookup is -1, on both sides.
    book empty_bid;
    empty_bid.init(side::bid);
    check(!empty_bid.has_price(), "empty book has no price");
    check_eq(empty_bid.level_index(px(100)), -1, "empty bid book -> -1");
    book empty_ask;
    empty_ask.init(side::ask);
    check_eq(empty_ask.level_index(px(100)), -1, "empty ask book -> -1");
}

// ---------------------------------------------------------------------------
// book: trade-driven adjustment (handleMDTrade, MarketDataConsumer.h:175-222)
// ---------------------------------------------------------------------------
void test_book_trade() {
    std::printf("book: trade adjustment\n");

    // A realistic book: the feed populates all ten levels, which matters because the
    // "inside the book" branch is guarded by IsBetter(trade, l[9]) and a NULL l[9] would send
    // the trade into the traded-through branch instead (as the legacy code does too).
    level bids[BOOK_LEVELS];
    for (int i = 0; i < static_cast<int>(BOOK_LEVELS); ++i) {
        bids[i] = make_level(100 - i, static_cast<qty_t>(40 - 2 * i), 4);
    }

    // A print at our best level only reduces it.
    {
        book b;
        b.init(side::bid);
        b.apply_snapshot(bids, BOOK_LEVELS);
        b.clear_change_indices();
        b.apply_trade(px(100), 25, 3);
        check_eq(b.qty_change_index(), 0, "same-price trade is a qty change at level 0");
        check_eq(b.price_change_index(), book::NO_CHANGE, "same-price trade is not a price change");
        check_eq(b.top().qty, 25, "best size follows the trade");
        check_eq(b.top().price, px(100), "best price unchanged");
        check_eq(b.at(1).qty, 38, "level 1 untouched");
    }

    // A print better than our best pushes the book back.
    {
        book b;
        b.init(side::bid);
        b.apply_snapshot(bids, BOOK_LEVELS);
        b.clear_change_indices();
        b.apply_trade(px(101), 7, 1);
        check_eq(b.top().price, px(101), "better print becomes the new best");
        check_eq(b.at(1).price, px(100), "the old best moves to level 1");
        check_eq(b.at(9).price, px(92), "the last level falls off the back");
        check_eq(b.qty_change_index(), 0, "qty index reset to the top");
        check_eq(b.price_change_index(), 0, "price index reset to the top");
    }

    // A print at level 2 (98) means levels 0 and 1 were consumed: the levels behind it move up.
    {
        book b;
        b.init(side::bid);
        b.apply_snapshot(bids, BOOK_LEVELS);
        b.clear_change_indices();
        b.apply_trade(px(98), 5, 1);
        check_eq(b.top().price, px(98), "the print becomes the best");
        check_eq(b.top().qty, 5, "with the traded size");
        check_eq(b.at(1).price, px(97), "the level behind it moves up to 1");
        check_eq(b.at(7).price, px(91), "the deepest level moves up to 7");
        // Legacy clears only pr/q at index LEVEL_NUM - i, and leaves what was already at
        // index LEVEL_NUM - 1 stale. level_index stops at the first NULL, so the stale tail
        // is unreachable — pinned here so the transcription stays exact.
        check_eq(b.at(8).price, tachyon::NULL_PRICE, "index 8 is cleared");
        check_eq(b.level_index(px(91)), 7, "the moved level is found at its new index");
    }

    // A print through the whole book collapses everything but the top.
    {
        book b;
        b.init(side::bid);
        b.apply_snapshot(bids, BOOK_LEVELS);
        b.clear_change_indices();
        b.apply_trade(px(50), 9, 1);
        check_eq(b.top().price, px(50), "traded-through price becomes the best");
        check_eq(b.at(1).price, tachyon::NULL_PRICE, "level 1 is cleared");
        check_eq(b.qty_change_index(), 0, "traded-through is a top change");
    }

    // The ask side mirrors the comparisons (a "better" print is a lower price).
    {
        level asks[BOOK_LEVELS];
        for (int i = 0; i < static_cast<int>(BOOK_LEVELS); ++i) {
            asks[i] = make_level(101 + i, static_cast<qty_t>(10 + 2 * i), 1);
        }
        book a;
        a.init(side::ask);
        a.apply_snapshot(asks, BOOK_LEVELS);
        a.clear_change_indices();
        a.apply_trade(px(100), 4, 1);
        check_eq(a.top().price, px(100), "better ask print becomes the best");
        check_eq(a.at(1).price, px(101), "old best shifts back");
    }
}

// ---------------------------------------------------------------------------
// order_queue: the legacy onOrderUpdate / onQueuePositionUpdate branches
// ---------------------------------------------------------------------------
void test_order_queue_branches() {
    std::printf("order_queue: legacy branches\n");

    order_queue q;
    const qty_t NOT_SET = order_queue::NOT_SET;

    // New order: appended, insertion order preserved (:1585-1589).
    check(q.upsert(1, px(100), 5, NOT_SET, side::bid), "insert id 1");
    check(q.upsert(2, px(100), 7, NOT_SET, side::bid), "insert id 2 at the same price");
    check_eq(q.size(), 2, "two resting orders");
    check_eq(q.first(side::bid, px(100))->order_id, 1, "front is the first inserted");
    check_eq(q.last(side::bid, px(100))->order_id, 2, "back is the newest (:835)");
    check_eq(q.count_at(side::bid, px(100)), 2, "level count");

    // A quantity-only update keeps the list position (:1608-1612).
    check(q.upsert(1, px(100), 9, 4, side::bid), "update id 1 in place");
    check_eq(q.last(side::bid, px(100))->order_id, 2, "in-place update does not reorder");
    check_eq(q.find(1)->qty, 9, "qty updated");
    check_eq(q.find(1)->qp, 4, "queue position updated with the order update");

    // A price move leaves the old level and joins the back of the new one (:1591-1603).
    check(q.upsert(1, px(101), 9, 4, side::bid), "move id 1 up a tick");
    check_eq(q.count_at(side::bid, px(100)), 1, "old level keeps only id 2");
    check_eq(q.count_at(side::bid, px(101)), 1, "new level holds id 1");
    check_eq(q.last(side::bid, px(101))->order_id, 1, "moved order is at the back");

    // Removing from the middle preserves the order of the remaining orders.
    check(q.upsert(3, px(101), 3, 0, side::bid), "insert id 3");
    check(q.upsert(4, px(101), 3, 0, side::bid), "insert id 4");
    check(q.upsert(3, px(101), 0, 0, side::bid), "cancel id 3 (qty 0, same price)");
    check_eq(q.first(side::bid, px(101))->order_id, 1, "front unchanged");
    check_eq(q.last(side::bid, px(101))->order_id, 4, "back unchanged");
    check_eq(q.count_at(side::bid, px(101)), 2, "count after the middle removal");
    check(q.find(3) == nullptr, "cancelled order is gone");

    // A price change carrying qty 0 is a fill: erased, never re-inserted (:1594-1601).
    check(q.upsert(4, px(99), 0, 0, side::bid), "price move with qty 0");
    check(q.find(4) == nullptr, "order erased when filled during a move");
    check_eq(q.count_at(side::bid, px(99)), 0, "no phantom level is left behind");

    // A *new* order with qty 0 is still stored: legacy has no quantity test in that branch.
    check(q.upsert(5, px(105), 0, NOT_SET, side::bid), "insert a new order with qty 0");
    check(q.find(5) != nullptr, "qty-0 insert retained (faithful to :1585-1589)");

    // The two sides are independent.
    check(q.upsert(6, px(100), 4, 1, side::ask), "insert on the ask side");
    check_eq(q.count_at(side::bid, px(100)), 1, "ask insert leaves the bid level alone");
    check_eq(q.count_at(side::ask, px(100)), 1, "ask level has the order");

    // Queue-position semantics (:1611-1642).
    check(!q.set_queue_position(4242, 5), "unknown order rejected");
    check(q.set_queue_position(1, NOT_SET), "NOT_SET accepted");
    check_eq(q.find(1)->qp, -1, "NOT_SET (-1) is preserved, not clamped");
    check(q.set_queue_position(1, -5), "negative accepted");
    check_eq(q.find(1)->qp, 0, "anything below NOT_SET clamps to 0");
    check(q.set_queue_position(1, 12), "positive accepted");
    check_eq(q.find(1)->qp, 12, "positive queue position stored");

    // Walking a level from `first` via `next` sees every order.
    uint32_t walked = 0;
    for (const auto* s = q.first(side::bid, px(101)); s != nullptr; s = q.next(*s)) {
        check(s->side == static_cast<uint8_t>(side::bid), "walked order is a bid");
        ++walked;
    }
    check_eq(walked, q.count_at(side::bid, px(101)), "walk length matches count_at");
    check(q.first(side::bid, px(77)) == nullptr, "unknown price has no orders");
    check(q.last(side::ask, px(77)) == nullptr, "unknown price has no back");
}

// ---------------------------------------------------------------------------
// order_queue: randomised property test against an independent transcription of the legacy
// containers (std::unordered_map + per-level std::vector). This is what catches an
// open-addressing, free-list or level-table defect that hand-written cases miss.
// ---------------------------------------------------------------------------
struct ref_order {
    price_t price = 0;
    qty_t qty = 0;
    qty_t qp = 0;
    side s = side::bid;
};

struct ref_queue {  // the legacy data structures and control flow, verbatim
    std::unordered_map<uint32_t, ref_order> orders;
    std::map<std::pair<int, price_t>, std::vector<uint32_t>> levels;

    void unlink(side s, price_t price, uint32_t id) {
        const auto key = std::make_pair(static_cast<int>(s), price);
        auto lit = levels.find(key);
        if (lit == levels.end()) return;
        auto& v = lit->second;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (*it == id) {
                v.erase(it);
                break;
            }
        }
        if (v.empty()) levels.erase(lit);
    }

    void upsert(uint32_t id, price_t price, qty_t qty, qty_t qp, side s) {
        auto it = orders.find(id);
        if (it == orders.end()) {
            orders.emplace(id, ref_order{price, qty, qp, s});
            levels[{static_cast<int>(s), price}].push_back(id);
            return;
        }
        ref_order& o = it->second;
        if (o.price != price) {
            unlink(o.s, o.price, id);
            if (qty != 0) {
                o.price = price;
                o.qty = qty;
                o.qp = qp;
                levels[{static_cast<int>(o.s), price}].push_back(id);
            } else {
                orders.erase(it);
            }
        } else if (qty == 0) {
            unlink(o.s, o.price, id);
            orders.erase(it);
        } else {
            o.qty = qty;
            o.qp = qp;
        }
    }

    void set_queue_position(uint32_t id, qty_t qp) {
        auto it = orders.find(id);
        if (it == orders.end()) return;
        it->second.qp = qp;
        if (it->second.qp < order_queue::NOT_SET) it->second.qp = 0;
    }
};

void compare_state(const order_queue& q, const ref_queue& ref, int step) {
    const std::string tag = "step " + std::to_string(step);
    check_eq(q.size(), static_cast<long long>(ref.orders.size()), tag + ": size");

    for (uint32_t id = 0; id < 96; ++id) {
        const order_queue::slot* slot = q.find(id);
        const auto it = ref.orders.find(id);
        const bool in_ref = it != ref.orders.end();
        if ((slot != nullptr) != in_ref) {
            ++g_failures;
            std::printf("  FAIL  %s: presence of order %u differs\n", tag.c_str(), id);
            continue;
        }
        if (slot == nullptr) continue;
        check_eq(slot->price, it->second.price, tag + ": price of " + std::to_string(id));
        check_eq(slot->qty, it->second.qty, tag + ": qty of " + std::to_string(id));
        check_eq(slot->qp, it->second.qp, tag + ": qp of " + std::to_string(id));
        check_eq(static_cast<int>(slot->side), static_cast<int>(it->second.s),
                 tag + ": side of " + std::to_string(id));
    }

    for (int sd = 0; sd < static_cast<int>(SIDE_COUNT); ++sd) {
        for (int t = 0; t < 8; ++t) {
            const price_t price = px(100 + t);
            const side s = static_cast<side>(sd);
            std::vector<uint32_t> walked;
            for (const auto* cur = q.first(s, price); cur != nullptr; cur = q.next(*cur)) {
                walked.push_back(cur->order_id);
            }
            const auto lit = ref.levels.find({sd, price});
            const bool same = lit == ref.levels.end() ? walked.empty() : (walked == lit->second);
            if (!same) {
                ++g_failures;
                std::printf("  FAIL  %s: level %d/%lld order differs\n", tag.c_str(), sd,
                            (long long)price);
            }
            check_eq(q.count_at(s, price), static_cast<long long>(walked.size()),
                     tag + ": count_at consistency");
        }
    }
}

void test_order_queue_property() {
    std::printf("order_queue: randomised property test (5000 ops)\n");

    std::mt19937 rng(12345);
    order_queue q;
    ref_queue ref;

    for (int step = 0; step < 5000; ++step) {
        const uint32_t id = rng() % 96;  // small id space, so updates and collisions are frequent
        const price_t price = px(100 + static_cast<int>(rng() % 8));
        const qty_t qty = (rng() % 6 == 0) ? 0 : 1 + static_cast<qty_t>(rng() % 40);
        const side s = (rng() & 1) ? side::bid : side::ask;
        // Include NOT_SET and illegal negatives to exercise the clamping rule.
        const qty_t qp =
            (rng() % 5 == 0) ? order_queue::NOT_SET : static_cast<qty_t>(rng() % 60) - 2;

        if (rng() % 4 == 0) {
            q.set_queue_position(id, qp);
            ref.set_queue_position(id, qp);
        } else {
            q.upsert(id, price, qty, qp, s);
            ref.upsert(id, price, qty, qp, s);
        }
        compare_state(q, ref, step);
    }
    check(!q.overflowed(), "randomised workload never overflowed the caps");
}

// ---------------------------------------------------------------------------
// order_queue: caps are hit on the cold path, never by allocating
// ---------------------------------------------------------------------------
void test_order_queue_capacity() {
    std::printf("order_queue: capacity paths\n");

    // Slot exhaustion: one price level, so only the arena can run out.
    {
        order_queue q;
        uint32_t inserted = 0;
        for (uint32_t i = 0; i < MAX_ORDERS; ++i) {
            if (q.upsert(1000 + i, px(100), 1, 0, side::bid)) ++inserted;
        }
        check_eq(inserted, MAX_ORDERS, "arena holds exactly MAX_ORDERS orders");
        check(!q.upsert(999999, px(100), 1, 0, side::bid), "the next insert is refused");
        check(q.overflowed(), "overflow is reported");
        check_eq(q.size(), MAX_ORDERS, "a refused insert does not change the size");
        for (uint32_t i = 0; i < MAX_ORDERS; i += 97) {
            check(q.find(1000 + i) != nullptr, "existing orders are intact");
        }
        check_eq(q.count_at(side::bid, px(100)), MAX_ORDERS, "level count preserved");
    }

    // Level-table exhaustion: distinct prices on one side.
    {
        order_queue q;
        uint32_t inserted = 0;
        for (uint32_t i = 0; i < MAX_LEVEL_SLOTS; ++i) {
            if (q.upsert(2000 + i, px(1000 + static_cast<int>(i)), 1, 0, side::bid)) ++inserted;
        }
        check_eq(inserted, MAX_LEVEL_SLOTS, "one order per distinct price up to the level cap");
        check(!q.upsert(9999, px(2000), 1, 0, side::bid), "one more distinct price is refused");
        check(q.overflowed(), "level overflow is reported");
        check_eq(q.size(), MAX_LEVEL_SLOTS, "size unchanged by the refused insert");
        check(q.find(9999) == nullptr, "the refused order is not visible");
    }

    // The arena really is recycled: the same id can leave and come back many times over.
    {
        order_queue q;
        for (uint32_t round = 0; round < 4 * MAX_ORDERS; ++round) {
            check(q.upsert(7, px(100), 5, 0, side::bid), "reinsert a single order repeatedly");
            q.upsert(7, px(100), 0, 0, side::bid);  // cancel
        }
        check_eq(q.size(), 0, "everything cancelled");
        check(!q.overflowed(), "recycling never overflows");
    }
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    test_book_change_indices();
    test_book_level_lookup();
    test_book_trade();
    test_order_queue_branches();
    test_order_queue_property();
    test_order_queue_capacity();

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("all MM state-layer checks passed\n");
    return EXIT_SUCCESS;
}
