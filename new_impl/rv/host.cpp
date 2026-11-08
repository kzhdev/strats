#include "host.hpp"

#include <new>

namespace tachyon::trade::rv {

host::host(const model& m, cycle_state& s, liquidity_ledger* ledger,
           const host_ports& ports) noexcept
    : m_(m), s_(s), ports_(ports) {
    if (ledger != nullptr) {
        // Allocated once at init (the cycle stays allocation-free). The engine sees the
        // bridge only through the three function pointers below.
        bridge_storage_.reset(new (std::nothrow) ledger_bridge(*ledger, m.n_all));
        bridge_ = bridge_storage_.get();
        if (bridge_ != nullptr) {
            policy_ = ledger_bridge::make_policy(*bridge_);
        }
    }
    // The validation gate always comes from the host, never from the ledger bridge.
    policy_.validate = &host::validate_static;
    policy_.validate_ctx = this;
    // The host forwards every emitted order to its own sinks after the bridge has accounted
    // for it, so the port points at the host rather than directly at the bridge.
    order_port_.send_hedge = &host::send_hedge_static;
    order_port_.send_quote = &host::send_quote_static;
    order_port_.ctx = this;
    venue_enabled_.fill(1);
    strategy_.reset(new (std::nothrow) strategy(m, s, policy_, order_port_));
}

host::~host() = default;

void host::wire_market(uint32_t contract, side sd, bool external, market_key key,
                       liquidity_source source) noexcept {
    if (bridge_ != nullptr) {
        bridge_->set_market(contract, sd, external, key, source);
    }
}

void host::set_venue_enabled(uint32_t venue, bool on) noexcept {
    if (venue < venue_enabled_.size()) {
        venue_enabled_[venue] = on ? 1 : 0;
    }
}

void host::set_max_clip(uint32_t contract, price_t clip) noexcept {
    if (contract < m_.n_all) {
        strategy_->set_max_clip(contract, clip);
    }
}

void host::enable(bool on) noexcept {
    if (on) {
        disable_external_quoting_ = false;
        stats_.disabled = false;
    } else {
        stats_.disabled = true;
    }
    strategy_->set_enabled(on);
}

// ---- engine seam trampolines -------------------------------------------------

bool host::validate_static(void* ctx, uint32_t contract, side sd, price_t price,
                           uint32_t lean) noexcept {
    return static_cast<host*>(ctx)->validate(contract, sd, price, lean);
}

void host::send_hedge_static(void* ctx, const hedge_order& order) noexcept {
    static_cast<host*>(ctx)->emit_hedge(order);
}

void host::send_quote_static(void* /*ctx*/, uint32_t /*contract*/) noexcept {}

// ---- validation: maxTicksThroughMarket (RelativeValue.h:2795-2824) -----------

bool host::validate(uint32_t contract, side sd, price_t /*price*/, uint32_t lean) noexcept {
    // The arithmetic is the engine's: legacy validateMaxTicksThroughMarket is a RelativeValue
    // method, so it lives in quote_engine. The host owns only what a violation *means* --
    // counting it and (in on_timer) disabling the strategy with a critical alert.
    if (strategy_ != nullptr &&
        strategy_->quotes().within_max_ticks_through_market(s_, contract, sd, lean)) {
        return true;
    }
    ++stats_.validation_failures;
    return false;
}

// ---- hedge emission ---------------------------------------------------------

void host::remember_hedge(uint32_t hedge_id, uint32_t market_index) noexcept {
    hedge_slot* slot = nullptr;
    for (auto& h : hedges_) {
        if (!h.used) {
            slot = &h;
            break;
        }
    }
    if (slot == nullptr) {  // all tracked slots busy: recycle the oldest hedge id
        slot = &hedges_[0];
        for (auto& h : hedges_) {
            if (h.hedge_id < slot->hedge_id) {
                slot = &h;
            }
        }
    }
    slot->used = true;
    slot->hedge_id = hedge_id;
    slot->key = bridge_ != nullptr ? bridge_->key_at(market_index) : market_key{0};
}

bool host::lookup_hedge(uint32_t hedge_id, market_key& key) noexcept {
    for (auto& h : hedges_) {
        if (h.used && h.hedge_id == hedge_id) {
            key = h.key;
            return true;
        }
    }
    return false;
}

void host::emit_hedge(const hedge_order& order) noexcept {
    if (bridge_ != nullptr && bridge_->wired_at(order.market_index)) {
        bridge_->on_hedge(order);  // reserve the liquidity the hedge is about to consume
        ++stats_.reservations_taken;
        remember_hedge(order.hedge_id, order.market_index);
    }
    ++stats_.hedges_sent;
    if (ports_.send_hedge != nullptr) {
        ports_.send_hedge(ports_.ctx, order);
    }
}

void host::on_hedge_ack(uint32_t hedge_id, ord_id_t order_id, qty_t actual) noexcept {
    if (bridge_ == nullptr) {
        return;
    }
    market_key key = 0;
    if (lookup_hedge(hedge_id, key)) {
        bridge_->ack_hedge(hedge_id, key, order_id, actual);
    }
}

void host::on_hedge_done(uint32_t hedge_id) noexcept {
    if (bridge_ == nullptr) {
        return;
    }
    market_key key = 0;
    if (lookup_hedge(hedge_id, key)) {
        bridge_->complete_hedge(hedge_id, key);
        for (auto& h : hedges_) {
            if (h.used && h.hedge_id == hedge_id) {
                h.used = false;
                break;
            }
        }
    }
}

// ---- quote lifecycle (sendQuoteOrder, RelativeValue.h:2914-2950) -----------

bool host::can_quote_external(uint32_t contract) const noexcept {
    if (!m_.quote_external[contract] || !external_quoting_allowed_ ||
        disable_external_quoting_) {
        return false;
    }
    const uint32_t venue = m_.venue[contract];
    return venue < venue_enabled_.size() ? venue_enabled_[venue] != 0 : true;
}

void host::emit_quotes() noexcept {
    if (!can_send_quotes_) {
        // Legacy returns before clearing _hasChange, so the change is retried next cycle.
        ++stats_.quotes_blocked;
        return;
    }
    // `traded[]` decides, not first_traded: a priced+quoted contract lives below it.
    for (uint32_t c = 0; c < m_.n_all; ++c) {
        if (!m_.traded[c]) {
            continue;
        }
        bool notified = false;
        for (uint32_t i = 0; i < SIDE_COUNT; ++i) {
            const side sd = static_cast<side>(i);
            quote& q = s_.quote_of(c, sd);
            if (!q.has_change) {
                continue;
            }
            q.has_change = false;
            quote_request request;
            request.contract = c;
            request.side_ = sd;
            request.price = q.price;
            request.size = q.size;
            request.external_price = q.external_price;
            request.external_size = q.external_size;
            request.quote_external = can_quote_external(c);
            if (request.price == tachyon::NULL_PRICE) {
                ++stats_.quote_removals;
            }
            ++stats_.quotes_emitted;
            if (ports_.send_quote != nullptr) {
                ports_.send_quote(ports_.ctx, request);
            }
            notified = true;
        }
        if (notified) {
            ++stats_.quote_notifications;
            if (ports_.notify_quote != nullptr) {
                ports_.notify_quote(ports_.ctx, c);
            }
        }
    }
}

void host::emit_all_removals() noexcept {
    for (uint32_t c = 0; c < m_.n_all; ++c) {
        if (!m_.traded[c]) {
            continue;
        }
        bool notified = false;
        for (uint32_t i = 0; i < SIDE_COUNT; ++i) {
            const side sd = static_cast<side>(i);
            quote& q = s_.quote_of(c, sd);
            q.has_change = false;
            if (q.price == tachyon::NULL_PRICE && q.external_price == tachyon::NULL_PRICE) {
                continue;  // nothing outstanding
            }
            q.reset();
            quote_request request;
            request.contract = c;
            request.side_ = sd;
            request.quote_external = false;
            ++stats_.quotes_emitted;
            ++stats_.quote_removals;
            if (ports_.send_quote != nullptr) {
                ports_.send_quote(ports_.ctx, request);
            }
            notified = true;
        }
        if (notified) {
            ++stats_.quote_notifications;
            if (ports_.notify_quote != nullptr) {
                ports_.notify_quote(ports_.ctx, c);
            }
        }
    }
}

void host::disable(const char* reason) noexcept {
    if (stats_.disabled) {
        return;
    }
    stats_.disabled = true;
    strategy_->set_enabled(false);
    // Legacy disables external quoting as well, then stops quoting (:2872-2879).
    disable_external_quoting_ = true;
    if (ports_.on_alert != nullptr) {
        ports_.on_alert(ports_.ctx, reason);
    }
    emit_all_removals();
}

// ---- inputs -----------------------------------------------------------------

void host::on_market_data(uint32_t contract, side sd, bool external, price_t price,
                          qty_t size) noexcept {
    if (contract >= m_.n_all) {
        return;
    }
    strategy_->on_market_data(contract, sd, external, price, size);
}

void host::on_fill(uint32_t contract, side sd, qty_t qty) noexcept {
    if (contract >= m_.n_all) {
        return;
    }
    strategy_->on_fill(contract, sd, qty);
}

void host::on_timer(uint64_t now_cycles) noexcept {
    ++stats_.cycles;
    const uint64_t before = stats_.validation_failures;
    strategy_->run_cycle(now_cycles);
    if (stats_.validation_failures > before) {
        // Legacy: one violation aborts the whole calculateQuotes() pass, disables the
        // strategy and disables external quoting (:2871-2880).
        disable("RV MaxTicksThroughMarketCheckFailed");
        return;
    }
    if (!strategy_->enabled() && !stats_.disabled) {
        // The engine disabled itself inside the cycle (driver: hedge exceeded the Rhone clip,
        // S12). Surface it instead of silently going quiet.
        disable("RV disabled during cycle (hedge clip / driver failure)");
        return;
    }
    emit_quotes();
}

}  // namespace tachyon::trade::rv
