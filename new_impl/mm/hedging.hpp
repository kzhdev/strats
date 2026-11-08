#pragma once

#include <cstdint>

#include "book.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// Legacy isInPayUp (:356-361) lives in book.hpp so that the quoting and hedging halves share one
// transcription. The hedge half is what this file adds.

// The hedge half of the cycle: size reconciliation (:1102-1215), hedge pricing (:1147-1163) and
// the emission of hedge requests. The cross checks that coordinate a hedge with the opposite
// quote live in `strategy`, because they touch both halves.
class hedger {
public:
    explicit hedger(const model& m) noexcept : m_(m) {}

    // Legacy modifyHedgeSize (:1119-1215). The hedge always covers the whole net position, so
    // only one side carries a size; flipping sides resets the other side explicitly and emits it
    // as size 0.
    bool reconcile_size(contract_state& s, intent_log& log, uint32_t c) const noexcept;

    // Legacy updateHedgePrice (:630-659). In payup the hedge is re-sent unconditionally; out of
    // payup it is re-sent when the internal price moved by more than minInternalPriceChange or
    // when the payup state flipped.
    bool update_price(contract_state& s, intent_log& log, uint32_t c, side sd) const noexcept;

    // Legacy calculateHedgePrices (:1147-1163). Returns the payup state.
    bool hedge_prices(const contract_state& s, uint32_t c, side sd, price_t& internal_price,
                      price_t& external_price) const noexcept;

    // Legacy modifyHedgeOrder (:1137-1146): record the intent and remember the sent price.
    void emit_hedge(intent_log& log, uint32_t c, side sd, hedge_state& h,
                    bool in_payup) const noexcept;

private:
    const model& m_;
};

}  // namespace tachyon::trade::mm
