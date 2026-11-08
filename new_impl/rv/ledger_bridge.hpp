#pragma once

// Bridges the engine's seams (`quote_policy`, `order_port`) to the shared market /
// liquidity-ledger objects in new_impl/common. This is the only place where the
// engine touches reservation accounting, so the cycle stays testable without it.

#include <common/liquidity_ledger.hpp>
#include <common/market.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "quotes.hpp"
#include "strategy.hpp"
#include "types.hpp"

namespace tachyon::trade::rv {

class ledger_bridge {
public:
    ledger_bridge(liquidity_ledger& ledger, uint32_t n_contracts) : ledger_(&ledger) {
        slots_.resize(static_cast<size_t>(n_contracts) * SIDE_COUNT * 2);
        for (auto& s : slots_) {
            s = slot{};
        }
    }

    // Wire one market book to its ledger key and the source currently supplying it.
    // `source` matters for HEDGE reservations: only reservations belonging to the
    // source that supplies the displayed size are counted (see market::adjusted_size).
    void set_market(uint32_t contract, side sd, bool external, market_key key,
                    liquidity_source source) noexcept {
        slot& s = at(contract, sd, external);
        s.key = key;
        s.source = source;
        s.wired = true;
    }

    // quote_policy::adjusted_size — displayed size minus outstanding reservations.
    static qty_t adjusted_size(void* ctx, uint32_t contract, side sd, bool external,
                               qty_t raw) noexcept {
        return static_cast<ledger_bridge*>(ctx)->available(contract, sd, external, raw);
    }

    qty_t available(uint32_t contract, side sd, bool external, qty_t raw) const noexcept {
        const slot& s = at(contract, sd, external);
        if (!s.wired || raw == 0) {
            return raw;
        }
        reservation_filter filter{};
        filter.source = s.source;
        return ledger_->available(s.key, raw, filter);
    }

    // order_port::send_hedge — claim the liquidity a hedge is about to consume, so
    // subsequent quote sizing in the same cycle sees it (legacy addFAKAdjustment).
    static void send_hedge(void* ctx, const hedge_order& order) noexcept {
        static_cast<ledger_bridge*>(ctx)->on_hedge(order);
    }

    // quote_policy::price_changed — a replaced price level drops its reservations
    // (legacy Market::update flushes FAK adjustments, Common.h:636-644).
    static void price_changed(void* ctx, uint32_t contract, side sd, bool external) noexcept {
        static_cast<ledger_bridge*>(ctx)->invalidate(contract, sd, external);
    }

    // order_port::send_quote — the legacy host forwards the quote request; the bridge
    // only records that blocked quotes were released (no ledger effect).
    static void send_quote(void* ctx, uint32_t /*contract*/) noexcept {
        static_cast<ledger_bridge*>(ctx)->quotes_resent_++;
    }

    const reservation_id on_hedge(const hedge_order& order) noexcept {
        // The reservation belongs to the market that supplies the displayed liquidity,
        // i.e. the lean market on the quote side -- which the engine already encodes in
        // order.market_index (legacy: hedgeMarket.addFAKAdjustment, :2512).
        if (order.market_index >= slots_.size()) {
            return reservation_id{};
        }
        const slot& s = slots_[order.market_index];
        if (!s.wired) {
            return reservation_id{};
        }
        return ledger_->reserve_hedge(s.key, s.source, order.hedge_id, order.size);
    }

    // Hedge lifecycle (legacy handleFakDone / ack): the external order came back with a
    // possibly smaller size, then completes when it is done.
    bool ack_hedge(uint32_t hedge_id, market_key target, ord_id_t order_id, qty_t actual) noexcept {
        return ledger_->acknowledge_external(hedge_id, target, order_id, actual);
    }
    bool complete_hedge(uint32_t hedge_id, market_key target) noexcept {
        return ledger_->complete_by_hedge_id(hedge_id, target);
    }

    // The market book changed price: release reservations it invalidated.
    void invalidate(uint32_t contract, side sd, bool external,
                    std::vector<market_key>* affected = nullptr) noexcept {
        const slot& s = at(contract, sd, external);
        if (s.wired) {
            ledger_->invalidate_market(s.key, affected);
        }
    }

    uint64_t quotes_resent() const noexcept { return quotes_resent_; }
    std::size_t active_reservations() const noexcept { return ledger_->active_reservation_count(); }

    // Ledger key for a market index (host bookkeeping: a hedge ack arrives with only the
    // hedge id, so the host records which book each reservation was taken against).
    market_key key_at(uint32_t index) const noexcept {
        return index < slots_.size() ? slots_[index].key : market_key{0};
    }
    bool wired_at(uint32_t index) const noexcept {
        return index < slots_.size() && slots_[index].wired;
    }

    static quote_policy make_policy(ledger_bridge& bridge) noexcept {
        quote_policy policy{};
        policy.adjusted_size = &ledger_bridge::adjusted_size;
        policy.adjusted_ctx = &bridge;
        policy.price_changed = &ledger_bridge::price_changed;
        policy.price_changed_ctx = &bridge;
        return policy;
    }

    static order_port make_port(ledger_bridge& bridge) noexcept {
        order_port port{};
        port.send_hedge = &ledger_bridge::send_hedge;
        port.send_quote = &ledger_bridge::send_quote;
        port.ctx = &bridge;
        return port;
    }

private:
    struct  slot {
        market_key key = 0;
        liquidity_source source = liquidity_source::__LIQUIDITY_SOURCE_END__;
        bool wired = false;
    };

    slot& at(uint32_t contract, side sd, bool external) noexcept {
        return slots_[market_index(contract, sd, external)];
    }
    const slot& at(uint32_t contract, side sd, bool external) const noexcept {
        return slots_[market_index(contract, sd, external)];
    }

    liquidity_ledger* ledger_ = nullptr;
    std::vector<slot> slots_;
    uint64_t quotes_resent_ = 0;
};

}  // namespace tachyon::trade::rv
