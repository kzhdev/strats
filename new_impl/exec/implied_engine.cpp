#include "implied_engine.hpp"

#include <algorithm>

#ifdef EXEC_TRACE_EMIT
#include <cstdio>
#endif

namespace tachyon::trade::exec {

void implied_engine::init(const engine_config& config, exchange_gateway* gateway) noexcept {
    gateway_ = gateway;
    hedge_tolerance_ = config.hedge_tolerance;
    book_.init(config.collision, config.contract);
    for (auto& per_side : published_) per_side = {internal_top{}, internal_top{}};
    for (auto& sent : sent_) sent = sent_quote{};
    fills_.clear();
    staged_ = {};
    staged_mask_ = 0;
    flush_aggressor_ = strategy_id::MM;
    stats_ = engine_stats{};
    last_mm_execution_ = 0;
    self_match_counter_ = 0;
    as_self_match_counter_ = 0;
    dirty_ = false;
    self_crossed_ = false;
}

void implied_engine::record(const internal_fill& fill) noexcept {
    // Counted once, in flush_fills(): the book appends its passive fills to the same buffer.
    fills_.add(fill);
}

// ---------------------------------------------------------------------------
// Intake
// ---------------------------------------------------------------------------
void implied_engine::on_request(const order_request& request) noexcept {
    ++stats_.requests;
    const request_key key = request.key();
    const strategy_id strategy = key.strategy;

    // Legacy MM staleness (:1980-1993): an MM request stamped before the last MM execution is
    // ignored -- unless it is a hedge with no external price, which may be the request that takes
    // the IME's order out of the book.
    if (strategy == strategy_id::MM && last_mm_execution_ > 0 &&
        request.exec_token < last_mm_execution_) {
        const bool keep = key.kind == request_kind::hedge && request.external_price == NULL_PRICE;
        if (!keep) {
            ++stats_.stale_dropped;
            return;
        }
    }

    // Legacy (:1996): a disabled strategy's request is dropped.
    const strategy_endpoint* endpoint = endpoints_[to_int(strategy)];
    if (endpoint != nullptr && !endpoint->enabled()) {
        ++stats_.disabled_dropped;
        return;
    }

    const key_traits traits = traits_of(strategy, key.kind);
    if (traits.policy == collapse::last_wins) {
        // The whole point of the mailbox: keep the newest request per key, drop the rest.
        staged_[key.slot()] = request;
        staged_mask_ |= 1u << key.slot();
        flush_aggressor_ = strategy;
        ++stats_.staged;
        dirty_ = true;
        return;
    }

    // FIFO streams (RV/AS hedges, AS quotes) are applied in arrival order, so anything staged from
    // an earlier stream is applied first (D-EX5).
    flush_aggressor_ = strategy;
    flush(0);
    ++stats_.applied_now;
    apply_now(request);
    dirty_ = true;
}

void implied_engine::apply_now(const order_request& request) noexcept {
    const request_key key = request.key();
    const key_traits traits = traits_of(key.strategy, key.kind);

    if (traits.rests) {
        // An AS quote replaces AS's resting order and then crosses (":1967 Aggressive Quote
        // adjustment").
        book_.apply(request);
        cross_book();
    } else {
        cross_fak(request);  // FAK hedge: never touches the book
    }
}

// ---------------------------------------------------------------------------
// The cycle
// ---------------------------------------------------------------------------
void implied_engine::flush(uint64_t /*now_cycles*/) noexcept {
    if (staged_mask_ == 0 && !dirty_) return;
    ++stats_.flushes;

    uint32_t mask = staged_mask_;
    staged_mask_ = 0;
    order_request hedges[SIDE_COUNT];
    bool hedge_touched[SIDE_COUNT] = {false, false};

    // Ascending slot order is the legacy apply order: MM quote bid, MM quote ask, MM hedge bid,
    // MM hedge ask, RV quote bid, RV quote ask.
    while (mask != 0) {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(mask));
        mask &= mask - 1;
        const order_request& request = staged_[slot];
        book_.apply(request);
        if (request.key().kind == request_kind::hedge) {
            hedges[to_index(request.key().sd)] = request;
            hedge_touched[to_index(request.key().sd)] = true;
        }
    }

