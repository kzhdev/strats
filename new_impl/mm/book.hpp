#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "limits.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// A single book price level. Layout mirrors md::top_t::level_t
// {pr, q, o, ext.implied.impliedQtyNotTrackedByQueuePosition}, of which the strategy reads
// pr (`:410-421`), q (`:438-446`, `:795`), o (the payup order-count test, `:359`) and
// ext.implied (`:795`) — 24 bytes against the framework's ~40, so the ten-level copy that the
// feed path performs stays inside two cache lines.
struct level {
    price_t price = tachyon::NULL_PRICE;
    qty_t qty = 0;              // legacy level.q
    uint32_t order_count = 0;   // legacy level.o
    // Internal-matching-engine quantity that the feed's order count does not cover. The
    // strategy subtracts it from the displayed size when it measures the queue ahead
    // (`targetSize`, SimpleMarketMaker.h:795), because our orders cannot be in front of
    // liquidity that never entered the queue.
    qty_t implied_untracked = 0;

    void reset() noexcept {
        price = tachyon::NULL_PRICE;
        qty = 0;
        order_count = 0;
        implied_untracked = 0;
    }
};

// One side of a contract's book: BOOK_LEVELS levels plus the "which level changed" indices
// that decide how much of the cycle runs (`onMarketDataChange`, SimpleMarketMaker.h:484-505,
// and `updateQuotesBySide`, :603-628).
//
// The class owns the book *arithmetic*, not the transport: it is fed either a snapshot or a
// trade adjustment and answers the three questions the engine asks — where is this price
// (`level_index`), what displayed size is the queue measured against (`tracked_qty`), and what
// changed since the last cycle (`*_change_index`). That keeps the one-based/unsigned
// bookkeeping of MarketDataConsumer.h:129-222 in one place instead of spread across a template
// helper and an int8 array inside the strategy object.
class book {
public:
    static constexpr int8_t NO_CHANGE = -1;

    // `side` decides which direction counts as "better"; set once at init.
    void init(side s) noexcept {
        side_ = s;
        for (level& l : levels_) l.reset();
        clear_change_indices();
    }

    // ---- change tracking ----------------------------------------------------
    // Records a feed update from its 1-based level numbers (0 = that field did not change).
    // Transcribed from copyBooks' checkAndSetChangeIndex (MarketDataConsumer.h:129-152): the
    // index becomes num - 1, and only the *smallest* index seen since the last reset is kept,
    // because a change deeper in the book makes everything in front of it suspect. Returns
    // false when the feed reports a level beyond the book, which legacy turns into
    // `success = false`.
    bool note_change(uint32_t updated_price_num, uint32_t updated_qty_num) noexcept {
        if (updated_price_num > BOOK_LEVELS || updated_qty_num > BOOK_LEVELS) return false;
        merge_index(price_change_index_, updated_price_num);
        merge_index(qty_change_index_, updated_qty_num);
        return true;
    }

    // Copies a snapshot. Legacy memcpy's the whole md::top_t only when the quantity index is set
    // (copyBooks, :157-166), so the caller applies it on the same condition; levels beyond
    // `count` are cleared, matching the feed's NULL-padded levels.
    void apply_snapshot(const level* snapshot, uint32_t count) noexcept {
        const uint32_t n = count < BOOK_LEVELS ? count : BOOK_LEVELS;
        for (uint32_t i = 0; i < n; ++i) levels_[i] = snapshot[i];
        for (uint32_t i = n; i < BOOK_LEVELS; ++i) levels_[i].reset();
    }

