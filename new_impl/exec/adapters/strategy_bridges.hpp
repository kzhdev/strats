#pragma once

// The inbound half of the integration seam: what the engine tells a strategy.
//
// `strategy_adapters.hpp` moves orders *out* of a strategy and into its mailbox. These bridges move
// the engine's outbound signals back *in*:
//
//   on_internal_market  -> the strategy's own internal-book input (RV/AS; the MM never subscribes)
//   on_fill             -> the strategy's fill handler (positions, then the hedge decision)
//   end_of_fills        -> the MM's per-batch hook
//   on_hedge_done       -> the RV/AS hedge completion (releases the liquidity reservation)
//
// The virtual call is the `strategy_endpoint` one the engine already makes, once per fill or market
// update -- never once per request -- so the bridge stays a thin adapter over the host's own API.

#include <as/host.hpp>
#include <mm/host.hpp>
#include <rv/host.hpp>

#include "ports.hpp"
#include "strategy_adapters.hpp"
#include "types.hpp"

namespace tachyon::trade::exec {

// ---------------------------------------------------------------------------
// Market maker
// ---------------------------------------------------------------------------
class mm_bridge final : public strategy_endpoint {
public:
    explicit mm_bridge(mm::host& host) noexcept : host_(host) {}

    // Legacy asserts the MM is never an internal-market subscriber (:1276): the MM sees only the
    // external book, which the market-data path feeds it.
    void on_internal_market(uint32_t, side, const internal_top&) noexcept override {}

    void on_fill(const internal_fill& fill) noexcept override {
        ++fills;
        mm::fill f;
        f.sd = from_side<mm::side>(fill.sd);
        f.price = fill.price;
        f.qty = fill.size;
        // Everything the implied engine reports is an internal execution; the kind says whether it
        // hit a quote or a hedge order (:562-584).
        f.type = mm::fill_type::internal;
        f.source = fill.kind == request_kind::hedge ? mm::fill_source::hedge : mm::fill_source::quote;
        f.from_other_strategy = true;  // an internal fill always has a counterparty strategy
        const mm::fill_outcome outcome = host_.on_fill(fill.contract, f);
        if (outcome.disable_external_quoting) {
            host_.set_external_quoting_allowed(false);
            ++external_quoting_disables;
        }
        if (outcome.untaken_remaining > 0) untaken_remaining += outcome.untaken_remaining;
        if (outcome.ends_cycle_inline) host_.end_of_fills(fill.contract);
    }

    void end_of_fills(uint32_t contract) noexcept override { host_.end_of_fills(contract); }

    // The MM hedge is an LMT order that rests: there is no completion ack to consume.
    void on_hedge_done(uint32_t, uint32_t, uint32_t, side) noexcept override {}

    // The engine already keeps a self-crossed contract out of the external market (PLAN D-EX14), so
    // the strategy only has to be told, not disabled: it re-quotes on its next cycle.
    void on_self_match(uint32_t, const char*) noexcept override { ++self_matches; }

    bool enabled() const noexcept override { return enabled_; }
    void disable() noexcept override {
        enabled_ = false;
        host_.set_strategy_enabled(false);
        ++disables;
    }

    uint64_t fills = 0;
    uint64_t self_matches = 0;
    uint64_t disables = 0;
    uint64_t external_quoting_disables = 0;
    qty_t untaken_remaining = 0;

private:
    mm::host& host_;
    bool enabled_ = true;
};


// ---------------------------------------------------------------------------
// Relative value
// ---------------------------------------------------------------------------
class rv_bridge final : public strategy_endpoint {
public:
    explicit rv_bridge(rv::host& host) noexcept : host_(host) {}

    void on_internal_market(uint32_t contract, side sd, const internal_top& top) noexcept override {
        ++market_updates;
        // An empty top is "the internal book is gone", which the strategy's market object models as
        // a NULL price and zero size.
        host_.on_market_data(contract, from_side<rv::side>(sd), /*external=*/false, top.price,
                             top.size);
    }

    void on_fill(const internal_fill& fill) noexcept override {
        ++fills;
        // A trade and a MissedHedge correction move the position the same way (the engine already
        // flipped the side for the correction), so one path covers both (:2033-2042).
        host_.on_fill(fill.contract, from_side<rv::side>(fill.sd), fill.size);
    }

    void end_of_fills(uint32_t) noexcept override {}

    void on_hedge_done(uint32_t, uint32_t hedge_id, uint32_t, side) noexcept override {
        ++hedges_done;
        host_.on_hedge_done(hedge_id);
    }

    void on_self_match(uint32_t, const char*) noexcept override { ++self_matches; }

    bool enabled() const noexcept override { return enabled_; }
    void disable() noexcept override {
        enabled_ = false;
        host_.enable(false);
        ++disables;
    }

    rv::host& host() noexcept { return host_; }

    uint64_t market_updates = 0;
    uint64_t fills = 0;
    uint64_t hedges_done = 0;
    uint64_t self_matches = 0;
    uint64_t disables = 0;

private:
    rv::host& host_;
    bool enabled_ = true;
};

// ---------------------------------------------------------------------------
// Arbie spreader
// ---------------------------------------------------------------------------
class as_bridge final : public strategy_endpoint {
public:
    explicit as_bridge(as::host& host) noexcept : host_(host) {}

    void on_internal_market(uint32_t contract, side sd, const internal_top& top) noexcept override {
        ++market_updates;
        host_.on_market_data(contract, from_side<as::side>(sd), /*external=*/false, top.price,
                             top.size);
    }

    void on_fill(const internal_fill& fill) noexcept override {
        ++fills;
        host_.on_fill(fill.contract, from_side<as::side>(fill.sd), fill.size, /*is_internal=*/true);
    }

    void end_of_fills(uint32_t) noexcept override {}

    void on_hedge_done(uint32_t contract, uint32_t hedge_id, uint32_t, side sd) noexcept override {
        ++hedges_done;
        // The ack names the book the hedge went to; the engine reports the filled side, and the
        // internal book it hedged in is the one on that side.
        const market_key target =
            make_market_key(contract, static_cast<order_side>(to_index(sd)), INTERNAL);
        host_.on_hedge_done(hedge_id, target);
    }

    void on_self_match(uint32_t, const char*) noexcept override { ++self_matches; }

    bool enabled() const noexcept override { return enabled_ && host_.enabled(); }
    void disable() noexcept override {
        enabled_ = false;
        host_.enable(false);
        ++disables;
    }

    as::host& host() noexcept { return host_; }

    uint64_t market_updates = 0;
    uint64_t fills = 0;
    uint64_t hedges_done = 0;
    uint64_t self_matches = 0;
    uint64_t disables = 0;

private:
    as::host& host_;
    bool enabled_ = true;
};

}  // namespace tachyon::trade::exec
