#pragma once

// Binds AS's policy seams onto the shared liquidity ledger, and nothing else: the engine stays
// testable without a ledger, and this is the only place reservation accounting happens.
//
// The mapping onto `common::liquidity_ledger`, from the legacy `Market` fields:
//
//   _asQuoteAdjustment  (per book)          -> one quote group (AGGRESSIVE_QUOTE on the book it
//                                             advertises on, ANTICIPATED_HEDGE on every book the
//                                             hedge would touch) — exactly what
//                                             create_quote_group() builds
//   _fakPreAdjustment   (per lean book)     -> the group's ANTICIPATED_HEDGE reservations
//   _fakSizeAdjustment  (per lean book)     -> HEDGE reservations (one per hedge order)
//   getAdjustedSize()                       -> available(book, all reservations)
//   getAdjustedSizeExcludeFAKPreAdj()       -> available(book, excluding ANTICIPATED_HEDGE)
//   update() price change                   -> invalidate_market(book)

#include <common/liquidity_ledger.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "limits.hpp"
#include "policy.hpp"
#include "types.hpp"

namespace tachyon::trade::as {

class ledger_bridge {
public:
    ledger_bridge(liquidity_ledger& ledger, uint32_t n_contracts) : ledger_(&ledger) {
        slots_.resize(static_cast<size_t>(n_contracts) * SIDE_COUNT * 2);
        groups_.resize(static_cast<size_t>(n_contracts) * SIDE_COUNT, group_slot{});
    }

    // Wire one book to its ledger key and the source that currently supplies it. The source
    // matters for HEDGE reservations: only the ones belonging to the supplying source are counted.
    void set_market(uint32_t contract, side sd, bool external, market_key key,
                    liquidity_source source) noexcept {
        slot& s = at(contract, sd, external);
        s.key = key;
        s.source = source;
        s.wired = true;
    }

    // ---- quote_policy hooks ----------------------------------------------------
    // legacy Market::getAdjustedSize / getAdjustedSizeExcludeFAKPreAdj.
    static qty_t adjusted_size(void* ctx, uint32_t contract, side sd, bool external,
                               size_filter filter) noexcept {
        return static_cast<ledger_bridge*>(ctx)->available(contract, sd, external, filter);
    }

    qty_t available(uint32_t contract, side sd, bool external, size_filter filter) const noexcept {
        const slot& s = at(contract, sd, external);
        if (!s.wired) return book_size(contract, sd, external);
        reservation_filter rf{};
        rf.source = s.source;
        rf.exclude_type = filter.exclude_mask;
        return ledger_->available(s.key, book_size(contract, sd, external), rf);
    }

    // legacy Market::update(): a replaced level releases what it held.
    static void price_changed(void* ctx, uint32_t contract, side sd, bool external,
                              qty_t) noexcept {
        static_cast<ledger_bridge*>(ctx)->invalidate(contract, sd, external);
    }

    void invalidate(uint32_t contract, side sd, bool external) noexcept {
        const slot& s = at(contract, sd, external);
        if (s.wired) ledger_->invalidate_market(s.key, nullptr);
    }

    // legacy Market::getASQuoteAdjustment() — the quantity AS itself reserved on this book.
    static qty_t as_quote_adjustment(void* ctx, uint32_t contract, side sd, bool external) noexcept {
        if (!external) return 0;  // AS only ever advertises on the external book
        const ledger_bridge* b = static_cast<const ledger_bridge*>(ctx);
        return b->groups_[contract * SIDE_COUNT + to_index(sd)].quantity;
    }

    // legacy Market::undoAsQuoteAdjustment() — release up to `qty` (0 == everything).
    static bool undo_as_quote(void* ctx, uint32_t contract, side sd, bool external,
                              qty_t qty) noexcept {
        if (!external) return false;
        return static_cast<ledger_bridge*>(ctx)->release(contract, sd, qty);
    }