    // Trade-driven adjustment, transcribed from handleMDTrade
    // (MarketDataConsumer.h:175-222) for this side: a print at our best level only reduces it,
    // a print better than our best pushes the book back, and a print below the book's last
    // level collapses the front.
    //
    // Fidelity note: legacy's copyBook writes only pr/q/o, so `ext.implied` keeps its previous
    // value here, and the "traded through" branches clear only pr and q. Both are reproduced
    // deliberately — implied quantity is only meaningful on a level that still has a price.
    void apply_trade(price_t trade_price, qty_t trade_qty, uint32_t order_count) noexcept {
        const auto write_best = [this, trade_price, trade_qty, order_count]() {
            level& b = levels_[0];
            b.price = trade_price;
            b.qty = trade_qty;
            b.order_count = order_count;
        };

        if (trade_price == levels_[0].price) {
            qty_change_index_ = 0;
            write_best();
            return;
        }

        price_change_index_ = 0;
        qty_change_index_ = 0;

        if (is_better_price(trade_price, levels_[0].price, side_)) {
            std::memmove(&levels_[1], &levels_[0], sizeof(level) * (BOOK_LEVELS - 1));
            write_best();
            return;
        }

        if (is_better_price(trade_price, levels_[BOOK_LEVELS - 1].price, side_)) {
            uint32_t i = 1;
            for (; i < BOOK_LEVELS; ++i) {
                if (trade_price == levels_[i].price) break;
            }
            if (i < BOOK_LEVELS - 1) {
                std::memmove(&levels_[1], &levels_[i + 1], sizeof(level) * (BOOK_LEVELS - i - 1));
                levels_[BOOK_LEVELS - i].price = tachyon::NULL_PRICE;
                levels_[BOOK_LEVELS - i].qty = 0;
            } else {
                levels_[1].price = tachyon::NULL_PRICE;
                levels_[1].qty = 0;
            }
            write_best();
            return;
        }

        // Traded through the whole book.
        write_best();
        levels_[1].price = tachyon::NULL_PRICE;
        levels_[1].qty = 0;
    }

    // ---- queries ------------------------------------------------------------

    // Legacy getPriceLevelIndex (:407-422): -1 when the price is NULL, better than our best, or
    // absent. The scan stops at the first NULL level, so a NULL-padded book never matches.
    int32_t level_index(price_t price) const noexcept {
        if (price == tachyon::NULL_PRICE || is_better_price(price, levels_[0].price, side_)) {
            return -1;
        }
        for (uint32_t i = 0; i < BOOK_LEVELS; ++i) {
            if (levels_[i].price == tachyon::NULL_PRICE) return -1;
            if (price == levels_[i].price) return static_cast<int32_t>(i);
        }
        return -1;
    }

    const level& top() const noexcept { return levels_[0]; }
    const level& at(uint32_t index) const noexcept { return levels_[index]; }
    const level* levels() const noexcept { return levels_.data(); }
    bool has_price() const noexcept { return levels_[0].price != tachyon::NULL_PRICE; }
    side which_side() const noexcept { return side_; }

    // Displayed size minus the implied quantity our orders cannot be behind (:795).
    qty_t tracked_qty(uint32_t index) const noexcept {
        return levels_[index].qty - levels_[index].implied_untracked;
    }

    int8_t price_change_index() const noexcept { return price_change_index_; }
    int8_t qty_change_index() const noexcept { return qty_change_index_; }
    bool qty_changed() const noexcept { return qty_change_index_ != NO_CHANGE; }
    // "top of book changed" (:436): the size at level 0 moved.
    bool top_changed() const noexcept { return qty_change_index_ == 0; }
    void clear_change_indices() noexcept {
        price_change_index_ = NO_CHANGE;
        qty_change_index_ = NO_CHANGE;
    }

private:
    static void merge_index(int8_t& index, uint32_t updated_num) noexcept {
        if (updated_num == 0 || updated_num > BOOK_LEVELS) return;  // 0 => no change
        const int8_t candidate = static_cast<int8_t>(updated_num - 1);
        if (index == NO_CHANGE || candidate < index) index = candidate;
    }

    std::array<level, BOOK_LEVELS> levels_{};
    int8_t price_change_index_ = NO_CHANGE;
    int8_t qty_change_index_ = NO_CHANGE;
    side side_ = side::bid;
};

// Legacy isInPayUp (:356-361): are we so close to the front of the queue that we should chase the
// touch? Shared by the quoting and hedging halves so there is exactly one transcription; the
// three parameters are the payup configuration (payUpQty, payUpOrders, payUpMultiple).
inline bool in_payup(qty_t payup_qty, uint32_t payup_orders, double payup_multiple,
                     qty_t our_size, const level& book) noexcept {
    const qty_t based_on_size = static_cast<qty_t>(our_size * payup_multiple);
    const qty_t threshold = std::max(payup_qty, based_on_size);
    return (book.qty <= threshold) |
           (static_cast<qty_t>(book.order_count) <= static_cast<qty_t>(payup_orders));
}

}  // namespace tachyon::trade::mm
