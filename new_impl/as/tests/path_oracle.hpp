#pragma once

// Reference oracle for the AS path calculation: a *transcription* of legacy
// Path::calculate<Side> (src/ArbieSpreader.h:711-831), PathSorter (:108-130) and
// Contract::updatePaths (:931-985).
//
// It deliberately shares nothing with as/paths.hpp: it keeps its own per-path record and
// re-derives the lean side, the commission direction and the change predicate from the legacy
// text. Sharing the engine would make the differential gate report parity on a shared bug
// (rv PARITY_NOTES §5).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "model.hpp"
#include "policy.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::as::oracle {

// legacy ArbieSpreader::PriceQtyPair
struct price_qty {
    price_t price = NULL_PRICE;  // legacy _price
    qty_t size = 0;              // legacy _size (reset() sets 0; the default ctor uses max())

    bool valid() const noexcept { return price != NULL_PRICE; }
    void reset() noexcept {
        price = NULL_PRICE;
        size = 0;
    }
};

// legacy ArbieSpreader::Path, reduced to the fields the calculation touches.
struct path {
    price_qty pq;
    uint32_t size_determiner = INVALID_INDEX;  // legacy _sizeDetermineContract (nullptr)
    bool lean_on_rv = false;                   // legacy _leanOnRV
    bool recalc = false;                       // legacy _recalc {false}
    bool resort = false;                       // legacy _resort {false}
    bool dirty = false;                        // legacy _dirty {false}
};

// legacy PathSorter<Side>::operator() -- lexicographic on (valid, price*factor), then size.
inline bool sorter_less(const path& a, const path& b, side sd) noexcept {
    const bool a_valid = a.pq.price != NULL_PRICE;
    const bool b_valid = b.pq.price != NULL_PRICE;
    if (a_valid != b_valid) return a_valid;
    if (a.pq.price != b.pq.price) {
        return sd == side::bid ? a.pq.price > b.pq.price : a.pq.price < b.pq.price;
    }
    return a.pq.size > b.pq.size;
}

}  // namespace tachyon::trade::as::oracle
namespace tachyon::trade::as::oracle {

// legacy Path::calculate<Side>: a full recompute, given the same engine inputs.
template <side S>
bool calculate(const model& m, const cycle_state& s, const quote_policy& policy, path& p,
               uint32_t d, bool force) noexcept {
    if (!(p.recalc || force)) return false;
    p.recalc = false;

    const uint32_t quoted = m.path_contract[d];
    if (m.path_enabled[d] == 0 || m.enabled[quoted] == 0) {
        if (p.pq.valid()) {
            p.pq.reset();
            return true;
        }
        return false;
    }

    const uint32_t offset = m.path_offset[d];
    const price_t offset_value = offset == INVALID_INDEX ? 0 : s.offset_value(offset);
    if (offset != INVALID_INDEX && offset_value == NULL_PRICE) {  // fair value not received yet
        if (p.pq.valid()) {
            p.pq.reset();
            return true;
        }
        return false;
    }

    const price_t old_price = p.pq.price;
    const qty_t old_size = p.pq.size;
    const qty_t quote_size = m.quote_size[quoted];

    p.pq.price = 0;
    p.pq.size = quote_size;
    p.size_determiner = INVALID_INDEX;
    p.lean_on_rv = false;

    for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
        const uint32_t lean = m.lean_contract[k];
        const int32_t multiplier = m.lean_mult[k];
        if (!s.can_trade(lean)) {
            p.pq.reset();
            break;
        }
        const side lean_side = multiplier > 0 ? S : other(S);
        const bool external = s.best_is_external(lean, lean_side);
        const market_state& book = s.book(lean, lean_side, external);

        if (!external && s.internal_is_rv_quote(lean, lean_side)) p.lean_on_rv = true;

        price_t market_price = book.price;
        if (external && market_price != NULL_PRICE) {
            market_price = apply_price_offset<S>(market_price, -m.ext_commission[lean]);
        }
        if (market_price == NULL_PRICE) {
            p.pq.reset();
            break;
        }

        qty_t size = s.adjusted_size(policy, lean, lean_side, external);
        if (size == 0) {
            if (!policy.aggressive_quote(quoted, other(S), true)) {
                p.pq.reset();
                break;
            }
            size = s.adjusted_size(policy, lean, lean_side, external,
                                   exclude_anticipated_hedge);
            if (size == 0) {
                p.pq.reset();
                break;
            }
        }

        p.pq.price += static_cast<price_t>(multiplier) * market_price;
        if (p.pq.size > size) {
            p.size_determiner = lean;
            p.pq.size = size;
        }
    }

