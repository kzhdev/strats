#include "quotes.hpp"

namespace tachyon::trade::as {

// legacy updateQuote<Side> (src/ArbieSpreader.h:1048-1113), transcribed branch for branch.
// The quote is built from the *best* path (rank 0 of the current ranking), so the ordering that
// P2/P3 gate is what decides the quote here.
template <side S>
quote_status quote_engine::build_side(cycle_state& s, uint32_t contract, bool force,
                                      bool can_quote_external, bool enabled) const noexcept {
    quote& q = s.quote_of(contract, S);
    const uint32_t slot = paths_.best_slot(s, contract, S);

    if (slot != INVALID_INDEX && s.path(slot).valid()) {
        const path_state& p = s.path(slot);
        const uint32_t d = m_.slot_path[slot];

        // :1053-1056
        q.lean_price = apply_price_offset<S>(p.price, m_.path_commission[d]);
        q.internal_price = apply_price_offset<S>(p.price, -m_.edge[contract]);
        q.internal_size = p.size;
        q.hedge_path = static_cast<int32_t>(m_.path_local_id[d]);

        // :1057-1063
        if (can_quote_external) {
            q.external_price = round_to_tick(q.internal_price, m_.tick_div[contract], S);
            q.external_size = external_size<S>(s, contract, q);
        } else {
            q.external_price = NULL_PRICE;
            q.external_size = 0;
        }

        // :1065-1079 -- the strategy is only in the market while it is enabled; the fields above
        // are still maintained, exactly as legacy does
        if (!enabled) return quote_status::unchanged;
        if (!within_max_ticks<S>(s, contract, q)) return quote_status::max_ticks_violation;

        // :1081-1093 -- change detection compares the four emitted fields only, which is what
        // `quote::operator==` does (legacy Quote::operator!=, :214-219)
        if (s.last_sent(contract, S) != q || s.force_update(contract, S) || force) {
            s.last_sent(contract, S) = q;
            s.set_force_update(contract, S, false);
            return quote_status::emit;
        }
        return quote_status::unchanged;
    }

    // :1095-1110 -- no usable path. A cancellation is only owed while a quote is outstanding;
    // `_hedgePath != -1` is exactly that test.
    if (q.hedge_path != -1) {
        q.reset();
        if (!enabled) return quote_status::unchanged;
        s.last_sent(contract, S) = q;
        s.set_force_update(contract, S, false);
        return quote_status::emit;
    }
    return quote_status::unchanged;
}

template quote_status quote_engine::build_side<side::bid>(cycle_state&, uint32_t, bool, bool,
                                                          bool) const noexcept;
template quote_status quote_engine::build_side<side::ask>(cycle_state&, uint32_t, bool, bool,
                                                          bool) const noexcept;

}  // namespace tachyon::trade::as
