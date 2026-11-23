#include "match_oracle.hpp"

#include <algorithm>
#include <cstdio>

namespace oracle {

ime::ime(uint32_t contract) noexcept : contract_(contract) {
    for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
        order& o = orders_[slot];
        o.strategy = static_cast<strategy_id>(slot / (KIND_COUNT * SIDE_COUNT));
        o.kind = static_cast<request_kind>((slot / SIDE_COUNT) % KIND_COUNT);
        o.sd = static_cast<side>(slot % SIDE_COUNT);
        o.id = static_cast<uint32_t>(to_int(o.strategy)) * KIND_COUNT + to_int(o.kind);
    }
    for (auto& per_side : persisted_) per_side = {NULL_PRICE, NULL_PRICE};
}

void ime::insert_order(order& o) {
    o.in_book = false;
    if (o.sd == side::bid) {
        // Legacy `internalOrderMap.emplace(price, &order)`: the first order at a price wins and a
        // second one is silently not matchable.
        auto result = bids_.emplace(o.price, &o);
        o.in_book = result.second;
    } else {
        auto result = asks_.emplace(o.price, &o);
        o.in_book = result.second;
    }
}

void ime::remove_from_level(order& o) {
    if (!o.in_level) return;
    if (o.sd == side::bid) {
        auto it = bid_levels_.find(o.external_price);
        if (it != bid_levels_.end()) {
            it->second.remove(&o);
            if (it->second.orders.empty()) bid_levels_.erase(it);
        }
    } else {
        auto it = ask_levels_.find(o.external_price);
        if (it != ask_levels_.end()) {
            it->second.remove(&o);
            if (it->second.orders.empty()) ask_levels_.erase(it);
        }
    }
    o.in_level = false;
}

void ime::remove_order(order& o) {
    // Legacy doRemoveOrder (:477-489): erase the entry whose id matches, then the price level.
    if (o.in_book) {
        if (o.sd == side::bid) {
            auto range = bids_.equal_range(o.price);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second->id == o.id) {
                    bids_.erase(it);
                    break;
                }
            }
        } else {
            auto range = asks_.equal_range(o.price);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second->id == o.id) {
                    asks_.erase(it);
                    break;
                }
            }
        }
        o.in_book = false;
    }
    remove_from_level(o);
}

void ime::add_to_level(order& o) {
    if (o.sd == side::bid) {
        price_level& level = bid_levels_[o.external_price];
        level.price = o.external_price;
        level.add(&o);
    } else {
        price_level& level = ask_levels_[o.external_price];
        level.price = o.external_price;
        level.add(&o);
    }
    o.in_level = true;
}

void ime::reduce_external(order& o, qty_t fill) {
    // Legacy doExternalReductionAfterInternalFill (:1043-1056).
    if (o.external_price == NULL_PRICE) return;
    const qty_t next = std::max<qty_t>(o.external_size - fill, 0);
    if (o.kind != request_kind::hedge && next == 0) remove_from_level(o);
    o.external_size = next;
}

void ime::modify(const order_request& request) {
    const request_key key = request.key();
    order& o = orders_[slot_of(key.strategy, key.kind, key.sd)];

    // Legacy modifyOrder (:329-378).
    remove_order(o);
    o.price = request.internal_price;
    o.external_price = request.external_price;
    o.pay_up = request.pay_up();
    o.lean_on_rv = request.lean_on_rv();
    const bool hedge = key.kind == request_kind::hedge;
    o.size = hedge ? request.hedge_size : request.internal_size;
    o.external_size = hedge ? request.hedge_size : request.external_size;

    if (o.price != NULL_PRICE && o.size != 0) insert_order(o);

    if (!hedge) {
        persisted_[to_int(key.strategy)][to_index(key.sd)] =
            request.persist() ? o.external_price : NULL_PRICE;
        if (o.external_price != NULL_PRICE && o.external_size != 0) add_to_level(o);
    }
}

void ime::remove_strategy(strategy_id strategy) {
    for (uint32_t kind = 0; kind < KIND_COUNT; ++kind) {
        for (uint32_t sd = 0; sd < SIDE_COUNT; ++sd) {
            order& o = orders_[slot_of(strategy, static_cast<request_kind>(kind),
                                       static_cast<side>(sd))];
            remove_order(o);
            o.price = NULL_PRICE;
            o.external_price = NULL_PRICE;
            o.size = 0;
            o.external_size = 0;
        }
    }
}

price_t ime::external_top_price(side sd) const {
    if (sd == side::bid) {
        return bid_levels_.empty() ? NULL_PRICE : bid_levels_.begin()->first;
    }
    return ask_levels_.empty() ? NULL_PRICE : ask_levels_.begin()->first;
}

qty_t ime::external_top_size(side sd) const {
    if (sd == side::bid) {
        return bid_levels_.empty() ? 0 : bid_levels_.begin()->second.get_size();
    }
    return ask_levels_.empty() ? 0 : ask_levels_.begin()->second.get_size();
}