    if (p.pq.valid()) {
        p.pq.price += offset_value;
        p.pq.price = apply_price_offset<S>(p.pq.price, -m.path_commission[d]);
    }

    return (old_price != p.pq.price) || ((old_size > p.pq.size) && (p.pq.size < quote_size)) ||
           ((old_size < p.pq.size) && (old_size < quote_size));
}

// legacy Contract::updatePaths<Side>: walk the current ranking, recalc what is flagged, then
// std::sort when anything asked for it.
template <side S>
bool update_paths(const model& m, const cycle_state& s, const quote_policy& policy,
                  std::vector<path>& paths, std::vector<uint32_t>& order, uint32_t contract,
                  bool force) noexcept {
    const uint32_t key = contract * SIDE_COUNT + to_index(S);
    const uint32_t begin = m.side_path_begin[key];
    const uint32_t count = m.side_path_begin[key + 1] - begin;
    bool sort = false;
    for (uint32_t rank = 0; rank < count; ++rank) {
        const uint32_t slot = order[begin + rank];
        path& p = paths[slot];
        const bool changed =
            (p.recalc || force) ? calculate<S>(m, s, policy, p, m.slot_path[slot], force) : false;
        sort |= changed | p.resort;
        p.resort = false;
        p.dirty = false;
    }
    if (sort) {
        std::sort(order.begin() + begin, order.begin() + begin + count,
                  [&paths](uint32_t a, uint32_t b) {
                      return sorter_less(paths[a], paths[b], S);
                  });
    }
    return sort;
}

// legacy Contract::updateAffectedPathsBestMarket<Side> (:1125-1142) + Path::onBestMarketDataUpdate
// (:833-928). `order` is the oracle's own ranking (rank 0 at side_path_begin).
template <side S>
void on_path_best_update(const model& m, cycle_state& s, const quote_policy& policy, path& p,
                         std::vector<uint32_t>& order, uint32_t slot, uint32_t lean_contract,
                         price_t old_price, price_t new_price, price_t price_delta, qty_t size,
                         int32_t multiplier) noexcept {
    if (p.recalc) return;

    const uint32_t d = m.slot_path[slot];
    const uint32_t quoted = m.path_contract[d];
    const uint32_t offset = m.path_offset[d];
    const bool offset_ready = offset == INVALID_INDEX || s.has_offset_value(offset);

    if (!p.pq.valid()) {
        if (new_price != NULL_PRICE && size != 0 && offset_ready) p.recalc = true;
        return;
    }
    if (offset != INVALID_INDEX && !offset_ready) {
        p.pq.reset();
        p.resort = true;
        return;
    }
    if (new_price == NULL_PRICE) {
        p.pq.reset();
        p.resort = true;
        return;
    }
    if (old_price == NULL_PRICE) {
        p.recalc = true;
        return;
    }

    p.pq.price += static_cast<price_t>(multiplier) * price_delta;
    p.dirty = price_delta != 0;
    const bool is_best = order[m.side_path_begin[quoted * SIDE_COUNT + to_index(S)]] == slot;
    if (is_better_price<S>(new_price, old_price)) {
        p.resort = !is_best;
    } else {
        p.resort = is_best && price_delta != 0;
    }

    if (size == 0) {
        if (!policy.aggressive_quote(quoted, other(S), true)) {
            p.pq.reset();
            p.resort = true;
            return;
        }
        const bool external = s.best_is_external(lean_contract, S);
        size = s.adjusted_size(policy, lean_contract, S, external, exclude_anticipated_hedge);
        if (size == 0) {
            p.pq.reset();
            p.resort = true;
            return;
        }
    }

    const qty_t quote_size = m.quote_size[quoted];
    if (size < p.pq.size) {
        p.pq.size = size;
        p.size_determiner = lean_contract;
        p.resort = true;
    } else if (size > p.pq.size && p.pq.size < quote_size &&
               (p.size_determiner == INVALID_INDEX || p.size_determiner == lean_contract)) {
        p.pq.size = std::min(size, quote_size);
        for (uint32_t k = m.path_lean_begin[d]; k < m.path_lean_begin[d + 1]; ++k) {
            const uint32_t lean = m.lean_contract[k];
            const side lean_side = m.lean_mult[k] > 0 ? S : other(S);
            const qty_t lean_size =
                s.adjusted_size(policy, lean, lean_side, s.best_is_external(lean, lean_side));
            if (lean_size < p.pq.size) {
                p.pq.size = lean_size;
                p.size_determiner = lean;
            }
        }
        p.resort = true;
    }
}



