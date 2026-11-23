#pragma once

#include <atomic>
#include <cstdint>

#include "limits.hpp"
#include "ports.hpp"
#include "request_queue.hpp"
#include "types.hpp"

// The return path: engine -> strategy.
//
// `strategy_endpoint` calls arrive on the *engine* thread. That is fine in the single-threaded
// deployment (and is what every gate in this module exercises), but with one thread per strategy it
// would be a cross-thread call into a host that is otherwise thread-confined. `strategy_link` is the
// answer: a per-strategy SPSC ring the engine posts events into, drained by the strategy thread at the
// start of its own cycle. It carries the enabled mirror too, because the engine has to know whether a
// strategy is live before routing work to it (InternalMatchingEngine.h:1996).
//
// Threading contract, identical to the request mailbox in the other direction:
//   engine thread  : post(), set_enabled(), enabled()
//   strategy thread: drain(), and it owns the real endpoint
namespace tachyon::trade::exec {

enum class feedback_kind : uint8_t {
    fill,             // strategy_endpoint::on_fill
    internal_top,     // strategy_endpoint::on_internal_market
    end_of_fills,     // strategy_endpoint::end_of_fills
    hedge_done,       // strategy_endpoint::on_hedge_done
    as_match_done,    // strategy_endpoint::on_as_match_done
    self_match,       // strategy_endpoint::on_self_match
    disable,          // strategy_endpoint::disable
};

// Small enough that a burst of fills fits several to a cache line.
struct feedback_event {
    price_t price = NULL_PRICE;
    qty_t size = 0;
    qty_t second_size = 0;  // as_match_done's ask size
    uint32_t contract = INVALID_CONTRACT;
    uint32_t hedge_id = 0;
    uint32_t market_id = 0;
    uint8_t kind = 0;  // feedback_kind
    uint8_t side = 0;
    uint8_t request_kind = 0;
    uint8_t strategy = 0;
    uint8_t missed = 0;  // fill: legacy FillSource::MissedHedge
};

// 48 bytes: three fields of payload, the identity, and the flags. The ring is walked sequentially, so
// a fixed small event matters more than packing it into a cache line.
static_assert(sizeof(feedback_event) <= 48, "a feedback event must stay small");

template <sync_mode Mode = sync_mode::single>
class strategy_link {
public:
    void init(uint32_t capacity) noexcept { ring_.init(capacity); }

    // ---- engine side ------------------------------------------------------
    bool post(const feedback_event& event) noexcept { return ring_.try_push(event); }
    // The engine must not route work to a disabled strategy; this is the thread-safe mirror of the
    // strategy's own gate (:1996).
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    void set_enabled(bool on) noexcept { enabled_.store(on, std::memory_order_release); }
    uint64_t dropped() const noexcept { return ring_.dropped(); }

    // ---- strategy side ---------------------------------------------------
    template <class Sink>
    uint32_t drain(Sink&& sink) noexcept {
        return ring_.drain([&](const feedback_event& event) { sink(event); });
    }

private:
    spsc_ring<feedback_event, Mode> ring_{};
    std::atomic<bool> enabled_{true};
};

}  // namespace tachyon::trade::exec

namespace tachyon::trade::exec {

// ---------------------------------------------------------------------------
// The two glue pieces, so neither the engine nor the strategies need changes:
//
//   endpoint_via_link  implements `strategy_endpoint` by posting into a link, and is registered with
//                      the executor *instead of* the real endpoint;
//   link_dispatch      the strategy-side sink that replays drained events onto the real endpoint.
// ---------------------------------------------------------------------------
template <sync_mode Mode = sync_mode::single>
class endpoint_via_link final : public strategy_endpoint {
public:
    explicit endpoint_via_link(strategy_link<Mode>& link) noexcept : link_(link) {}

    void on_internal_market(uint32_t contract, side sd, const internal_top& top) noexcept override {
        feedback_event event;
        event.kind = static_cast<uint8_t>(feedback_kind::internal_top);
        event.contract = contract;
        event.side = static_cast<uint8_t>(sd);
        event.price = top.price;
        event.size = top.size;
        post(event);
    }

