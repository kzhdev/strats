#pragma once

// An independent transcription of the legacy matching engine, used only as a differential oracle.
//
// The point is that it shares no code with `implied_book`: it keeps the legacy containers
// (std::map keyed by price, a per-price PriceLevel holding a sorted std::vector<Order*>), and it
// reproduces the legacy control flow statement by statement from
// src/InternalMatchingEngine.h. If the new book and this transcription agree on a random request
// tape, the new book is behaving like the code it replaces; where they disagree, one of the two is
// wrong and the diff says exactly which field.
//
//   modifyOrder                        :329-378
//   insertOrder / removeOrder          :380-390
//   removeOrderByStrategy              :428-439
//   removeOrderFromLevel / addToPriceLevel  :443-505, :1058-1074
//   matchOrders (the cross)            :1076-1230
//   tryMatch (FAK)                     :881-957
//   doExternalReductionAfterInternalFill    :1043-1056
//   getPriceAndSize / getPersistedPrice     :835-869
//   getBestMarket (publication)        :1249-1272
//
// Deliberately *not* transcribed: banners, GTC, pay-up and economic numbers (deferred surface,
// PLAN.md D-EX6), and the executor around it.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "order_request.hpp"
#include "types.hpp"

namespace oracle {

using namespace tachyon::trade::exec;
using tachyon::trade::to_int;  // oracle lives outside tachyon::trade, so it must be named

constexpr uint8_t MAX_ORDER_ID = STRATEGY_COUNT * KIND_COUNT;  // legacy toUInt(StrategyId::MAX)*2

struct order {
    price_t price = NULL_PRICE;           // legacy _price
    qty_t size = 0;                       // legacy _size
    price_t external_price = NULL_PRICE;  // legacy _externalPrice
    qty_t external_size = 0;              // legacy _externalSize
    uint32_t id = 0;                      // legacy _id
    strategy_id strategy = strategy_id::MM;
    request_kind kind = request_kind::quote;
    side sd = side::bid;
    bool in_book = false;   // present in the internal map
    bool in_level = false;  // present in the external level
    bool pay_up = false;
    bool lean_on_rv = false;

    bool matchable() const noexcept { return in_book && size > 0; }
};

// Legacy PriceLevel (Common.h:1100-1148): a vector kept sorted by external size, descending.
struct price_level {
    price_t price = NULL_PRICE;
    std::vector<order*> orders;

    qty_t get_size() const noexcept { return orders.empty() ? 0 : orders.front()->external_size; }

    void add(order* o) {
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            if (o->external_size > (*it)->external_size) {
                orders.insert(it, o);
                return;
            }
        }
        orders.push_back(o);
    }

    void remove(order* o) {
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            if (*it == o) {
                orders.erase(it);
                return;
            }
        }
    }
};

struct fill_record {
    strategy_id strategy = strategy_id::MM;
    request_kind kind = request_kind::quote;
    side sd = side::bid;
    price_t price = NULL_PRICE;
    qty_t size = 0;
};

// Legacy matchOrders' return value plus its AS bookkeeping.
struct match_result {
    bool ok = true;
    bool as_self_match = false;
    qty_t as_matched_bid = 0;
    qty_t as_matched_ask = 0;
};

struct match_fills {
    std::vector<fill_record> items;
    void clear() { items.clear(); }
};

class ime {
public:
    explicit ime(uint32_t contract) noexcept;

    // Legacy modifyOrder: leave the map and the level, install the fields, re-enter if there is a
    // price and a size, then update the persisted price and the external level.
    void modify(const order_request& request);

    // Legacy removeOrderByStrategy (:428-439).
    void remove_strategy(strategy_id strategy);

    // Legacy matchOrders. Fills are appended to `out`.
    match_result match(strategy_id aggressor, match_fills& out);

    // Legacy tryMatch: the FAK path. Returns the fills it made and what it could not take.
    struct aggress_result {
        qty_t filled = 0;
        qty_t remaining = 0;
        price_t vwap = NULL_PRICE;
        uint32_t self_matches = 0;
        bool as_self_match = false;
    };
    aggress_result try_match(side aggressor_sd, strategy_id strategy, price_t limit, qty_t size,
                             match_fills& out);

    // Legacy getPriceAndSize (:835-852) / getPersistedPrice (:854-869).
    price_t external_top_price(side sd) const;
    qty_t external_top_size(side sd) const;
    price_t persisted_price(side sd) const;

    // Legacy getBestMarket (:1249-1272).
    internal_top best_for(strategy_id subscriber, side sd) const;

    const order* find(strategy_id s, request_kind k, side sd) const {
        return &orders_[slot_of(s, k, sd)];
    }
    const order& raw(uint32_t slot) const { return orders_[slot]; }
    // Self-check used by the differential: every map entry must agree with its order, and every
    // order that claims to be in the map must be found in it. A violation here is a bug in this
    // transcription, not in the engine.
    bool validate(const char* when) const;
    // Debug dump of the legacy maps, for the differential's first-divergence report.
    void dump(const char* tag) const;
    // The external levels with their list order, which is what decides the reported size.
    void dump_levels(const char* tag) const;

private:
    static uint32_t slot_of(strategy_id s, request_kind k, side sd) noexcept {
        return (to_int(s) * KIND_COUNT + to_int(k)) * SIDE_COUNT + to_index(sd);
    }

    void insert_order(order& o);
    void remove_order(order& o);
    void add_to_level(order& o);
    void remove_from_level(order& o);
    void reduce_external(order& o, qty_t fill);

    uint32_t contract_ = 0;
    std::array<order, SLOTS_PER_CONTRACT> orders_{};
    // The legacy internal order maps: one order per price, first inserted wins (emplace).
    std::map<price_t, order*, std::greater<price_t>> bids_;
    std::map<price_t, order*> asks_;
    // The legacy external levels.
    std::map<price_t, price_level, std::greater<price_t>> bid_levels_;
    std::map<price_t, price_level> ask_levels_;
    std::array<std::array<price_t, SIDE_COUNT>, STRATEGY_COUNT> persisted_{};
};

}  // namespace oracle
