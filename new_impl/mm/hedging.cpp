#include "hedging.hpp"

namespace tachyon::trade::mm {

void hedger::emit_hedge(intent_log& log, uint32_t c, side sd, hedge_state& h,
                        bool in_payup) const noexcept {
    log.add_hedge(c, sd, h.internal_price, h.external_price, h.size_to_quote, in_payup);
    h.last_internal_price_sent = h.internal_price;
}

bool hedger::reconcile_size(contract_state& s, intent_log& log, uint32_t c) const noexcept {
    bool has_change = false;
    qty_t quote_size = s.hedge_size();

    if (quote_size > 0) {
        // We are net short: bid for the hedge, and pull any ask-side hedge.
        hedge_state& hedge = s.hedge_of(side::bid);
        has_change = hedge.size_to_quote != quote_size;
        hedge.size_to_quote = quote_size;
        hedge_state& other = s.hedge_of(side::ask);
        if (has_change) emit_hedge(log, c, side::bid, hedge, false);
        if (other.size_to_quote > 0) {
            other.reset();
            emit_hedge(log, c, side::ask, other, false);
            has_change = true;
        }
    } else if (quote_size < 0) {
        // Net long: offer the hedge, and pull any bid-side hedge.
        quote_size = -quote_size;
        hedge_state& hedge = s.hedge_of(side::ask);
        has_change = hedge.size_to_quote != quote_size;
        hedge.size_to_quote = quote_size;
        hedge_state& other = s.hedge_of(side::bid);
        if (has_change) emit_hedge(log, c, side::ask, hedge, false);
        if (other.size_to_quote > 0) {
            other.reset();
            emit_hedge(log, c, side::bid, other, false);
            has_change = true;
        }
    } else {
        // Flat: nothing to hedge, so both sides are explicitly pulled.
        hedge_state& bid_hedge = s.hedge_of(side::bid);
        if (bid_hedge.size_to_quote > 0) {
            bid_hedge.reset();
            emit_hedge(log, c, side::bid, bid_hedge, false);
            has_change = true;
        }
        hedge_state& ask_hedge = s.hedge_of(side::ask);
        if (ask_hedge.size_to_quote > 0) {
            ask_hedge.reset();
            emit_hedge(log, c, side::ask, ask_hedge, false);
            has_change = true;
        }
    }

    return has_change;
}

bool hedger::hedge_prices(const contract_state& s, uint32_t c, side sd, price_t& internal_price,
                          price_t& external_price) const noexcept {
    const hedge_state& hedge = s.hedge_of(sd);
    const level& other_book = s.side_book(other(sd)).top();

    // Legacy :1149: payup is decided from the *other* side's book.
    if (in_payup(m_.payup_qty[c], m_.payup_orders[c], m_.payup_multiple[c], hedge.size_to_quote,
                 other_book)) {
        external_price = other_book.price;
        internal_price = apply_price_offset(external_price, m_.commission[c], sd);
        return true;
    }

    // Legacy :1154-1157: reference the more aggressive of our own quote on the other side and
    // that side's touch, step back by the hedge offset, and never price through the mid.
    const price_t our_price = s.quote_of(other(sd)).internal_price;
    price_t ref_price;
    if (our_price == tachyon::NULL_PRICE) {
        ref_price = other_book.price;
    } else {
        ref_price = sd == side::bid ? std::min(our_price, other_book.price)
                                    : std::max(our_price, other_book.price);
    }
    internal_price =
        better_price(apply_price_offset(ref_price, -m_.hedge_offset_price[c], sd), s.mid_point, sd);
    external_price = m_.round(c, internal_price, sd);
    return false;
}

bool hedger::update_price(contract_state& s, intent_log& log, uint32_t c, side sd) const noexcept {
    hedge_state& hedge = s.hedge_of(sd);
    const bool pre_payup = hedge.in_payup;

    price_t internal_price = tachyon::NULL_PRICE;
    price_t external_price = tachyon::NULL_PRICE;
    hedge.in_payup = hedge_prices(s, c, sd, internal_price, external_price);

    if (hedge.in_payup) {
        // Payup re-sends unconditionally (:640-646): the price is chasing the touch.
        hedge.internal_price = internal_price;
        hedge.external_price = external_price;
        emit_hedge(log, c, sd, hedge, true);
        return true;
    }

    // Legacy :648-656, with the difference taken overflow-free (types.hpp::abs_diff).
    const bool price_changed = abs_diff(hedge.last_internal_price_sent, internal_price) >
                               static_cast<uint64_t>(m_.min_internal_price_change[c]);
    if (price_changed || pre_payup != hedge.in_payup) {
        hedge.internal_price = internal_price;
        hedge.external_price = external_price;
        emit_hedge(log, c, sd, hedge, false);
        return true;
    }
    return false;
}

}  // namespace tachyon::trade::mm