price_t ime::persisted_price(side sd) const {
    // Legacy getPersistedPrice (:854-869).
    price_t result = NULL_PRICE;
    for (uint32_t s = 0; s < STRATEGY_COUNT; ++s) {
        const price_t price = persisted_[s][to_index(sd)];
        if (price == NULL_PRICE) continue;
        if (result == NULL_PRICE) {
            result = price;
        } else if (sd == side::bid) {
            result = std::max(result, price);
        } else {
            result = std::min(result, price);
        }
    }
    return result;
}

}  // namespace oracle

namespace oracle {

internal_top ime::best_for(strategy_id subscriber, side sd) const {
    // Legacy getBestMarket (:1249-1272): walk the price map from the top, skipping the subscriber's
    // own liquidity and any order that leans on RV.
    internal_top top;
    auto take = [&](const order* o) {
        top.price = o->price;
        top.size = o->size;
        top.source = traits_of(o->strategy, o->kind).source;
    };
    if (sd == side::bid) {
        for (const auto& entry : bids_) {
            const order* o = entry.second;
            if (o->strategy == subscriber || o->lean_on_rv) continue;
            take(o);
            break;
        }
    } else {
        for (const auto& entry : asks_) {
            const order* o = entry.second;
            if (o->strategy == subscriber || o->lean_on_rv) continue;
            take(o);
            break;
        }
    }
    return top;
}

match_result ime::match(strategy_id aggressor, match_fills& out) {
    match_result result;
    if (bids_.empty() || asks_.empty()) return result;

    auto bid_it = bids_.begin();
    auto ask_it = asks_.begin();
    order* bid_order = bid_it->second;
    order* ask_order = ask_it->second;
    price_t bid_top = bid_it->first;
    price_t ask_top = ask_it->first;
    qty_t bid_size = bid_order->size;
    qty_t ask_size = ask_order->size;
    bool match_bid_external = false;
    bool match_ask_external = false;
    bool as_self_match = false;

    while (bid_top >= ask_top) {
        const bool self_match = bid_order->strategy == ask_order->strategy;
        if (self_match) {
            // ARBIE self match is allowed, everything else aborts the whole cross (:1127-1145).
            if (bid_order->strategy != strategy_id::AS) {
                result.ok = false;
                return result;
            }
            as_self_match = true;
        }

        const bool has_as_order = bid_order->strategy == strategy_id::AS ||
                                  ask_order->strategy == strategy_id::AS;
        const price_t fill_price = bid_order->strategy == aggressor ? ask_top : bid_top;
        const qty_t fill_size = std::min(ask_size, bid_size);

        if (aggressor == strategy_id::AS && has_as_order) {
            if (bid_order->strategy == strategy_id::AS) result.as_matched_bid += fill_size;
            if (ask_order->strategy == strategy_id::AS) result.as_matched_ask += fill_size;
        }

        out.items.push_back({bid_order->strategy, bid_order->kind, bid_order->sd, fill_price,
                             fill_size});
        out.items.push_back({ask_order->strategy, ask_order->kind, ask_order->sd, fill_price,
                             fill_size});
        reduce_external(*bid_order, fill_size);
        reduce_external(*ask_order, fill_size);

        bid_size -= fill_size;
        ask_size -= fill_size;

        if (!match_bid_external) {
            bid_order->size -= fill_size;
            if (bid_order->size == 0) {
                if (bid_order->external_price != NULL_PRICE && bid_order->external_size) {
                    match_bid_external = true;
                    bid_top = bid_order->external_price;
                    bid_size = bid_order->external_size;
                }
            }
        }

        if (!match_ask_external) {
            ask_order->size -= fill_size;
            if (ask_order->size == 0) {
                if (ask_order->external_price != NULL_PRICE && ask_order->external_size) {
                    match_ask_external = true;
                    ask_top = ask_order->external_price;
                    ask_size = ask_order->external_size;
                }
            }
        }

        if (bid_size == 0) {
            // The entry's own order is the one that leaves the map, whoever bid_order points at.
            bid_it->second->in_book = false;
            bid_it = bids_.erase(bid_it);
            match_bid_external = false;
            if (bid_it == bids_.end()) break;
            bid_order = bid_it->second;
            bid_top = bid_it->first;
            bid_size = bid_order->size;
        }

        if (ask_size == 0) {
            ask_it->second->in_book = false;
            ask_it = asks_.erase(ask_it);
            match_ask_external = false;
            if (ask_it == asks_.end()) break;
            ask_order = ask_it->second;
            ask_top = ask_it->first;
            ask_size = ask_order->size;
        }
    }

    if (match_bid_external && bid_it != bids_.end()) {
        bid_it->second->in_book = false;
        bids_.erase(bid_it);
    }
    if (match_ask_external && ask_it != asks_.end()) {
        ask_it->second->in_book = false;
        asks_.erase(ask_it);
    }

    if (as_self_match) {
        result.as_self_match = true;
        // Legacy (:1213-1223): with no external level left, the AS persisted top is cleared.
        if (bid_levels_.empty()) persisted_[to_int(strategy_id::AS)][to_index(side::bid)] = NULL_PRICE;
        if (ask_levels_.empty()) persisted_[to_int(strategy_id::AS)][to_index(side::ask)] = NULL_PRICE;
    }

    return result;
}

}  // namespace oracle

