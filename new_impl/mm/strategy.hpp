#pragma once

#include <cstdint>

#include "hedging.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "order_queue.hpp"
#include "quoting.hpp"
#include "state.hpp"

namespace tachyon::trade::mm {

// The cycle: it runs the pricing and hedging halves in exactly the order legacy does
// (SimpleMarketMaker::onMarketDataChange, :424-510, and applyUpdates, :1194-1210), including the
// cross checks and their forced re-emissions, and hands the resulting request sequence to an
// `intent_log`.
//
// It owns no state of its own beyond the two collaborators: the contract state and the order book
// are passed in, so a cycle can be driven from a host, from a test or from a replay without any
// of them knowing about the others.
class strategy {
public:
    explicit strategy(const model& m) noexcept : m_(m), quotes_(m), hedger_(m) {}

    // Legacy onMarketDataChange (:484-509), after the pricing block has run.
    // Returns true when at least one request must be sent (legacy `hasChange`).
    bool on_md_change(contract_state& s, const order_queue& q, uint32_t c, bool top_book_changed,
                      bool quote_externally, intent_log& log) const noexcept;

    // Legacy applyUpdates (:1194-1210) via forceUpdateQuotesUpdateHedgePrice (:579-601): the
    // size is reconciled first, then both quotes and both hedges are forced out.
    // Returns true when at least one request must be sent.
    bool apply_updates(contract_state& s, const order_queue& q, uint32_t c, bool quote_externally,
                       intent_log& log) const noexcept;

    const quote_engine& quotes() const noexcept { return quotes_; }
    const hedger& hedge() const noexcept { return hedger_; }

    // Legacy checkHedgeCross (:539-570): a hedge and the opposite quote must not cross. Unlike the
    // quote/quote check this can force out *either* instrument, so it lives at this level where
    // both halves are visible. Public so the phase-5 gate can drive it directly.
    bool cross_hedge(contract_state& s, intent_log& log, uint32_t c, side sd,
                     bool quote_externally, bool hedge_changed,
                     bool other_side_quote_changed) const noexcept;

private:
    // Legacy :579-601 (forced quote + hedge refresh, sources 7 and 8).
    bool force_update(contract_state& s, const order_queue& q, uint32_t c, bool quote_externally,
                      intent_log& log) const noexcept;

    const model& m_;
    quote_engine quotes_;
    hedger hedger_;
};

}  // namespace tachyon::trade::mm
