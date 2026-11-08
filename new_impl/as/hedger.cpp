#include "hedger.hpp"

namespace tachyon::trade::as {

// legacy updateHedge<Side> (src/ArbieSpreader.h:552-708), transcribed step by step.
template <side S>
hedge_plan hedger::hedge(cycle_state& s, uint32_t contract, qty_t qty,
                         bool is_internal) noexcept {
    constexpr side other_side = other(S);
    hedge_plan plan;
    plan.contract = contract;
    plan.fill_side = S;
    plan.qty = qty;

    // :555-575 -- a fill releases what AS reserved through its own aggressive quotes. An internal
    // fill also releases the same-side internal and external books; every fill releases the
    // *opposite* side's external book, because an aggressive quote there was predicated on a hedge
    // that has now happened.
    if (is_internal) {
        policy_.undo_aggressive_quote(contract, S, false, 0);
        const qty_t same_external = policy_.as_quote(contract, S, true);
        if (same_external > 0) {
            policy_.undo_aggressive_quote(contract, S, true, std::min(qty, same_external));
        }
    }
    const qty_t opposite_external = policy_.as_quote(contract, other_side, true);
    if (opposite_external > 0) {
        policy_.undo_aggressive_quote(contract, other_side, true,
                                      std::min(qty, opposite_external));
    }

    // :577-586 -- can the best path still carry this quantity? If not, re-price the paths and
    // accept whatever valid path comes out (legacy only re-checks *validity* after the recalc).
    uint32_t slot = paths_.best_slot(s, contract, S);
    bool can_hedge = slot != INVALID_INDEX && paths_.can_hedge(s, slot, qty);
    if (!can_hedge) {
        paths_.update_paths<S>(s, contract, false);
        slot = paths_.best_slot(s, contract, S);
        can_hedge = slot != INVALID_INDEX && s.path(slot).valid();
        plan.recalc_forced = true;
    }

    if (!can_hedge) return plan;  // :694-707 -- "Unable to hedge. No valid path."

    const uint32_t d = m_.slot_path[slot];
    plan.hedged = true;
    plan.path_id = m_.path_local_id[d];

    // :588-590 -- one id for the whole hedge event, shared by the legs and their reservations
    plan.hedge_id = ++hedge_id_;

    // :592-608 -- one order per lean, priced off the book the path leaned on
    for (uint32_t k = m_.path_lean_begin[d]; k < m_.path_lean_begin[d + 1]; ++k) {
        if (plan.leg_count == MAX_LEANS_PER_PATH) break;  // cannot happen: caps are validated
        const uint32_t lean = m_.lean_contract[k];
        const int32_t multiplier = m_.lean_mult[k];
        const side book_side = multiplier > 0 ? S : other_side;      // where the path leaned
        const side order_side = multiplier > 0 ? other_side : S;     // the closing direction
        const bool external = s.best_is_external(lean, book_side);
        const market_state& book = s.book(lean, book_side, external);

        hedge_leg& leg = plan.legs[plan.leg_count++];
        leg.contract = lean;
        leg.order_side = order_side;
        leg.internal_price = apply_price_offset(order_side, book.price, m_.hedge_offset_price[lean]);
        leg.external_price = book.price;  // the raw lean price, no payup
        leg.size = qty;
        leg.external = external;
        leg.market_index = market_index(lean, book_side, external);
    }

    // :612-681 -- a switch path hands the position to RV instead of holding it
    if (m_.path_offset[d] != INVALID_INDEX) {
        plan.switch_transfer = true;
        plan.quoted.contract = contract;
        plan.quoted.stf = s.stf(contract);
        plan.quoted.front_side = other_side;
        plan.quoted.rv_side = S;
        // legacy multiplies the per-unit fair move by the fill quantity (D-AS8)
        plan.adjusted_fill_price =
            s.stf(contract) + m_.path_rv_stf_move[d] * static_cast<price_t>(qty);

        for (uint32_t k = m_.path_lean_begin[d]; k < m_.path_lean_begin[d + 1]; ++k) {
            if (plan.switch_leg_count == MAX_LEANS_PER_PATH) break;
            const uint32_t lean = m_.lean_contract[k];
            const int32_t multiplier = m_.lean_mult[k];
            switch_leg& leg = plan.switch_legs[plan.switch_leg_count++];
            leg.contract = lean;
            leg.stf = s.stf(lean);
            // the front notification and the RV transfer disagree on the side in legacy; both are
            // reproduced (PARITY_NOTES X3)
            leg.front_side = multiplier > 0 ? S : other_side;
            leg.rv_side = multiplier > 0 ? other_side : S;
        }
    }

    return plan;
}

template hedge_plan hedger::hedge<side::bid>(cycle_state&, uint32_t, qty_t, bool) noexcept;
template hedge_plan hedger::hedge<side::ask>(cycle_state&, uint32_t, qty_t, bool) noexcept;

}  // namespace tachyon::trade::as
