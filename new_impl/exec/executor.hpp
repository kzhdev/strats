#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "implied_engine.hpp"
#include "limits.hpp"
#include "order_request.hpp"
#include "ports.hpp"
#include "request_queue.hpp"
#include "types.hpp"

// The composition root: three mailboxes, one implied engine per contract, one gateway, three
// strategy endpoints.
//
//   MM thread  --push-->  mailbox[MM]  --.
//   RV thread  --push-->  mailbox[RV]  --+-->  run_cycle(): drain -> route -> flush -> fan out
//   AS thread  --push-->  mailbox[AS]  --'
//
// `run_cycle` is the whole cycle and it is the only function on the hot path:
//
//   1. drain each mailbox exactly once, routing each request to its contract's engine;
//   2. flush every touched contract (apply staged -> cross -> residual -> publish);
//   3. fan the fills of every touched contract out and close the batch.
//
// Cost is O(requests drained + contracts touched * SLOTS_PER_CONTRACT): no allocation, no
// lookups beyond one array index per request, no formatting. The synchronisation mode is a
// template parameter so the single-threaded deployment pays nothing for the atomic cursors.
namespace tachyon::trade::exec {

#ifndef EXEC_QUEUE_SPSC
#define EXEC_QUEUE_SPSC 0
#endif

inline constexpr sync_mode DEFAULT_SYNC_MODE =
    EXEC_QUEUE_SPSC ? sync_mode::spsc : sync_mode::single;

struct executor_config {
    uint32_t contracts = 0;  // engine per contract id in [0, contracts)
    uint32_t queue_capacity = DEFAULT_QUEUE_CAPACITY;
    price_t hedge_tolerance = 0;
    implied_book::price_collision collision = implied_book::price_collision::all_matchable;
};

struct executor_stats {
    uint64_t cycles = 0;
    uint64_t requests_drained = 0;
    uint64_t contracts_flushed = 0;
    uint64_t fills_fanned_out = 0;
    uint64_t queue_dropped = 0;      // mailbox back-pressure (cold: the caller alerts)
    uint64_t dropped_bad_contract = 0;
    uint64_t dropped_bad_strategy = 0;
};

template <sync_mode Mode = DEFAULT_SYNC_MODE>
class basic_executor {
public:
    using mailbox_type = request_queue<Mode>;

    basic_executor() = default;

    // One allocation per mailbox and per engine, all at init time.
    void init(const executor_config& config, exchange_gateway* gateway);

    // ---- wiring -----------------------------------------------------------
    void register_strategy(strategy_id strategy, strategy_endpoint* endpoint) noexcept;
    void set_hedge_tolerance(uint32_t contract, price_t tolerance) noexcept;

    // ---- producer side (called by the strategies) -------------------------
    mailbox_type& mailbox(strategy_id strategy) noexcept { return mailboxes_[to_int(strategy)]; }
    const mailbox_type& mailbox(strategy_id strategy) const noexcept {
        return mailboxes_[to_int(strategy)];
    }
    bool submit(strategy_id strategy, const order_request& request) noexcept {
        return mailboxes_[to_int(strategy)].try_push(request);
    }

    // ---- consumer side ----------------------------------------------------
    // Drain, route, flush, fan out. The only function on the per-cycle path.
    void run_cycle(uint64_t now_cycles) noexcept;

    // ---- out-of-band ------------------------------------------------------
    void cancel_strategy(strategy_id strategy, bool disable_gtc) noexcept;
    void cancel_contract(uint32_t contract, strategy_id strategy, bool disable_gtc) noexcept;
    void cancel_all(bool disable_gtc) noexcept;
    void set_last_mm_execution(uint32_t contract, uint32_t token) noexcept;
    // Cold-path fault report (legacy sendErrorMessage), used by the adapters.
    void alert(uint32_t contract, const char* reason, int32_t code) noexcept {
        if (gateway_ != nullptr) gateway_->alert(contract, reason, code);
    }

    // ---- inspection -------------------------------------------------------
    uint32_t contract_count() const noexcept { return static_cast<uint32_t>(engines_.size()); }
    implied_engine& engine(uint32_t contract) noexcept { return engines_[contract]; }
    const implied_engine& engine(uint32_t contract) const noexcept { return engines_[contract]; }
    const executor_stats& stats() const noexcept { return stats_; }
    bool ready() const noexcept { return !engines_.empty(); }

private:
    static constexpr uint32_t TOUCH_WORDS = CONTRACT_WORDS;

    void dispatch(const order_request& request) noexcept;
    void mark_touched(uint32_t contract) noexcept {
        touched_[contract >> 6] |= 1ull << (contract & 63);
    }

    std::array<mailbox_type, STRATEGY_COUNT> mailboxes_{};
    std::array<strategy_endpoint*, STRATEGY_COUNT> endpoints_{};
    std::vector<implied_engine> engines_;
    exchange_gateway* gateway_ = nullptr;