    cross_book();

    // Legacy synchronizeQuote per side: the aggregated residual of the internal book. Only the
    // sides that moved are recomputed (the book tracks that itself), and it publishes the new
    // internal best as part of the same step.
    emit_dirty();

    // Legacy synchronizeQuote's `_pendingHedge`: one hedge leg per side, from the *requested*
    // size (modifyOrder stored `_size = size`, :2017-2019), not from what the cross consumed.
    for (uint32_t sd = 0; sd < SIDE_COUNT; ++sd) {
        if (hedge_touched[sd] && hedges[sd].external_price != NULL_PRICE) emit_hedge(hedges[sd]);
    }

    dirty_ = false;
}

void implied_engine::flush_fills() noexcept {
    if (fills_.count == 0) return;

    uint32_t strategies = 0;
    for (const internal_fill& fill : fills_) {
        strategy_endpoint* endpoint = endpoints_[to_int(fill.strategy)];
        if (endpoint == nullptr) continue;
        endpoint->on_fill(fill);
        strategies |= 1u << to_int(fill.strategy);
    }
    if (fills_.overflowed) {
        ++stats_.fill_overflow;
        if (gateway_ != nullptr) {
            gateway_->alert(contract(), "Internal fill buffer overflow", ALERT_FILL_OVERFLOW);
        }
    }
    stats_.fills += fills_.count;
    fills_.clear();

    for (uint32_t s = 0; s < STRATEGY_COUNT; ++s) {
        if ((strategies & (1u << s)) == 0) continue;
        strategy_endpoint* endpoint = endpoints_[s];
        if (endpoint != nullptr) endpoint->end_of_fills(contract());
    }
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------
void implied_engine::cross_book() noexcept {
    ++stats_.crosses;
    const cross_result result = book_.cross(flush_aggressor_, fills_);

    if (result.as_self_match) {
        strategy_endpoint* as = endpoints_[to_int(strategy_id::AS)];
        if (as != nullptr) as->on_as_match_done(contract(), result.as_matched_bid, result.as_matched_ask);
    }

    if (!result.ok) {
        // Legacy (:1926-1933): disable external quoting for the contract, count it, and shut down
        // when it keeps happening.
        ++stats_.self_matches;
        if (gateway_ != nullptr) gateway_->alert(contract(), "Self crossed", ALERT_SELF_MATCH);
        for (strategy_endpoint* endpoint : endpoints_) {
            if (endpoint != nullptr) endpoint->on_self_match(contract(), "Self crossed");
        }
        if (++self_match_counter_ >= MAX_SELF_MATCH_THRESHOLD) {
            if (gateway_ != nullptr) {
                gateway_->alert(contract(), "Self match reached 50 times. Shutting Viper down.",
                                ALERT_SELF_MATCH_LIMIT);
            }
        }
        // The book stays crossed: take the contract out of the external market (the strategies have
        // already been told to stop quoting it) and publish what is left.
        if (!self_crossed_) {
            self_crossed_ = true;
            emit_quote(side::bid);
            emit_quote(side::ask);
        }
        publish();  // legacy: the disable path publishes the internal market (:1929)
        return;
    }

    if (self_crossed_) {
        // The strategies cleared the cross: quoting may resume (change detection re-emits).
        self_crossed_ = false;
        book_.mark_dirty();
    }
    if (result.touched) dirty_ = true;
}

void implied_engine::cross_fak(const order_request& request) noexcept {
    const side sd = request.side_value();
    const strategy_id strategy = request.strategy_id_value();
    // Legacy adjustPrice (:149-160): the tolerance widens the price the hedge is willing to pay.
    const price_t limit = sd == side::bid ? request.internal_price + hedge_tolerance_
                                          : request.internal_price - hedge_tolerance_;

    const implied_book::aggressive_result result =
        book_.aggress(sd, strategy, limit, request.hedge_size, fills_);

    if (result.self_matches > 0) {
        if (strategy == strategy_id::AS) {
            // Legacy (:899-907): AS counts its self matches and is disabled at the threshold.
            stats_.as_self_matches += result.self_matches;
            as_self_match_counter_ += result.self_matches;
            if (as_self_match_counter_ >= MAX_AS_SELF_MATCH_THRESHOLD) {
                as_self_match_counter_ = 0;
                if (gateway_ != nullptr) {
                    gateway_->alert(contract(), "Disable AS. Self match reached 50 times",
                                    ALERT_AS_SELF_MATCH_LIMIT);
                }
                strategy_endpoint* as = endpoints_[to_int(strategy_id::AS)];
                if (as != nullptr) as->disable();
            }
        } else if (gateway_ != nullptr) {
            // Legacy (:908-915): a skipped self match is a critical alert for everyone else.
            gateway_->alert(contract(), "Skip self match order", ALERT_SKIPPED_SELF_MATCH);
        }
    }

    if (result.touched) dirty_ = true;

    if (result.filled > 0) {
        // Legacy (:1875-1880): the aggressor's own fill is reported at the volume-weighted price,
        // then the *other* side is re-quoted and the internal market republished.
        record(internal_fill{contract(), strategy, request_kind::hedge, sd, result.vwap,
                             result.filled, false});
        emit_dirty();
    }

    if (request.external()) {
        // "The remaining order is sent externally": legacy sendExternalFAKHedge (:1881-1890).
        if (result.remaining > 0) {
            order_request residual = request;
            residual.hedge_size = result.remaining;
            emit_hedge(residual);
        }
    } else {
        if (result.remaining > 0) {
            // Legacy (:2033-2042): an internal-only hedge that missed is handed back on the other
            // side as FillSource::MissedHedge so the strategy can unwind.
            record(internal_fill{contract(), strategy, request_kind::hedge, other(sd),
                                 request.external_price, result.remaining, true});
            dirty_ = true;
        }
        // Legacy (:1898): the internal hedge is done either way.
        strategy_endpoint* endpoint = endpoints_[to_int(strategy)];
        if (endpoint != nullptr) {
            endpoint->on_hedge_done(contract(), request.hedge_id, request.market_id, other(sd));
        }
    }
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------
void implied_engine::emit_quote(side sd) noexcept {
    // Legacy getPriceAndSize (:835-852) in one pass: the best external level, unless a strategy
    // asked to keep a better price, in which case that price goes out with size 0.
    const implied_book::external_snapshot top = book_.external(sd);
    price_t price = top.price;
    qty_t size = top.size;
    const price_t persisted = book_.persisted_price(sd);
    bool price_only = false;
    if (self_crossed_) {
        // Legacy getQuoteOutOfMarket (:559): nothing of this contract may rest on the exchange
        // while the internal book is self-crossed.
        price = NULL_PRICE;
        size = 0;
        price_only = false;
    } else
    if (!self_crossed_ && persisted != NULL_PRICE &&
        (price == NULL_PRICE || is_better_price(persisted, price, sd))) {
        price = persisted;
        size = 0;
        price_only = true;
    }

    sent_quote& last = sent_[to_index(sd)];
#ifdef EXEC_TRACE_EMIT
    std::printf("[emit] contract=%u side=%u price=%lld size=%lld (last %lld@%lld valid=%d) dirty=%u\n",
                contract(), (unsigned)sd, (long long)price, (long long)size,
                (long long)last.price, (long long)last.size, (int)last.valid, book_.dirty());
#endif
    if (last.valid && last.price == price && last.size == size) return;  // change-detected
    last = sent_quote{price, size, true};

    if (gateway_ == nullptr) return;
    // A NULL price with size 0 tells the gateway to leave the market (legacy
    // `_om[Side].getQuoteOutOfMarket()`, :559).
    gateway_->send_quote(exchange_order{contract(), top.strategy, sd, price, size, false, price_only,
                                        false, 0});
    ++stats_.quotes_sent;
}

void implied_engine::emit_hedge(const order_request& request) noexcept {
    if (gateway_ == nullptr) return;
    gateway_->send_hedge(exchange_order{contract(), request.strategy_id_value(),
                                        request.side_value(), request.external_price,
                                        request.hedge_size, true, request.persist(),
                                        request.pay_up(), request.hedge_id});
    ++stats_.hedges_sent;
}

void implied_engine::emit_dirty() noexcept {
    const uint32_t dirty = book_.dirty();
    book_.clear_dirty();
    if (dirty == 0) return;

    const bool bid_dirty = (dirty & (1u << to_index(side::bid))) != 0;
    const bool ask_dirty = (dirty & (1u << to_index(side::ask))) != 0;

    if (bid_dirty && ask_dirty) {
        // Legacy synchronizeOrders (:524-541) chooses the order of the two messages so that a
        // crossed pair never exists on the exchange -- not even between the two messages of one
        // flush: when the bid is not getting worse, the ask is moved out of the way first.
        const implied_book::external_snapshot bid = book_.external(side::bid);
        const sent_quote& last_bid = sent_[to_index(side::bid)];
        const bool bid_improving = bid.price != NULL_PRICE &&
                                   (!last_bid.valid || is_better_price(bid.price, last_bid.price, side::bid));
        if (bid_improving) {
            emit_quote(side::ask);
            emit_quote(side::bid);
        } else {
            emit_quote(side::bid);
            emit_quote(side::ask);
        }
    } else if (bid_dirty) {
        emit_quote(side::bid);
    } else {
        emit_quote(side::ask);
    }

    publish(dirty);
}

void implied_engine::publish(uint32_t side_mask) noexcept {
    // Legacy asserts the MM is never an internal-market subscriber (:1276), so only RV and AS are
    // published to. Only the sides that moved are scanned: one cross normally moves one side, and a
    // masked publish costs one scan instead of two.
    if (side_mask == 0) return;
    for (uint32_t s = to_int(strategy_id::RV); s < STRATEGY_COUNT; ++s) {
        strategy_endpoint* endpoint = endpoints_[s];
        if (endpoint == nullptr) continue;
        const strategy_id subscriber = static_cast<strategy_id>(s);
        internal_top bid;
        internal_top ask;
        if (side_mask == 3) {
            book_.best_pair(subscriber, bid, ask);  // one pass for both sides
        } else if ((side_mask & 1u) != 0) {
            bid = book_.best_for(subscriber, side::bid);
        } else {
            ask = book_.best_for(subscriber, side::ask);
        }
        if ((side_mask & 1u) != 0 && bid != published_[s][0]) {
            published_[s][0] = bid;
            endpoint->on_internal_market(contract(), side::bid, bid);
            ++stats_.publishes;
        }
        if ((side_mask & 2u) != 0 && ask != published_[s][1]) {
            published_[s][1] = ask;
            endpoint->on_internal_market(contract(), side::ask, ask);
            ++stats_.publishes;
        }
    }
}

// ---------------------------------------------------------------------------
// Out-of-band commands
// ---------------------------------------------------------------------------
void implied_engine::cancel_strategy(strategy_id strategy, bool disable_gtc) noexcept {
    book_.remove_strategy(strategy);
    ++stats_.cancels;
    if (gateway_ != nullptr) gateway_->cancel(contract(), strategy, disable_gtc);
    // The residual shrank, so both sides are re-emitted; publish because the internal book moved.
    emit_dirty();
    dirty_ = false;
}

void implied_engine::cancel_all(bool disable_gtc) noexcept {
    for (uint32_t s = 0; s < STRATEGY_COUNT; ++s) {
        book_.remove_strategy(static_cast<strategy_id>(s));
        ++stats_.cancels;
        if (gateway_ != nullptr) gateway_->cancel(contract(), static_cast<strategy_id>(s), disable_gtc);
    }
    book_.clear_dirty();
    emit_dirty();
    dirty_ = false;
}

}  // namespace tachyon::trade::exec
