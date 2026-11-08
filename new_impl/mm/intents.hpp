#pragma once

#include <array>
#include <cstdint>

#include "limits.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// What one cycle asks the host to send. The engine never touches a ring buffer: it appends
// *intents* in the exact order legacy calls modifyQuoteOrder / modifyHedgeOrder, and the host
// forwards them (`sendQuoteRequest` / `sendHedgeRequest`, then one `notifyOrderRequest`).
//
// Ordering is behaviour, not an implementation detail — the cross checks re-emit the *other*
// instrument (:512-570), a hedge flip resets the opposite hedge (:1119-1215), and the spray
// inserts one more order at the level — so the gate compares this list, not just the final
// prices.
struct order_intent {
    enum class kind : uint8_t { quote, hedge };

    kind type = kind::quote;
    uint32_t contract = 0;
    side sd = side::bid;

    // quote (legacy OrderRequest::Quote, Common.h:1153-1160)
    price_t internal_price = tachyon::NULL_PRICE;
    qty_t internal_size = 0;
    price_t external_price = tachyon::NULL_PRICE;
    qty_t external_size = 0;

    // hedge (legacy OrderRequest::Hedge, Common.h:1162-1171)
    qty_t size = 0;      // legacy _sizeToQuote
    bool in_payup = false;

    // quote only: whether the external side persists (`persist` in sendQuoteRequest). Legacy
    // passes `quoteExternally` for a quote and `true` for a hedge (:878-893, :1137-1146).
    bool persist = false;
};

// Fixed-capacity, allocation-free per-cycle intent list.
class intent_log {
public:
    void clear() noexcept {
        count_ = 0;
        overflowed_ = false;
    }

    void add_quote(uint32_t contract, side sd, price_t internal_price, qty_t internal_size,
                   price_t external_price, qty_t external_size, bool persist) noexcept {
        if (count_ == MAX_INTENTS) {
            overflowed_ = true;
            return;
        }
        order_intent& it = items_[count_++];
        it = order_intent{};
        it.type = order_intent::kind::quote;
        it.contract = contract;
        it.sd = sd;
        it.internal_price = internal_price;
        it.internal_size = internal_size;
        it.external_price = external_price;
        it.external_size = external_size;
        it.persist = persist;
    }

    void add_hedge(uint32_t contract, side sd, price_t internal_price, price_t external_price,
                   qty_t size, bool in_payup) noexcept {
        if (count_ == MAX_INTENTS) {
            overflowed_ = true;
            return;
        }
        order_intent& it = items_[count_++];
        it = order_intent{};
        it.type = order_intent::kind::hedge;
        it.contract = contract;
        it.sd = sd;
        it.internal_price = internal_price;
        it.external_price = external_price;
        it.size = size;
        it.in_payup = in_payup;
        it.persist = true;  // hedge orders are always persistent LMT (:1141)
    }

    uint32_t size() const noexcept { return count_; }
    const order_intent& at(uint32_t i) const noexcept { return items_[i]; }
    bool empty() const noexcept { return count_ == 0; }
    bool overflowed() const noexcept { return overflowed_; }

private:
    std::array<order_intent, MAX_INTENTS> items_{};
    uint32_t count_ = 0;
    bool overflowed_ = false;
};

}  // namespace tachyon::trade::mm