    executor_config config_{};
    executor_stats stats_{};
    std::array<uint64_t, TOUCH_WORDS> touched_{};
    std::array<uint64_t, STRATEGY_COUNT> mailbox_dropped_seen_{};
    std::array<bool, STRATEGY_COUNT> mailbox_overflowing_{};
};

using executor = basic_executor<DEFAULT_SYNC_MODE>;

// ---------------------------------------------------------------------------
// Implementation (header-only: the class is a template on the synchronisation mode, and the
// instantiation gate for both modes lives in executor.cpp).
// ---------------------------------------------------------------------------
template <sync_mode Mode>
void basic_executor<Mode>::init(const executor_config& config, exchange_gateway* gateway) {
    config_ = config;
    gateway_ = gateway;
    for (mailbox_type& queue : mailboxes_) {
        if (queue.capacity() == 0) queue.init(config.queue_capacity);
    }
    engines_.clear();
    engines_.resize(config.contracts);
    for (uint32_t c = 0; c < config.contracts; ++c) {
        engine_config engine_cfg;
        engine_cfg.contract = c;
        engine_cfg.hedge_tolerance = config.hedge_tolerance;
        engine_cfg.collision = config.collision;
        engines_[c].init(engine_cfg, gateway);
    }
    touched_.fill(0);
    mailbox_dropped_seen_.fill(0);
    mailbox_overflowing_.fill(false);
    stats_ = executor_stats{};
}

template <sync_mode Mode>
void basic_executor<Mode>::register_strategy(strategy_id strategy,
                                             strategy_endpoint* endpoint) noexcept {
    endpoints_[to_int(strategy)] = endpoint;
    for (implied_engine& engine : engines_) engine.set_endpoint(strategy, endpoint);
}

template <sync_mode Mode>
void basic_executor<Mode>::set_hedge_tolerance(uint32_t contract, price_t tolerance) noexcept {
    // Cold path (config change). The engine keeps its book, its staged requests and everything it
    // has already published; only the aggressiveness of FAK hedges changes.
    if (contract < engines_.size()) engines_[contract].set_hedge_tolerance(tolerance);
}

template <sync_mode Mode>
void basic_executor<Mode>::dispatch(const order_request& request) noexcept {
    if (request.contract >= engines_.size()) {
        ++stats_.dropped_bad_contract;
        return;
    }
    if (request.strategy >= STRATEGY_COUNT) {
        ++stats_.dropped_bad_strategy;
        return;
    }
    // Mark before handing the request over: a FIFO stream may flush the engine inside on_request.
    mark_touched(request.contract);
    engines_[request.contract].on_request(request);
}

template <sync_mode Mode>
void basic_executor<Mode>::run_cycle(uint64_t now_cycles) noexcept {
    ++stats_.cycles;

    // 1. drain. Strategy order is fixed (MM, RV, AS) so a replay is deterministic.
    for (uint32_t s = 0; s < STRATEGY_COUNT; ++s) {
        mailbox_type& queue = mailboxes_[s];
        const uint32_t drained = queue.drain([this](const order_request& request) {
            dispatch(request);
        });
        stats_.requests_drained += drained;

        // Back-pressure is a cold path, and a sustained overflow must not alert on every cycle: the
        // counter tracks *episodes* (rising edges), so a strategy that keeps hitting a full mailbox
        // raises one alert per episode and the caller can alert on the count instead.
        const uint64_t dropped = queue.dropped();
        const bool overflowing = dropped != mailbox_dropped_seen_[s];
        if (overflowing) mailbox_dropped_seen_[s] = dropped;
        if (overflowing && !mailbox_overflowing_[s]) {
            ++stats_.queue_dropped;
            if (gateway_ != nullptr) {
                gateway_->alert(INVALID_CONTRACT, "Order request mailbox overflow", 10);
            }
        }
        mailbox_overflowing_[s] = overflowing;
    }

    // 2. flush + 3. fan out, in contract order.
    for (uint32_t word = 0; word < TOUCH_WORDS; ++word) {
        uint64_t bits = touched_[word];
        if (bits == 0) continue;
        touched_[word] = 0;
        do {
            const uint32_t contract = word * 64 + static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            if (contract >= engines_.size()) continue;
            implied_engine& engine = engines_[contract];
            engine.flush(now_cycles);
            engine.flush_fills();
            stats_.fills_fanned_out += engine.stats().fills;
            ++stats_.contracts_flushed;
        } while (bits != 0);
    }
}

template <sync_mode Mode>
void basic_executor<Mode>::cancel_strategy(strategy_id strategy, bool disable_gtc) noexcept {
    for (implied_engine& engine : engines_) engine.cancel_strategy(strategy, disable_gtc);
}

template <sync_mode Mode>
void basic_executor<Mode>::cancel_contract(uint32_t contract, strategy_id strategy,
                                           bool disable_gtc) noexcept {
    if (contract >= engines_.size()) return;
    engines_[contract].cancel_strategy(strategy, disable_gtc);
}

template <sync_mode Mode>
void basic_executor<Mode>::cancel_all(bool disable_gtc) noexcept {
    for (implied_engine& engine : engines_) engine.cancel_all(disable_gtc);
}

template <sync_mode Mode>
void basic_executor<Mode>::set_last_mm_execution(uint32_t contract, uint32_t token) noexcept {
    if (contract >= engines_.size()) return;
    engines_[contract].set_last_mm_execution(token);
}

}  // namespace tachyon::trade::exec
