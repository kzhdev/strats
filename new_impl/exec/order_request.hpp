#pragma once

#include <cstdint>

#include "types.hpp"

// The one object that crosses the strategy -> executor boundary.
//
// It is a 64-byte POD on purpose: one mailbox entry is exactly one cache line, so a push writes
// one line and a drain reads lines sequentially (PLAN.md §3). The layout is the union of the
// legacy OrderRequest::Quote and OrderRequest::Hedge (Common.h:1151-1189) minus the one field the
// new strategies do not produce (see D-EX10 in PLAN.md).
namespace tachyon::trade::exec {

struct order_request {
    enum flags_t : uint8_t {
        flag_persist = 1u << 0,     // quote: keep the external price when the size reaches zero
        flag_pay_up = 1u << 1,      // hedge: the strategy asked to pay up (legacy _inPayUp)
        flag_external = 1u << 2,    // hedge: the remainder goes to the exchange (legacy _isExternal)
        flag_lean_on_rv = 1u << 3,  // quote: the price leans on an RV quote (legacy _leanOnRV)
    };

    // ---- payload ----
    price_t internal_price = NULL_PRICE;
    price_t external_price = NULL_PRICE;
    qty_t internal_size = 0;
    qty_t external_size = 0;
    qty_t hedge_size = 0;  // legacy OrderRequest::Hedge::_size
    // ---- identity ----
    uint32_t contract = INVALID_CONTRACT;
    uint32_t hedge_id = 0;    // hedge: echoed back on the ack/fill (legacy _hedgeId)
    uint32_t market_id = 0;   // hedge: which book the strategy hedged in (legacy _marketId)
    uint32_t exec_token = 0;  // MM: staleness token (legacy _orderExecutionToken)
    uint32_t seq = 0;         // producer sequence, for traces and tests
    uint8_t strategy = 0;
    uint8_t kind = 0;
    uint8_t sd = 0;
    uint8_t flags = 0;

    constexpr strategy_id strategy_id_value() const noexcept {
        return static_cast<strategy_id>(strategy);
    }
    constexpr request_kind kind_value() const noexcept { return static_cast<request_kind>(kind); }
    constexpr side side_value() const noexcept { return static_cast<side>(sd); }

    constexpr request_key key() const noexcept {
        return request_key{strategy_id_value(), kind_value(), side_value()};
    }

    constexpr bool persist() const noexcept { return (flags & flag_persist) != 0; }
    constexpr bool pay_up() const noexcept { return (flags & flag_pay_up) != 0; }
    constexpr bool external() const noexcept { return (flags & flag_external) != 0; }
    constexpr bool lean_on_rv() const noexcept { return (flags & flag_lean_on_rv) != 0; }

    // A resting-stream request is one whose order becomes part of the internal book.
    constexpr bool rests() const noexcept {
        return rests_internally(strategy_id_value(), kind_value());
    }
};

static_assert(sizeof(order_request) == CACHE_LINE, "a mailbox entry must be one cache line");
static_assert(alignof(order_request) == 8, "order_request must be 8-byte aligned");

// Legacy OrderRequestManager::sendQuoteRequest (OrderRequestManager.h:69-108).
constexpr order_request make_quote(strategy_id strategy,
                                   uint32_t contract,
                                   side sd,
                                   price_t internal_price,
                                   qty_t internal_size,
                                   price_t external_price,
                                   qty_t external_size,
                                   bool persist,
                                   bool lean_on_rv = false,
                                   uint32_t exec_token = 0) noexcept {
    order_request r{};
    r.internal_price = internal_price;
    r.internal_size = internal_size;
    r.external_price = external_price;
    r.external_size = external_size;
    r.contract = contract;
    r.exec_token = exec_token;
    r.strategy = static_cast<uint8_t>(strategy);
    r.kind = static_cast<uint8_t>(request_kind::quote);
    r.sd = static_cast<uint8_t>(sd);
    r.flags = static_cast<uint8_t>((persist ? order_request::flag_persist : 0) |
                                   (lean_on_rv ? order_request::flag_lean_on_rv : 0));
    return r;
}

// Legacy OrderRequestManager::sendHedgeRequest (OrderRequestManager.h:110-141).
constexpr order_request make_hedge(strategy_id strategy,
                                   uint32_t contract,
                                   side sd,
                                   price_t internal_price,
                                   price_t external_price,
                                   qty_t size,
                                   bool external,
                                   bool pay_up,
                                   uint32_t hedge_id = 0,
                                   uint32_t market_id = 0,
                                   uint32_t exec_token = 0) noexcept {
    order_request r{};
    r.internal_price = internal_price;
    r.external_price = external_price;
    r.hedge_size = size;
    r.contract = contract;
    r.hedge_id = hedge_id;
    r.market_id = market_id;
    r.exec_token = exec_token;
    r.strategy = static_cast<uint8_t>(strategy);
    r.kind = static_cast<uint8_t>(request_kind::hedge);
    r.sd = static_cast<uint8_t>(sd);
    // Hedges are always persistent LMT/FAK (legacy passes `persist = true` for a hedge,
    // SimpleMarketMaker.h:1141); the external bit is what makes the remainder go out.
    r.flags = static_cast<uint8_t>(order_request::flag_persist |
                                   (external ? order_request::flag_external : 0) |
                                   (pay_up ? order_request::flag_pay_up : 0));
    return r;
}

}  // namespace tachyon::trade::exec
