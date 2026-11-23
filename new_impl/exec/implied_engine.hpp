#pragma once

#include <array>
#include <cstdint>

#include "implied_book.hpp"
#include "limits.hpp"
#include "order_request.hpp"
#include "ports.hpp"
#include "types.hpp"

// One contract's pipeline: requests in, internal cross, residual out (PLAN.md §2).
//
//     on_request()   stage the request (last-wins streams) or apply it at once (FIFO streams:
//                    RV/AS hedges and AS quotes). A FIFO request flushes the staged set first,
//                    which is what keeps the legacy per-(strategy, kind) ordering (D-EX5).
//          v
//     flush()        apply the staged slots in the legacy order -> cross -> residual to the
//                    exchange -> publish the new internal best to RV and AS
//          v
//     flush_fills()  fan the fills out and close the batch with end_of_fills
//
// Everything above is O(orders in the book) with a bound of SLOTS_PER_CONTRACT, allocation-free,
// and touches one book object plus one staged array.
namespace tachyon::trade::exec {

struct engine_config {
    uint32_t contract = INVALID_CONTRACT;
    // Legacy _relativeValueHedgeMatchingTolerance: an FAK hedge aggresses at
    // internal_price +/- tolerance (adjustPrice, :149-160).
    price_t hedge_tolerance = 0;
    implied_book::price_collision collision = implied_book::price_collision::all_matchable;
};

struct engine_stats {
    uint64_t requests = 0;
    uint64_t staged = 0;
    uint64_t applied_now = 0;
    uint64_t stale_dropped = 0;     // legacy MM exec-token drop (:1980-1993)
    uint64_t disabled_dropped = 0;  // legacy disabled-strategy drop (:1996)
    uint64_t flushes = 0;
    uint64_t crosses = 0;
    uint64_t fills = 0;
    uint64_t self_matches = 0;
    uint64_t as_self_matches = 0;
    uint64_t fill_overflow = 0;
    uint64_t quotes_sent = 0;
    uint64_t hedges_sent = 0;
    uint64_t publishes = 0;
    uint64_t cancels = 0;
};

class implied_engine {
public:
    // Fault codes reported through exchange_gateway::alert.
    enum alert_code : int32_t {
        ALERT_SELF_MATCH = 1,           // non-AS self match, quoting disabled (:1127-1138)
        ALERT_SELF_MATCH_LIMIT = 2,     // MAX_SELF_MATCH_THRESHOLD reached (:1930-1932)
        ALERT_AS_SELF_MATCH_LIMIT = 3,  // AS self match limit reached (:899-907)
        ALERT_FILL_OVERFLOW = 4,        // MAX_FILLS_PER_FLUSH exhausted: a book invariant broke
        ALERT_SKIPPED_SELF_MATCH = 5,   // an FAK hedge skipped its own resting order (:908-915)
    };

    implied_engine() noexcept = default;

    void init(const engine_config& config, exchange_gateway* gateway) noexcept;
    void set_endpoint(strategy_id strategy, strategy_endpoint* endpoint) noexcept {
        endpoints_[to_int(strategy)] = endpoint;
    }
    strategy_endpoint* endpoint(strategy_id strategy) const noexcept {
        return endpoints_[to_int(strategy)];
    }

    // ---- request intake ---------------------------------------------------
    void on_request(const order_request& request) noexcept;

    // ---- cycle ------------------------------------------------------------
    // Apply + cross + emit. Called by the executor once per drained contract; does nothing when
    // nothing is pending.
    void flush(uint64_t now_cycles) noexcept;
    // Fan the fills produced since the last call out to their strategies and close the batch.
    void flush_fills() noexcept;

    // ---- out-of-band commands --------------------------------------------
    // Legacy getStrategyOutOfMarket: clear a strategy's internal orders and cancel its external
    // leg. `disable_gtc` is threaded through to the gateway untouched.
    void cancel_strategy(strategy_id strategy, bool disable_gtc) noexcept;
    void cancel_all(bool disable_gtc) noexcept;
    // Legacy _relativeValueHedgeMatchingTolerance: a config value, so it can change at runtime.
    // The book, the staged requests and the published tops are untouched by a change.
    void set_hedge_tolerance(price_t tolerance) noexcept { hedge_tolerance_ = tolerance; }
    // Legacy _lastMMOrderExecution: everything the MM sent before this token is stale.
    void set_last_mm_execution(uint32_t token) noexcept { last_mm_execution_ = token; }
    uint32_t last_mm_execution() const noexcept { return last_mm_execution_; }

    // ---- inspection -------------------------------------------------------
    uint32_t contract() const noexcept { return book_.contract(); }
    const implied_book& book() const noexcept { return book_; }
    const engine_stats& stats() const noexcept { return stats_; }
    bool self_crossed() const noexcept { return self_crossed_; }
    bool pending() const noexcept { return staged_mask_ != 0; }
    const internal_top& published(strategy_id strategy, side sd) const noexcept {
        return published_[to_int(strategy)][to_index(sd)];
    }

private:
    // Apply one request to the book and cross immediately (FIFO streams).
    void apply_now(const order_request& request) noexcept;
    // Legacy tryMatch (:881-957) for an FAK hedge: the aggressor never rests.
    void cross_fak(const order_request& request) noexcept;
    // The set-level match over the whole book (legacy matchOrders).
    void cross_book() noexcept;
    // Legacy getPriceAndSize + synchronizeQuote: emit the aggregated residual for one side.
    void emit_quote(side sd) noexcept;
    // Emit the residual for every side that moved, then publish if anything did.
    void emit_dirty() noexcept;
    // Legacy sendHedge: emit one hedge leg.
    void emit_hedge(const order_request& request) noexcept;
    // Legacy sendInternalMarketData: publish the best internal price per subscriber. `side_mask`
    // limits the work to the sides that actually moved (bit 0 = bid, bit 1 = ask).
    void publish(uint32_t side_mask = 3) noexcept;
    void record(const internal_fill& fill) noexcept;

    std::array<strategy_endpoint*, STRATEGY_COUNT> endpoints_{};
    exchange_gateway* gateway_ = nullptr;

    implied_book book_{};
    // Staged (last-wins) requests indexed by slot. Ascending slot order is exactly the legacy
    // apply order: MM quote bid/ask, MM hedge bid/ask, RV quote bid/ask.
    std::array<order_request, SLOTS_PER_CONTRACT> staged_{};
    uint32_t staged_mask_ = 0;
    strategy_id flush_aggressor_ = strategy_id::MM;  // D-EX13
    fill_buffer fills_{};

    // Last published internal best, per (subscriber, side) -- legacy InternalMarketSub.
    std::array<std::array<internal_top, SIDE_COUNT>, STRATEGY_COUNT> published_{};
    // Last emitted residual, per side: keeps one exchange order per (contract, side).
    struct sent_quote {
        price_t price = NULL_PRICE;
        qty_t size = 0;
        bool valid = false;
    };
    std::array<sent_quote, SIDE_COUNT> sent_{};

    engine_stats stats_{};
    price_t hedge_tolerance_ = 0;
    uint32_t last_mm_execution_ = 0;
    uint32_t self_match_counter_ = 0;
    uint32_t as_self_match_counter_ = 0;
    bool dirty_ = false;
    // Legacy disableContractExternalQuoteOnAllStrategy (:1928): while the internal book is crossed
    // by one strategy, this contract must not quote externally at all -- the engine would otherwise
    // leave a crossed pair resting on the exchange.
    bool self_crossed_ = false;
};

}  // namespace tachyon::trade::exec
