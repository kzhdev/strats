#pragma once

// Reference oracle for the AS hedge stage: a transcription of legacy
// ArbieSpreader::updateHedge<Side> (src/ArbieSpreader.h:552-708) -- the aggressive-quote undo
// order, the canHedge veto with its forced recalculation, the per-lean order construction and the
// switch-path transfer.
//
// It keeps its own hedge id counter and re-derives every expression from the legacy text, so the
// differential cannot pass on a shared mistake. The order *transport* and the FAK reservations are
// not modelled on either side: they belong to the host.

#include <algorithm>
#include <cstdint>

#include "hedger.hpp"
#include "model.hpp"
#include "paths.hpp"
#include "policy.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::as::oracle {

struct undo_call {
    uint32_t contract;
    side sd;
    bool external;
    qty_t qty;
    bool attempted;
};

// The undo sequence as three ordered attempts, so the gate can compare *which* books were touched
// and with what quantity, not just the final plan.
struct undo_plan {
    undo_call calls[3];
    uint8_t count = 0;
};

inline undo_plan undo_quotes(const quote_policy& policy, uint32_t contract, side fill_side,
                             qty_t qty, bool is_internal) noexcept {
    const side other_side = other(fill_side);
    undo_plan u;
    if (is_internal) {
        u.calls[u.count++] = {contract, fill_side, false, 0, true};
        const qty_t same = policy.as_quote(contract, fill_side, true);
        if (same > 0) {
            u.calls[u.count++] = {contract, fill_side, true, std::min(qty, same), true};
        }
    }
    const qty_t opposite = policy.as_quote(contract, other_side, true);
    if (opposite > 0) {
        u.calls[u.count++] = {contract, other_side, true, std::min(qty, opposite), true};
    }
    return u;
}

template <side S>
hedge_plan hedge(const model& m, cycle_state& s, const path_engine& paths,
                 const quote_policy& policy, uint32_t& hedge_id, uint32_t contract, qty_t qty,
                 bool is_internal) noexcept {
    constexpr side other_side = other(S);
    hedge_plan plan;
    plan.contract = contract;
    plan.fill_side = S;
    plan.qty = qty;

    undo_quotes(policy, contract, S, qty, is_internal);

    uint32_t slot = paths.best_slot(s, contract, S);
    bool can = slot != INVALID_INDEX && paths.can_hedge(s, slot, qty);
    if (!can) {
        paths.update_paths<S>(s, contract, false);
        slot = paths.best_slot(s, contract, S);
        can = slot != INVALID_INDEX && s.path(slot).valid();
        plan.recalc_forced = true;
    }
    if (!can) return plan;

    const uint32_t d = m.slot_path[slot];
    plan.hedged = true;
    plan.path_id = m.path_local_id[d];
    plan.hedge_id = ++hedge_id;

    for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
        if (plan.leg_count == MAX_LEANS_PER_PATH) break;
        const uint32_t lean = m.lean_contract[k];
        const int32_t multiplier = m.lean_mult[k];
        const side book_side = multiplier > 0 ? S : other_side;
        const side order_side = multiplier > 0 ? other_side : S;
        const bool external = s.best_is_external(lean, book_side);
        const market_state& book = s.book(lean, book_side, external);
        hedge_leg& leg = plan.legs[plan.leg_count++];
        leg.contract = lean;
        leg.order_side = order_side;
        leg.internal_price =
            apply_price_offset(order_side, book.price, m.hedge_offset_price[lean]);
        leg.external_price = book.price;
        leg.size = qty;
        leg.external = external;
        leg.market_index = market_index(lean, book_side, external);
    }

    if (m.path_offset[d] != INVALID_INDEX) {
        plan.switch_transfer = true;
        plan.quoted.contract = contract;
        plan.quoted.stf = s.stf(contract);
        plan.quoted.front_side = other_side;
        plan.quoted.rv_side = S;
        plan.adjusted_fill_price = s.stf(contract) + m.path_rv_stf_move[d] * static_cast<price_t>(qty);
        for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
            if (plan.switch_leg_count == MAX_LEANS_PER_PATH) break;
            const uint32_t lean = m.lean_contract[k];
            const int32_t multiplier = m.lean_mult[k];
            switch_leg& leg = plan.switch_legs[plan.switch_leg_count++];
            leg.contract = lean;
            leg.stf = s.stf(lean);
            leg.front_side = multiplier > 0 ? S : other_side;
            leg.rv_side = multiplier > 0 ? other_side : S;
        }
    }
    return plan;
}

}  // namespace tachyon::trade::as::oracle
