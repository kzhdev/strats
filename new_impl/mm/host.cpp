#include "host.hpp"

namespace tachyon::trade::mm {

bool host::can_quote_externally(const contract_state& s, uint32_t c) const noexcept {
    const uint32_t venue = m_.venue[c];
    const bool venue_ok = venue < venue_enabled_.size() ? venue_enabled_[venue] != 0 : true;
    return s.quote_external && external_quoting_allowed_ && venue_ok;
}

void host::alert(uint32_t c, const char* reason) noexcept {
    if (ports_.on_alert != nullptr) ports_.on_alert(ports_.ctx, c, reason);
}

void host::disable_external_quoting(contract_state& s, uint32_t c) noexcept {
    // Legacy setQuoteExternalFlagToFalse: the flag drops, and the next update re-emits the quote
    // with a zero external size.
    if (s.quote_external) {
        s.quote_external = false;
        alert(c, "external quoting disabled");
    }
}

void host::disable_contract(contract_state& s, uint32_t c, bool disable_gtc) noexcept {
    // Legacy disable (:1279-1295): cancel everything, clear the quotes and hedges, drop the flag.
    if (ports_.cancel_orders != nullptr) ports_.cancel_orders(ports_.ctx, c, disable_gtc);
    s.quote_of(side::bid).reset();
    s.quote_of(side::ask).reset();
    s.hedge_of(side::bid).reset();
    s.hedge_of(side::ask).reset();
    s.enabled = false;
    ++stats_.disables;
    stats_.disabled = true;
}

void host::emit_removal(contract_state& s, uint32_t c, side sd, intent_log& log) noexcept {
    // Legacy modifyQuoteOrder<Side, 0>(quote, false) right after reset(): a NULL-priced, zero-size
    // request that pulls the order, with lastSent following the reset values.
    log.add_quote(c, sd, tachyon::NULL_PRICE, 0, tachyon::NULL_PRICE, 0, false);
    quote_state& q = s.quote_of(sd);
    q.last_internal_price_sent = tachyon::NULL_PRICE;
    q.last_external_size_sent = 0;
    ++stats_.quote_removals;
}

void host::emit(intent_log& log) noexcept {
    if (log.empty()) return;
    if (!can_send_quotes_) return;  // deferred: the dirty set keeps the contract for the next cycle
    for (uint32_t i = 0; i < log.size(); ++i) {
        const order_intent& it = log.at(i);
        if (it.type == order_intent::kind::quote) {
            if (ports_.send_quote != nullptr) ports_.send_quote(ports_.ctx, it);
        } else if (ports_.send_hedge != nullptr) {
            ports_.send_hedge(ports_.ctx, it);
        }
    }
    if (ports_.notify != nullptr) ports_.notify(ports_.ctx, log.at(0).contract);
    stats_.intents_emitted += log.size();
    ++stats_.notifications;
}

bool host::has_market_data(uint32_t c) noexcept {
    contract_state& s = s_.contract(c);
    if (s.side_book(side::bid).has_price() && s.side_book(side::ask).has_price()) return true;

    // Legacy :375-394: losing the book turns the contract off and (when we were quoting externally)
    // turns external quoting and GTC off as well.
    const uint32_t venue = m_.venue[c];
    const bool venue_ok = venue < venue_enabled_.size() ? venue_enabled_[venue] != 0 : true;
    if (venue_ok) {
        if (s.enabled) {
            alert(c, "no market data: disabling the contract");
            disable_contract(s, c, /*disable_gtc=*/true);
            disable_external_quoting(s, c);
        } else {
            disable_external_quoting(s, c);
        }
    }
    return false;
}

bool host::on_market_data(uint32_t contract, side sd, const level* levels, uint32_t count,
                          uint32_t price_num, uint32_t qty_num, uint64_t batch_id) noexcept {
    if (contract >= s_.n_contracts()) return false;
    contract_state& s = s_.contract(contract);

    // Legacy MarketMaker_MD.cpp:13-19: a batch older than the last one we applied is stale.
    if (s.last_md_bid != 0 && batch_id <= s.last_md_bid) {
        ++stats_.stale_md_rejected;
        return false;
    }

    book& b = s.side_book(sd);
    if (!b.note_change(price_num, qty_num)) return false;  // level index out of range: ignore
    // copyBooks (:157-166) only copies the snapshot when the *quantity* index moved.
    if (qty_num != 0) b.apply_snapshot(levels, count);

    s.pending_md_bid = batch_id;
    ++stats_.md_updates;
    s_.mark_dirty(contract);
    return true;
}

bool host::on_internal_book_update(uint32_t contract, side sd, const level* levels, uint32_t count,
                                   uint64_t batch_id) noexcept {
    if (contract >= s_.n_contracts()) return false;
    contract_state& s = s_.contract(contract);
    // Legacy MarketMaker_MessageHandler.cpp:79-92: the internal book wins ties (`>=`) and both
    // change indices are reset so the cycle recalculates from the new book.
    if (batch_id < s.last_md_bid) return false;
    book& b = s.side_book(sd);
    b.apply_snapshot(levels, count);
    b.note_change(1, 1);
    s.last_md_bid = batch_id;
    s.pending_md_bid = batch_id;
    s_.mark_dirty(contract);
    return true;
}

void host::run_cycle(uint64_t now_cycles) noexcept {
    ++stats_.cycles;
    s_.now_cycles = now_cycles;

    // canSendQuotes: while the executor has a transaction open the whole cycle is deferred and the
    // dirty set keeps the contracts for the next attempt (legacy retains `hasChange`).
    if (!can_send_quotes_) return;

    for (uint32_t i = 0; i < s_.changed_count(); ++i) {
        const uint32_t c = s_.changed_at(i);
        contract_state& s = s_.contract(c);

        // ---- pricing (:436-446) ----
        const bool top_changed =
            s.side_book(side::bid).top_changed() || s.side_book(side::ask).top_changed();
        if (top_changed) strat_.quotes().refresh_market(s, c, now_cycles);

        if (!has_market_data(c) || !s.is_enabled(strategy_enabled_)) {
            s.side_book(side::bid).clear_change_indices();
            s.side_book(side::ask).clear_change_indices();
            s.last_md_bid = s.pending_md_bid;
            continue;
        }

        // ---- the maxTicksWide block (:452-482) ----
        const price_t spread =
            s.side_book(side::ask).top().price - s.side_book(side::bid).top().price;
        const bool was_breached = s.max_ticks_wide_breached;

        if (spread > m_.max_ticks_wide_2x[c]) {
            s.max_ticks_wide_breached = true;
            alert(c, "2x maxTicksWide breached: disabling the contract and external quoting");
            disable_contract(s, c, /*disable_gtc=*/true);
            disable_external_quoting(s, c);
            s.side_book(side::bid).clear_change_indices();
            s.side_book(side::ask).clear_change_indices();
            s.last_md_bid = s.pending_md_bid;
            continue;
        }
        if (spread > m_.max_ticks_wide[c]) {
            if (!s.max_ticks_wide_breached) {
                alert(c, "maxTicksWide breached: disabling external quoting");
                disable_external_quoting(s, c);
                s.max_ticks_wide_breached = true;
                ++stats_.max_ticks_wide_latches;
            }
        } else if (s.max_ticks_wide_breached) {
            s.max_ticks_wide_breached = false;
        }

        intent_log& log = last_log_[c];
        log.clear();
        bool changed = false;

        // On the transition into the latch both quotes are pulled explicitly (:475-482).
        if (s.max_ticks_wide_breached && !was_breached) {
            s.quote_of(side::bid).reset();
            s.quote_of(side::ask).reset();
            emit_removal(s, c, side::bid, log);
            emit_removal(s, c, side::ask, log);
            changed = true;
        }

        const bool quote_externally = can_quote_externally(s, c);
        changed |= strat_.on_md_change(s, s_.queue(c), c, top_changed, quote_externally, log);

        if (changed || !log.empty()) {
            pending_[c] = true;
            emit(log);
            pending_[c] = false;
        }

        s.side_book(side::bid).clear_change_indices();
        s.side_book(side::ask).clear_change_indices();
        s.last_md_bid = s.pending_md_bid;
    }
    s_.clear_dirty();
}

fill_outcome host::on_fill(uint32_t contract, const fill& f) noexcept {
    if (contract >= s_.n_contracts()) return fill_outcome{};
    ++stats_.fills;
    contract_state& s = s_.contract(contract);

    fill_outcome out = book_.on_fill(s, contract, f);

    if (out.untaken_remaining > 0) {
        // Legacy :288-309: with no other strategy to hand the remainder to, the MM takes it and
        // stops quoting externally.
        ++stats_.untaken_overfills;
        alert(contract, "external overfill beyond the position limit: disabling external quoting");
        out.disable_external_quoting = true;
    } else if (f.source != fill_source::quote && f.source != fill_source::hedge && f.qty > 0) {
        ++stats_.overfills_taken;
    }
    if (out.disable_external_quoting) disable_external_quoting(s, contract);

    if (ports_.on_fill_report != nullptr) ports_.on_fill_report(ports_.ctx, contract, nullptr, out);
    if (out.ends_cycle_inline) end_of_fills(contract);
    return out;
}

void host::end_of_fills(uint32_t contract) noexcept {
    if (contract >= s_.n_contracts()) return;
    contract_state& s = s_.contract(contract);
    if (!s.is_enabled(strategy_enabled_)) return;
    if (!s.side_book(side::bid).has_price() || !s.side_book(side::ask).has_price()) return;

    intent_log& log = last_log_[contract];
    log.clear();
    const bool quote_externally = can_quote_externally(s, contract);
    const bool changed =
        strat_.apply_updates(s, s_.queue(contract), contract, quote_externally, log);
    if (changed || !log.empty()) emit(log);
}

void host::on_order_update(uint32_t contract, uint32_t order_id, price_t price, qty_t qty, qty_t qp,
                           side sd) noexcept {
    if (contract >= s_.n_contracts()) return;
    s_.queue(contract).upsert(order_id, price, qty, qp, sd);
}

void host::on_queue_position_update(uint32_t contract, uint32_t order_id, qty_t qp) noexcept {
    if (contract >= s_.n_contracts()) return;
    order_queue& q = s_.queue(contract);
    const order_queue::slot* slot = q.find(order_id);
    if (slot == nullptr) return;

    const side sd = static_cast<side>(slot->side);
    const price_t price = slot->price;
    // Legacy :1615-1622: the position is recorded first, then a re-quote may follow.
    q.set_queue_position(order_id, qp);

    contract_state& s = s_.contract(contract);
    if (!s.is_enabled(strategy_enabled_) || !can_quote_externally(s, contract)) return;
    if (!s.side_book(sd).has_price()) return;
    if (price != s.quote_of(sd).external_price) return;

    // Legacy's guard is `std::next(orderIter) == _orders.end()`, i.e. "this is the last entry of
    // the global order map" — an unordered_map traversal order, so it is not reproducible. The
    // deterministic interpretation used here is "the last order at our own price level", which is
    // the order whose queue position actually gates the spray (PARITY_NOTES §2d N4).
    if (q.last(sd, price) != slot) return;

    intent_log& log = last_log_[contract];
    log.clear();
    const uint64_t batch = s.last_md_bid;
    if (strat_.quotes().build_quote(s, q, contract, sd, /*quote_externally=*/true, false, &log)) {
        emit(log);
    }
    (void)batch;
}

}  // namespace tachyon::trade::mm