template <side S>
void on_best_market_update(const model& m, cycle_state& s, const quote_policy& policy,
                           std::vector<path>& paths, std::vector<uint32_t>& order,
                           uint32_t lean_contract, price_t old_price) noexcept {
    const uint32_t key = lean_contract * SIDE_COUNT + to_index(S);
    if (m.affected_begin[key] == m.affected_begin[key + 1]) return;

    const price_t new_price = s.best_price_adjusted(m, lean_contract, S);
    const price_t price_delta = new_price - old_price;
    const qty_t size =
        s.adjusted_size(policy, lean_contract, S, s.best_is_external(lean_contract, S));

    for (uint32_t i = m.affected_begin[key]; i < m.affected_begin[key + 1]; ++i) {
        const uint32_t slot = m.affected_slot[i];
        const int32_t multiplier = m.lean_mult[m.affected_edge[i]];
        on_path_best_update<S>(m, s, policy, paths[slot], order, slot, lean_contract, old_price,
                               new_price, price_delta, size, multiplier);
    }
    for (uint32_t i = m.affected_begin[key]; i < m.affected_begin[key + 1]; ++i) {
        const uint32_t slot = m.affected_slot[i];
        const path& p = paths[slot];
        if (p.dirty || p.recalc || p.resort) s.mark_dirty(m.slot_contract[slot]);
    }
}

// legacy ShortTermFairOffset::calculateShortTermFairOffset (:199-243). `previous` is passed in
// by the caller because the engine has already written the new value into the shared state; the
// two implementations must not read each other's result.
inline bool update_offset(const model& m, cycle_state& s, std::vector<path>& paths,
                          uint32_t offset, price_t previous) noexcept {
    if (offset == INVALID_INDEX) return false;
    price_t value = 0;
    for (uint32_t k = m.offset_term_begin[offset]; k < m.offset_term_begin[offset + 1]; ++k) {
        const price_t fair = s.stf(m.offset_term_contract[k]);
        if (fair == NULL_PRICE) {
            if (previous != NULL_PRICE) {
                value = NULL_PRICE;
                break;
            }
            return false;
        }
        value += static_cast<price_t>(m.offset_term_ratio[k]) * fair;
    }
    if (previous == value) return false;

    const uint32_t begin = m.offset_path_begin[offset];
    const uint32_t end = m.offset_path_begin[offset + 1];
    if (value == NULL_PRICE) {
        for (uint32_t i = begin; i < end; ++i) {
            path& p = paths[m.offset_path_slot[i]];
            p.pq.reset();
            p.resort = true;
        }
    } else if (previous == NULL_PRICE) {
        for (uint32_t i = begin; i < end; ++i) paths[m.offset_path_slot[i]].recalc = true;
    } else {
        const price_t diff = value - previous;
        for (uint32_t i = begin; i < end; ++i) {
            path& p = paths[m.offset_path_slot[i]];
            if (p.pq.valid()) {
                p.pq.price += diff;
                p.resort = true;
            }
        }
    }
    return true;
}

}  // namespace tachyon::trade::as::oracle