    void on_fill(const internal_fill& fill) noexcept override {
        feedback_event event;
        event.kind = static_cast<uint8_t>(feedback_kind::fill);
        event.contract = fill.contract;
        event.side = static_cast<uint8_t>(fill.sd);
        event.request_kind = static_cast<uint8_t>(fill.kind);
        event.strategy = static_cast<uint8_t>(fill.strategy);
        event.price = fill.price;
        event.size = fill.size;
        event.missed = fill.missed ? 1 : 0;
        post(event);
    }

    void end_of_fills(uint32_t contract) noexcept override {
        feedback_event event;
        event.kind = static_cast<uint8_t>(feedback_kind::end_of_fills);
        event.contract = contract;
        post(event);
    }

    void on_hedge_done(uint32_t contract, uint32_t hedge_id, uint32_t market_id,
                       side sd) noexcept override {
        feedback_event event;
        event.kind = static_cast<uint8_t>(feedback_kind::hedge_done);
        event.contract = contract;
        event.hedge_id = hedge_id;
        event.market_id = market_id;
        event.side = static_cast<uint8_t>(sd);
        post(event);
    }

    void on_as_match_done(uint32_t contract, qty_t bid_size, qty_t ask_size) noexcept override {
        feedback_event event;
        event.kind = static_cast<uint8_t>(feedback_kind::as_match_done);
        event.contract = contract;
        event.size = bid_size;
        event.second_size = ask_size;
        post(event);
    }

    void on_self_match(uint32_t contract, const char* reason) noexcept override {
        // The reason is a string: a cross-thread event carries the code, not the text.
        (void)reason;
        feedback_event event;
        event.kind = static_cast<uint8_t>(feedback_kind::self_match);
        event.contract = contract;
        post(event);
    }

    void disable() noexcept override {
        feedback_event event;
        event.kind = static_cast<uint8_t>(feedback_kind::disable);
        post(event);
        link_.set_enabled(false);  // stop routing at once, not on the strategy's next drain
    }

    bool enabled() const noexcept override { return link_.enabled(); }

    // Events the ring could not take (cold path: the strategy is not draining).
    uint64_t overflowed() const noexcept { return overflowed_; }

private:
    void post(const feedback_event& event) noexcept {
        if (!link_.post(event)) ++overflowed_;
    }

    strategy_link<Mode>& link_;
    uint64_t overflowed_ = 0;
};

// The strategy-side sink: replays one drained event onto the real endpoint.
class link_dispatch {
public:
    explicit link_dispatch(strategy_endpoint& target) noexcept : target_(target) {}

    void operator()(const feedback_event& event) noexcept {
        switch (static_cast<feedback_kind>(event.kind)) {
            case feedback_kind::fill: {
                internal_fill fill;
                fill.contract = event.contract;
                fill.strategy = static_cast<strategy_id>(event.strategy);
                fill.kind = static_cast<request_kind>(event.request_kind);
                fill.sd = static_cast<side>(event.side);
                fill.price = event.price;
                fill.size = event.size;
                fill.missed = event.missed != 0;
                target_.on_fill(fill);
                break;
            }
            case feedback_kind::internal_top: {
                internal_top top;
                top.price = event.price;
                top.size = event.size;
                target_.on_internal_market(event.contract, static_cast<side>(event.side), top);
                break;
            }
            case feedback_kind::end_of_fills:
                target_.end_of_fills(event.contract);
                break;
            case feedback_kind::hedge_done:
                target_.on_hedge_done(event.contract, event.hedge_id, event.market_id,
                                      static_cast<side>(event.side));
                break;
            case feedback_kind::as_match_done:
                target_.on_as_match_done(event.contract, event.size, event.second_size);
                break;
            case feedback_kind::self_match:
                target_.on_self_match(event.contract, "self match");
                break;
            case feedback_kind::disable:
                target_.disable();
                break;
        }
    }

private:
    strategy_endpoint& target_;
};

}  // namespace tachyon::trade::exec
