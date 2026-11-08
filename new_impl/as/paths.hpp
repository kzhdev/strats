#pragma once

// The path engine: what a path is worth right now, and which path is the best one.
//
// A path is a signed basket of lean contracts (`Σ multiplier · leanPrice`) plus the basket's
// short-term-fair offset, minus its commission. Its size is the smallest displayed size among
// the leans, capped by the quoted contract's configured quote size. Everything is integer
// arithmetic at 1e-9 (PLAN D2), so the calculation is exact and reproducible.
//
// This object owns no data: it is the behaviour of (model, cycle_state, quote_policy). That
// keeps the incremental update (P3) and the full recompute able to share one transcription of
// the legacy branches instead of drifting into two.
//
// `calculate` is defined in paths.cpp (templated on the side, instantiated for both) so this
// header stays small for translation units that only need the sort/hedge predicates.

#include <algorithm>
#include <cstdint>

#include "model.hpp"
#include "policy.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::as {

// Legacy writes the price it priced a lean at into `_lastPriceInPathCalculation[_side]`, where
// `_side` is the *path's* side (ArbieSpreader.h:792), while canHedge reads the slot of the
// *lean's* side (ArbieSpreader_Path.cpp:41-42). For a negative multiplier those differ, so the
// legacy guard inspects an unrelated slot -- and the array is never initialised at all
// (ArbieSpreader.h:195 has no initialiser, initMarkets does not touch it), so legacy reads
// indeterminate memory. The engine uses the lean's side and initialises to NULL_PRICE
// (deviation D-AS6). Set this to true to reproduce the legacy index for measurement.
inline constexpr bool kLegacyLastPriceSide = false;

// Re-sort with a stable insertion sort (default) or with the exact std::sort call the legacy code
// makes. PLAN D5: `as_paths` proves the two produce the identical ranking over 320 000 randomised
// comparisons, and `as_cycle_bench` measures the insertion sort at 16.4 ns against 22.8 ns for
// std::sort (one call), so the cheaper one ships. Set AS_INSERTION_SORT=0 to get the legacy call
// back for measurement.
#ifndef AS_INSERTION_SORT
#define AS_INSERTION_SORT 1
#endif

class path_engine {
public:
    path_engine(const model& m, const quote_policy& policy) noexcept : m_(m), policy_(policy) {}

    // ---- the calculation -------------------------------------------------------
    // legacy Path::calculate<Side> (:711-831). `force` recalculates a path whose `recalc` flag
    // is clear. Returns the legacy "path changed" predicate, which is what asks for a re-sort:
    // the price moved, or the size moved while it was below the configured quote size.
    template <side S>
    bool calculate(cycle_state& s, uint32_t slot, bool force) const noexcept;

    bool calculate(cycle_state& s, uint32_t slot, bool force) const noexcept {
        return m_.slot_side[slot] == to_index(side::bid) ? calculate<side::bid>(s, slot, force)
                                                        : calculate<side::ask>(s, slot, force);
    }

    // ---- the incremental update -------------------------------------------------
    // legacy Contract::updateAffectedPathsBestMarket<Side> (:1125-1142) and
    // Path::onBestMarketDataUpdate (:833-928): apply this lean's price move to every path that
    // leans on it, as a delta, and record which paths now need re-sorting or a full recalc.
    // `old_price` must be the commission-adjusted best price *before* the book update.
    template <side S>
    void on_best_market_update(cycle_state& s, uint32_t lean_contract, price_t old_price) const noexcept;

    void on_best_market_update(cycle_state& s, uint32_t lean_contract, side sd,
                               price_t old_price) const noexcept {
        if (sd == side::bid) {
            on_best_market_update<side::bid>(s, lean_contract, old_price);
        } else {
            on_best_market_update<side::ask>(s, lean_contract, old_price);
        }
    }

    // legacy ShortTermFairOffset::calculateShortTermFairOffset (:199-243): recompute an offset
    // from the per-contract fairs and propagate the change to the paths that share it -- a
    // price delta when the offset merely moved, a reset when it became unavailable, a recalc
    // when it became available.
    bool update_offset(cycle_state& s, uint32_t offset) const noexcept;

    // ---- the update pass -------------------------------------------------------
    // legacy Contract::updatePaths<Side> (:931-985): recalculate the flagged paths in the
    // current ranking order, then re-sort if anything asked for it.
    template <side S>
    void update_paths(cycle_state& s, uint32_t contract, bool force) const noexcept {
        if (!m_.is_quoting(contract)) return;
        bool sort = false;
        const uint32_t count = s.order_size(m_, contract, S);
        uint32_t* order = s.order_data(m_, contract, S);
        for (uint32_t rank = 0; rank < count; ++rank) {
            const uint32_t slot = order[rank];
            path_state& p = s.path(slot);
            if (p.recalc || force) {
                sort |= calculate<S>(s, slot, force) | p.resort;
            } else {
                sort |= p.resort;
            }
            p.resort = false;
            p.dirty = false;
        }
        if (sort) sort_side<S>(s, contract, order, count);
    }

    void update_paths(cycle_state& s, uint32_t contract, bool force) const noexcept {
        update_paths<side::bid>(s, contract, force);
        update_paths<side::ask>(s, contract, force);
    }

    // legacy Path::canHedge (ArbieSpreader_Path.cpp:35-48): every lean must still be quoted at
    // the price the path was priced from, must be tradeable, and must display at least `qty`.
    // A lean that has moved *better* since the calculation vetoes the hedge -- that is the
    // staleness guard, and it is why `last_path_price` exists.
    bool can_hedge(const cycle_state& s, uint32_t slot, qty_t qty) const noexcept;

    // legacy `_paths[side][0]` -- rank 0 after the last sort.
    uint32_t best_slot(const cycle_state& s, uint32_t contract, side sd) const noexcept {
        return s.order_size(m_, contract, sd) == 0 ? INVALID_INDEX
                                                   : s.order_at(m_, contract, sd, 0);
    }

    // legacy PathSorter<Side> (:108-130): a valid path beats an invalid one, then the better
    // price wins (higher for a bid, lower for an ask), then the larger size.
    static bool better(const path_state& a, const path_state& b, side sd) noexcept {
        const bool a_valid = a.price != NULL_PRICE;
        const bool b_valid = b.price != NULL_PRICE;
        if (a_valid != b_valid) return a_valid;
        if (a.price != b.price) {
            return sd == side::bid ? a.price > b.price : a.price < b.price;
        }
        return a.size > b.size;
    }

    template <side S>
    void sort_side(cycle_state& s, uint32_t contract, uint32_t* order,
                   uint32_t count) const noexcept {
        (void)contract;
#if AS_INSERTION_SORT
        for (uint32_t i = 1; i < count; ++i) {
            const uint32_t slot = order[i];
            uint32_t j = i;
            while (j > 0 && better(s.path(slot), s.path(order[j - 1]), S)) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = slot;
        }
#else
        // Exactly the legacy call: std::sort with an equivalent comparator yields the same
        // permutation, including among paths that compare equal (PLAN D5).
        std::sort(order, order + count,
                  [&s](uint32_t a, uint32_t b) { return better(s.path(a), s.path(b), S); });
#endif
    }

private:
    const model& m_;
    quote_policy policy_;
};

}  // namespace tachyon::trade::as
