#pragma once

// Fair-value valuation of the RV model: contract midpoints in, short-term fair values out.
//
// This is the whole numeric pipeline of `RelativeValue.h` stages 1-6 (mid -> ODM -> factor
// prices -> long-term fair/EMA -> per-vector short-term fair -> per-contract short-term fair),
// behind one object so the stage order and the scratch buffers live in one place instead of
// being a set of free functions that each take the model and the state.
//
// Stateless apart from the model reference: holding it does not allocate, and every method is
// a pure function of (model, state at entry, cycle counter) plus the EMA's own state.

#include <cstdint>

#include "model.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::rv {

class valuation {
public:
    explicit valuation(const model& m) noexcept : m_(m) {}

    // Midpoint of one contract (legacy calculateMidpoint / calculateWeightedMidpoint).
    // Reads the *external* books, as legacy does. Returned in dollars.
    double mid(const cycle_state& s, uint32_t contract) const noexcept;

    // Midpoint + store into the state (legacy updateMidPoint, :1921).
    void update_mid(cycle_state& s, uint32_t contract) const noexcept {
        s.mid[contract] = mid(s, contract);
    }

    // Refresh mids for every contract the cycle marked dirty and that takes part in pricing
    // (legacy updates every contract with a non-zero ODM weight, spreads included, :1875-1881).
    void refresh_dirty_mids(cycle_state& s) const noexcept;

    // The dense pipeline. One mid change can reach every contract through the ODM, so this
    // is a full recompute, not an incremental one (PLAN §4.3).
    void recompute(cycle_state& s, uint64_t now_cycles) const noexcept;

private:
    const model& m_;
};

}  // namespace tachyon::trade::rv
