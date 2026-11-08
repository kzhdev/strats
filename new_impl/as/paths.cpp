#include "paths.hpp"

namespace tachyon::trade::as {

// legacy Path::calculate<Side> (src/ArbieSpreader.h:711-831), transcribed branch for branch.
// The comments tag legacy line numbers; behaviour, not shape, is what is preserved:
//   * a path that cannot be priced is *reset*, not left at its previous value, and the caller
//     is told so (the return value feeds the re-sort decision);
//   * the size is capped by the tightest lean and by the configured quote size, and the lean
//     that capped it is remembered for the incremental update;
//   * the short-term-fair offset and the commission are applied only to a valid basket, in the
//     side's direction.
template <side S>
bool path_engine::calculate(cycle_state& s, uint32_t slot, bool force) const noexcept {
    path_state& p = s.path(slot);
    if (!(p.recalc || force)) return false;
    p.recalc = false;

    const uint32_t d = m_.slot_path[slot];
    const uint32_t quoted = m_.path_contract[d];

    // :715-723 -- a disabled path or contract drops out of the ranking
    if (m_.path_enabled[d] == 0 || m_.enabled[quoted] == 0) {
        if (p.valid()) {
            p.reset();
            return true;
        }
        return false;
    }

    // :725-732 -- a switch path whose fair value has not arrived yet (or was cleared because RV
    // was disabled) must not quote at all
    const uint32_t offset = m_.path_offset[d];
    if (offset != INVALID_INDEX && !s.has_offset_value(offset)) {
        if (p.valid()) {
            p.reset();
            return true;
        }
        return false;
    }

    const price_t old_price = p.price;
    const qty_t old_size = p.size;
    const qty_t quote_size = m_.quote_size[quoted];

    // :742-746
    p.price = 0;
    p.size = quote_size;
    p.size_determiner = INVALID_INDEX;
    p.lean_on_rv = false;

    for (uint32_t k = m_.path_lean_begin[d]; k < m_.path_lean_begin[d + 1]; ++k) {
        const uint32_t lean = m_.lean_contract[k];
        const int32_t multiplier = m_.lean_mult[k];

        // :749-752 -- the basket is unusable while any of its leans cannot trade
        if (!s.can_trade(lean)) {
            p.reset();
            break;
        }

        // legacy PathContract::_side: `multiplier > 0 ? path side : other side` (:19)
        const side lean_side = multiplier > 0 ? S : other(S);
        const bool external = s.best_is_external(lean, lean_side);
        const market_state& book = s.book(lean, lean_side, external);

        // :755 -- a lean on RV's own quote market is flagged, because the order manager needs to
        // know that this quote may be filled internally by RV
        if (!external && s.internal_is_rv_quote(lean, lean_side)) p.lean_on_rv = true;

        price_t market_price = book.price;
        // :758-761 -- an external lean is priced net of that contract's external commission
        if (external && market_price != NULL_PRICE) {
            market_price = apply_price_offset<S>(market_price, -m_.ext_commission[lean]);
        }
        if (market_price == NULL_PRICE) {  // :763-766
            p.reset();
            break;
        }

        qty_t size = s.adjusted_size(policy_, lean, lean_side, external);
        if (size == 0) {  // :769-783
            // ... unless AS itself reserved that liquidity: then the zero is ours, and the path
            // looks at the size that excludes our own anticipated hedges instead of dying.
            if (!policy_.aggressive_quote(quoted, other(S), true)) {
                p.reset();
                break;
            }
            size = s.adjusted_size(policy_, lean, lean_side, external,
                                   exclude_anticipated_hedge);
            if (size == 0) {
                p.reset();
                break;
            }
        }

        // :792-793
        s.set_last_path_price(lean, kLegacyLastPriceSide ? S : lean_side, market_price);
        p.price += static_cast<price_t>(multiplier) * market_price;

        // :795-798 -- the tightest lean sets the size and is remembered so that the incremental
        // update knows whose size increase may raise it again (P3)
        if (p.size > size) {
            p.size_determiner = lean;
            p.size = size;
        }
    }

    // :801-804 -- the basket's fair offset and its commission, in the side's direction. A path
    // without an offset (legacy's shared `_zeroStfOffset`) contributes 0, not a lookup.
    if (p.valid()) {
        if (offset != INVALID_INDEX) p.price += s.offset_value(offset);
        p.price = apply_price_offset<S>(p.price, -m_.path_commission[d]);
    }

    // :806-808. Note the asymmetry: a size *increase* only counts while the path is still below
    // its configured quote size, which is why a full recompute is not equivalent to the
    // incremental update (PLAN §4.3).
    return (old_price != p.price) || ((old_size > p.size) && (p.size < quote_size)) ||
           ((old_size < p.size) && (old_size < quote_size));
}

template bool path_engine::calculate<side::bid>(cycle_state&, uint32_t, bool) const noexcept;
template bool path_engine::calculate<side::ask>(cycle_state&, uint32_t, bool) const noexcept;

template <side S>
void path_engine::on_best_market_update(cycle_state& s, uint32_t lean_contract,
                                        price_t old_price) const noexcept {
    const uint32_t key = lean_contract * SIDE_COUNT + to_index(S);
    if (m_.affected_begin[key] == m_.affected_begin[key + 1]) return;

    // legacy computes these once, before touching the paths (:1126-1128)
    const price_t new_price = s.best_price_adjusted(m_, lean_contract, S);
    const price_t price_delta = new_price - old_price;
    const qty_t size =
        s.adjusted_size(policy_, lean_contract, S, s.best_is_external(lean_contract, S));

    for (uint32_t i = m_.affected_begin[key]; i < m_.affected_begin[key + 1]; ++i) {
        const uint32_t slot = m_.affected_slot[i];
        const int32_t multiplier = m_.lean_mult[m_.affected_edge[i]];
        path_state& p = s.path(slot);

        // :836-838 -- a path that is going to be recalculated ignores deltas
        if (p.recalc) continue;

        const uint32_t d = m_.slot_path[slot];
        const uint32_t quoted = m_.path_contract[d];
        const uint32_t offset = m_.path_offset[d];
        const bool is_switch_path = offset != INVALID_INDEX;
        const bool offset_ready = offset == INVALID_INDEX || s.has_offset_value(offset);

        if (!p.valid()) {
            // :841-852 -- an unpriceable path only asks for a recalc once the lean can support
            // one: a real price, a non-zero size, and (for a switch path) a fair value
            if (new_price != NULL_PRICE && size != 0 && offset_ready) p.recalc = true;
            continue;
        }

        if (is_switch_path && !offset_ready) {
            // :854-860 -- the fair value went away (legacy logs "shouldn't be here")
            p.reset();
            p.resort = true;
            continue;
        }

        if (new_price == NULL_PRICE) {  // :862-866
            p.reset();
            p.resort = true;
            continue;
        }

        if (old_price == NULL_PRICE) {  // :868-871
            p.recalc = true;
            continue;
        }

        // :873-877 -- the delta, and the re-sort decision that goes with it
        p.price += static_cast<price_t>(multiplier) * price_delta;
        p.dirty = price_delta != 0;
        const bool is_best = s.order_at(m_, quoted, S, 0) == slot;
        if (is_better_price<S>(new_price, old_price)) {
            // the best path getting better stays best; only a non-best one may need to move up
            p.resort = !is_best;
        } else {
            // only the best path getting worse needs a re-sort
            p.resort = is_best && price_delta != 0;
        }

        qty_t available = size;
        if (available == 0) {  // :888-905
            if (!policy_.aggressive_quote(quoted, other(S), true)) {
                p.reset();
                p.resort = true;
                continue;
            }
            // the zero was our own reservation: use the size without our anticipated hedges
            available = s.adjusted_size(policy_, lean_contract, S,
                                        s.best_is_external(lean_contract, S),
                                        exclude_anticipated_hedge);
            if (available == 0) {
                p.reset();
                p.resort = true;
                continue;
            }
        }

        const qty_t quote_size = m_.quote_size[quoted];
        if (available < p.size) {  // :907-910 -- any decrease applies immediately
            p.size = available;
            p.size_determiner = lean_contract;
            p.resort = true;
        } else if (available > p.size && p.size < quote_size &&
                   (p.size_determiner == INVALID_INDEX || p.size_determiner == lean_contract)) {
            // :911-923 -- a size increase applies only while there is headroom and only when
            // this lean is the one that limited the path; then every lean is re-checked.
            p.size = std::min(available, quote_size);
            for (uint32_t k = m_.path_lean_begin[d]; k < m_.path_lean_begin[d + 1]; ++k) {
                const uint32_t lean = m_.lean_contract[k];
                const side lean_side = m_.lean_mult[k] > 0 ? S : other(S);
                const qty_t lean_size = s.adjusted_size(policy_, lean, lean_side,
                                                        s.best_is_external(lean, lean_side));
                if (lean_size < p.size) {
                    p.size = lean_size;
                    p.size_determiner = lean;
                }
            }
            p.resort = true;
        }

        // :925-927 -- remember the price this lean was last priced at (the canHedge guard).
        // Written on the lean's side, which is what canHedge reads (deviation D-AS6).
        if (price_delta != 0) {
            const side write_side =
                kLegacyLastPriceSide ? static_cast<side>(m_.slot_side[slot]) : S;
            s.set_last_path_price(lean_contract, write_side, new_price);
        }
    }

    // legacy Contract::setMarketDirty (:246-252): revisit the quoted contracts whose paths
    // changed in any way
    for (uint32_t i = m_.affected_begin[key]; i < m_.affected_begin[key + 1]; ++i) {
        const uint32_t slot = m_.affected_slot[i];
        const path_state& p = s.path(slot);
        if (p.dirty || p.recalc || p.resort) s.mark_dirty(m_.slot_contract[slot]);
    }
}

template void path_engine::on_best_market_update<side::bid>(cycle_state&, uint32_t,
                                                            price_t) const noexcept;
template void path_engine::on_best_market_update<side::ask>(cycle_state&, uint32_t,
                                                            price_t) const noexcept;

bool path_engine::update_offset(cycle_state& s, uint32_t offset) const noexcept {
    if (offset == INVALID_INDEX) return false;  // the shared zero offset never changes
    const price_t previous = s.offset_value(offset);

    price_t value = 0;
    for (uint32_t k = m_.offset_term_begin[offset]; k < m_.offset_term_begin[offset + 1]; ++k) {
        const price_t fair = s.stf(m_.offset_term_contract[k]);
        if (fair == NULL_PRICE) {
            if (previous != NULL_PRICE) {
                value = NULL_PRICE;
                break;
            }
            s.set_offset_value(offset, NULL_PRICE);
            return false;
        }
        value += static_cast<price_t>(m_.offset_term_ratio[k]) * fair;
    }

    if (previous == value) return false;

    const uint32_t begin = m_.offset_path_begin[offset];
    const uint32_t end = m_.offset_path_begin[offset + 1];
    if (value == NULL_PRICE) {
        for (uint32_t i = begin; i < end; ++i) {
            path_state& p = s.path(m_.offset_path_slot[i]);
            p.reset();
            p.resort = true;
        }
    } else if (previous == NULL_PRICE) {
        for (uint32_t i = begin; i < end; ++i) s.path(m_.offset_path_slot[i]).recalc = true;
    } else {
        const price_t diff = value - previous;
        for (uint32_t i = begin; i < end; ++i) {
            path_state& p = s.path(m_.offset_path_slot[i]);
            if (p.valid()) {
                p.price += diff;
                p.resort = true;
            }
        }
    }
    s.set_offset_value(offset, value);
    return true;
}

bool path_engine::can_hedge(const cycle_state& s, uint32_t slot, qty_t qty) const noexcept {
    const uint32_t d = m_.slot_path[slot];
    const side own = static_cast<side>(m_.slot_side[slot]);
    for (uint32_t k = m_.path_lean_begin[d]; k < m_.path_lean_begin[d + 1]; ++k) {
        const uint32_t lean = m_.lean_contract[k];
        const side lean_side = m_.lean_mult[k] > 0 ? own : other(own);
        const bool external = s.best_is_external(lean, lean_side);
        const market_state& book = s.book(lean, lean_side, external);
        const price_t last = s.last_path_price(lean, kLegacyLastPriceSide ? own : lean_side);
        // legacy also rejects a market whose id is still __end__ (never published); in this
        // engine that is exactly "the book has no price".
        if (book.price == NULL_PRICE || !s.can_trade(lean) || last == NULL_PRICE ||
            is_better_price(lean_side, last, book.price) ||
            s.adjusted_size(policy_, lean, lean_side, external) < qty) {
            return false;
        }
    }
    return true;
}

}  // namespace tachyon::trade::as