    // ---- the aggressive quote itself -------------------------------------------
    // legacy Contract::addAggressiveQuoteAdjustment (:1016-1044): a quote that trades through the
    // opposite external book reserves that liquidity and pre-reserves the hedge it implies.
    // `quote_side` is the side AS is quoting; the reservation lands on the opposite book.
    void set_aggressive_quote(uint32_t contract, side quote_side, qty_t quantity,
                              std::span<const market_key> hedge_markets) noexcept {
        const side advertised = other(quote_side);
        group_slot& g = groups_[contract * SIDE_COUNT + to_index(advertised)];
        const slot& s = at(contract, advertised, true);
        if (g.id != 0) {
            if (quantity == 0) {
                ledger_->release_group(g.id, nullptr);
                g = group_slot{};
                return;
            }
            if (!ledger_->resize_quote_group(g.id, quantity)) return;
            g.quantity = quantity;
            return;
        }
        if (quantity == 0 || !s.wired) return;
        const uint32_t id = ledger_->create_quote_group(s.key, s.source, quantity, hedge_markets);
        if (id != 0) {
            g.id = id;
            g.quantity = quantity;
        }
    }

    bool release(uint32_t contract, side sd, qty_t qty) noexcept {
        group_slot& g = groups_[contract * SIDE_COUNT + to_index(sd)];
        if (g.id == 0 || g.quantity == 0) return false;
        const qty_t remaining = (qty == 0 || qty >= g.quantity) ? 0 : g.quantity - qty;
        if (remaining == 0) {
            const bool released = ledger_->release_group(g.id, nullptr);
            g = group_slot{};
            return released;
        }
        if (!ledger_->resize_quote_group(g.id, remaining)) return false;
        g.quantity = remaining;
        return true;
    }

    // ---- hedge reservations ----------------------------------------------------
    // legacy Market::addFAKAdjustment(hedgeId, qty) on the lean's best book.
    reservation_id reserve_hedge(uint32_t index, qty_t qty, uint32_t hedge_id) noexcept {
        if (index >= slots_.size()) return reservation_id{};
        const slot& s = slots_[index];
        if (!s.wired || qty <= 0) return reservation_id{};
        return ledger_->reserve_hedge(s.key, s.source, hedge_id, qty);
    }

    // legacy handleFakDone / HedgeMDAdjsutAck: the external hedge came back with a possibly
    // smaller quantity, then completes when it is done.
    bool ack_hedge(uint32_t hedge_id, market_key target, ord_id_t order_id, qty_t actual) noexcept {
        return ledger_->acknowledge_external(hedge_id, target, order_id, actual);
    }
    bool complete_hedge(uint32_t hedge_id, market_key target) noexcept {
        return ledger_->complete_by_hedge_id(hedge_id, target);
    }

    market_key key_at(uint32_t index) const noexcept {
        return index < slots_.size() ? slots_[index].key : market_key{0};
    }

    std::size_t active_reservations() const noexcept { return ledger_->active_reservation_count(); }

    // The host owns the books, so it supplies the displayed size the ledger deducts from.
    void set_book_size_source(qty_t (*fn)(void*, uint32_t, side, bool), void* ctx) noexcept {
        book_size_fn_ = fn;
        book_size_ctx_ = ctx;
    }

    static quote_policy make_policy(ledger_bridge& bridge) noexcept {
        quote_policy p{};
        p.adjusted_size = &ledger_bridge::adjusted_size;
        p.adjusted_ctx = &bridge;
        p.price_changed = &ledger_bridge::price_changed;
        p.price_changed_ctx = &bridge;
        p.as_quote_adjustment = &ledger_bridge::as_quote_adjustment;
        p.as_quote_ctx = &bridge;
        p.undo_as_quote = &ledger_bridge::undo_as_quote;
        p.undo_ctx = &bridge;
        return p;
    }

private:
    struct slot {
        market_key key = 0;
        liquidity_source source = liquidity_source::__LIQUIDITY_SOURCE_END__;
        bool wired = false;
    };
    struct group_slot {
        uint32_t id = 0;
        qty_t quantity = 0;
    };

    qty_t book_size(uint32_t contract, side sd, bool external) const noexcept {
        return book_size_fn_ ? book_size_fn_(book_size_ctx_, contract, sd, external) : 0;
    }

    slot& at(uint32_t contract, side sd, bool external) noexcept {
        return slots_[market_index(contract, sd, external)];
    }
    const slot& at(uint32_t contract, side sd, bool external) const noexcept {
        return slots_[market_index(contract, sd, external)];
    }

    liquidity_ledger* ledger_ = nullptr;
    std::vector<slot> slots_;
    std::vector<group_slot> groups_;
    qty_t (*book_size_fn_)(void*, uint32_t, side, bool) = nullptr;
    void* book_size_ctx_ = nullptr;
};

}  // namespace tachyon::trade::as
