#include "strategy.hpp"

namespace tachyon::trade::mm {

bool strategy::cross_hedge(contract_state& s, intent_log& log, uint32_t c, side sd,
                           bool quote_externally, bool hedge_changed,
                           bool other_side_quote_changed) const noexcept {
    hedge_state& hedge = s.hedge_of(sd);
    // Legacy :541-543: a hedge in payup is chasing the touch on purpose, so the cross check is
    // suspended for it.
    if (hedge.in_payup) return false;

    quote_state& opposite = s.quote_of(other(sd));
    const bool would_cross = hedge.internal_price != tachyon::NULL_PRICE &&
                             opposite.last_internal_price_sent != tachyon::NULL_PRICE &&
                             is_better_or_equal(hedge.internal_price,
                                                opposite.last_internal_price_sent, sd);

    if (hedge_changed && !other_side_quote_changed) {
        // The hedge moved onto (or through) the opposite quote: push that quote out.
        if (would_cross) {
            quotes_.emit_quote(log, c, other(sd), opposite, quote_externally);
            return true;
        }
    } else if (!hedge_changed && other_side_quote_changed) {
        // The opposite quote moved onto the hedge: push the hedge out.
        if (would_cross) {
            hedger_.emit_hedge(log, c, sd, hedge, false);
            return true;
        }
    }
    return false;
}

bool strategy::force_update(contract_state& s, const order_queue& q, uint32_t c,
                            bool quote_externally, intent_log& log) const noexcept {
    // Legacy forceUpdateQuotesUpdateHedgePrice (:579-601): both quotes are recalculated with
    // forceUpdate=true, the cross check runs, then each active hedge is repriced and re-checked.
    bool bid_changed = quotes_.update_side(s, q, c, side::bid, false, quote_externally, true, &log);
    bool ask_changed = quotes_.update_side(s, q, c, side::ask, false, quote_externally, true, &log);
    quotes_.cross_quote(s, log, c, quote_externally, bid_changed, ask_changed);
    bool has_change = bid_changed | ask_changed;

    if (s.is_hedging(side::bid)) {
        const bool hedge_changed = hedger_.update_price(s, log, c, side::bid);
        has_change |= hedge_changed;
        has_change |=
            cross_hedge(s, log, c, side::bid, quote_externally, hedge_changed, ask_changed);
    }
    if (s.is_hedging(side::ask)) {
        const bool hedge_changed = hedger_.update_price(s, log, c, side::ask);
        has_change |= hedge_changed;
        has_change |=
            cross_hedge(s, log, c, side::ask, quote_externally, hedge_changed, bid_changed);
    }
    return has_change;
}

bool strategy::on_md_change(contract_state& s, const order_queue& q, uint32_t c,
                            bool top_book_changed, bool quote_externally,
                            intent_log& log) const noexcept {
    // Legacy :486-509, in order: both quotes, the quote/quote cross check, then each hedge and its
    // cross check against the *opposite* quote's change flag.
    bool bid_changed =
        quotes_.update_side(s, q, c, side::bid, top_book_changed, quote_externally, false, &log);
    bool ask_changed =
        quotes_.update_side(s, q, c, side::ask, top_book_changed, quote_externally, false, &log);
    quotes_.cross_quote(s, log, c, quote_externally, bid_changed, ask_changed);
    bool has_change = bid_changed | ask_changed;

    if (top_book_changed || ask_changed) {
        if (s.is_hedging(side::bid)) {
            const bool hedge_changed = hedger_.update_price(s, log, c, side::bid);
            has_change |= hedge_changed;
            has_change |=
                cross_hedge(s, log, c, side::bid, quote_externally, hedge_changed, ask_changed);
        }
    }
    if (top_book_changed || bid_changed) {
        if (s.is_hedging(side::ask)) {
            const bool hedge_changed = hedger_.update_price(s, log, c, side::ask);
            has_change |= hedge_changed;
            has_change |=
                cross_hedge(s, log, c, side::ask, quote_externally, hedge_changed, bid_changed);
        }
    }
    return has_change;
}

bool strategy::apply_updates(contract_state& s, const order_queue& q, uint32_t c,
                             bool quote_externally, intent_log& log) const noexcept {
    // Legacy applyUpdates (:1194-1210): the hedge size is reconciled first, then everything is
    // forced out. The enable / market-data gates belong to the host.
    const bool hedge_size_changed = hedger_.reconcile_size(s, log, c);
    const bool changed = force_update(s, q, c, quote_externally, log);
    return changed | hedge_size_changed;
}

}  // namespace tachyon::trade::mm