namespace oracle {

ime::aggress_result ime::try_match(side aggressor_sd, strategy_id strategy, price_t limit,
                                   qty_t size, match_fills& out) {
    // Legacy tryMatch (:881-957). The aggressor never rests: it walks the opposite side from the
    // best price while `limit` is still acceptable, skipping its own orders.
    aggress_result result;
    result.remaining = size;

    qty_t cost = 0;
    const side passive_sd = other(aggressor_sd);
    // The two legacy maps have different comparators, so the walk is written once per side.
    auto walk = [&](auto& map) {
    auto it = map.begin();
    while (result.remaining > 0 && it != map.end()) {
        order* passive = it->second;
        if (!is_better_or_equal(limit, passive->price, aggressor_sd)) break;

        if (passive->strategy == strategy) {
            ++result.self_matches;
            if (strategy == strategy_id::AS) result.as_self_match = true;
            ++it;
            continue;
        }

        const qty_t fill = std::min(passive->size, result.remaining);
        out.items.push_back({passive->strategy, passive->kind, passive->sd, passive->price, fill});
        reduce_external(*passive, fill);
        passive->size -= fill;
        cost += passive->price * fill;
        result.filled += fill;
        result.remaining -= fill;

        if (passive->size == 0) {
            passive->in_book = false;
            it = map.erase(it);
        } else {
            ++it;
        }
    }
    };
    if (passive_sd == side::bid) walk(bids_); else walk(asks_);

    if (result.filled > 0) result.vwap = cost / result.filled;
    return result;
}

}  // namespace oracle

namespace oracle {

bool ime::validate(const char* when) const {
    bool ok = true;
    auto check_map = [&](const auto& map, const char* name) {
        for (const auto& entry : map) {
            const order* o = entry.second;
            if (!o->in_book) {
                std::printf("  ORACLE %s: %s entry at %lld points at an order that left the map\n",
                            when, name, (long long)entry.first);
                ok = false;
            }
            if (o->price != entry.first) {
                std::printf("  ORACLE %s: %s entry key %lld != order price %lld\n", when, name,
                            (long long)entry.first, (long long)o->price);
                ok = false;
            }
            if (o->size <= 0) {
                // Legitimate: legacy keeps an order whose internal size was exhausted while its
                // external leg was being matched, until the next cross reaches it.
            }
        }
    };
    check_map(bids_, "bids");
    check_map(asks_, "asks");
    for (uint32_t slot = 0; slot < SLOTS_PER_CONTRACT; ++slot) {
        const order& o = orders_[slot];
        if (!o.in_book) continue;
        const bool found = o.sd == side::bid ? bids_.count(o.price) != 0 &&
                                                  bids_.find(o.price)->second == &o
                                            : asks_.count(o.price) != 0 &&
                                                  asks_.find(o.price)->second == &o;
        if (!found) {
            std::printf("  ORACLE %s: slot %u claims in_map at %lld but the map disagrees\n", when,
                        slot, (long long)o.price);
            ok = false;
        }
    }
    return ok;
}

}  // namespace oracle

namespace oracle {

void ime::dump(const char* tag) const {
    std::printf("  %s bids (%zu):", tag, bids_.size());
    for (const auto& entry : bids_) {
        std::printf(" [%lld strat%u k%u %lld]", (long long)entry.first, entry.second->strategy,
                    (unsigned)entry.second->kind, (long long)entry.second->size);
    }
    std::printf("\n  %s asks (%zu):", tag, asks_.size());
    for (const auto& entry : asks_) {
        std::printf(" [%lld strat%u k%u %lld]", (long long)entry.first, entry.second->strategy,
                    (unsigned)entry.second->kind, (long long)entry.second->size);
    }
    std::printf("\n");
}

}  // namespace oracle

namespace oracle {

void ime::dump_levels(const char* tag) const {
    std::printf("  %s bid levels:", tag);
    for (const auto& entry : bid_levels_) {
        std::printf(" %lld{size=%lld head=%lld list=", (long long)entry.first,
                    (long long)entry.second.get_size(),
                    (long long)(entry.second.orders.empty() ? -1 : entry.second.orders[0]->size));
        for (const order* o : entry.second.orders) {
            std::printf("s%u/%lld ", o->strategy, (long long)o->external_size);
        }
        std::printf("}");
    }
    std::printf("\n  %s ask levels:", tag);
    for (const auto& entry : ask_levels_) {
        std::printf(" %lld{size=%lld list=", (long long)entry.first,
                    (long long)entry.second.get_size());
        for (const order* o : entry.second.orders) {
            std::printf("s%u/%lld ", o->strategy, (long long)o->external_size);
        }
        std::printf("}");
    }
    std::printf("\n");
}

}  // namespace oracle
