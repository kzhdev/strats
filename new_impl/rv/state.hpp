#pragma once

// The runtime state of one RV engine instance: the two market books per contract per side,
// the fair-value pipeline's values, the quotes, the positions, and the dirty set that tells
// the cycle which midpoints to refresh.
//
// It is a *record*: the data members are public because the hot path reads and writes them
// directly (no getter chains, no virtuals, no allocation). The behaviour it does own --
// book selection, dirty tracking, initialisation -- is expressed as methods rather than as
// free functions taking a `cycle_state&`, so the state cannot be mutated through a path that
// bypasses its invariants (e.g. marking a contract dirty twice, or re-initialising half of it).

#include <array>
#include <cstdint>

#include "model.hpp"
#include "types.hpp"

namespace tachyon::trade::rv {

class cycle_state {
public:
    cycle_state() noexcept = default;
    explicit cycle_state(const model& m) noexcept { init(m); }

    // Full reset to a freshly-started session: empty books, ltf from the model, no quotes,
    // no dirty contracts (legacy initializeFromRelativeValue + initializeContracts).
    void init(const model& m) noexcept;

    // ---- market books -------------------------------------------------------
    market_state& market(uint32_t contract, side sd, bool external) noexcept {
        return markets[market_index(contract, sd, external)];
    }
    const market_state& market(uint32_t contract, side sd, bool external) const noexcept {
        return markets[market_index(contract, sd, external)];
    }

    // Best of the internal and external book on one side (legacy BetterMarket<side>):
    // NULL_PRICE loses, otherwise the higher bid / lower ask wins.
    const market_state& best_market(uint32_t contract, side sd) const noexcept;
    // Whether that best book is the external one (decides `_externalPrice` and which
    // source the ledger reservation filter sees).
    bool best_is_external(uint32_t contract, side sd) const noexcept;

    // ---- quotes -------------------------------------------------------------
    quote& quote_of(uint32_t contract, side sd) noexcept {
        return quotes[contract * SIDE_COUNT + to_index(sd)];
    }
    const quote& quote_of(uint32_t contract, side sd) const noexcept {
        return quotes[contract * SIDE_COUNT + to_index(sd)];
    }

    // ---- dirty tracking (bitset + packed list, no hashing) -------------------
    // A contract is marked at most once per cycle; the list keeps first-touch order, which
    // is what makes the mid refresh deterministic.
    void mark_dirty(uint32_t contract) noexcept {
        const uint64_t bit = uint64_t(1) << (contract & 63u);
        uint64_t& word = dirty_[contract >> 6u];
        if ((word & bit) == 0) {
            word |= bit;
            changed_[changed_count_++] = contract;
        }
    }
    void clear_dirty() noexcept {
        for (uint32_t i = 0; i < changed_count_; ++i) {
            dirty_[changed_[i] >> 6u] = 0;
        }
        changed_count_ = 0;
    }
    uint32_t changed_count() const noexcept { return changed_count_; }
    uint32_t changed_at(uint32_t index) const noexcept { return changed_[index]; }

    // ---- fair values --------------------------------------------------------
    // Midpoints by contract (dollars), as fed to the ODM.
    std::array<double, MAX_CONTRACTS> mid{};
    // Short-term fair value by contract (price units): the number quotes are built from.
    std::array<price_t, MAX_CONTRACTS> stf{};

    // Pricing pipeline (dense stage outputs, kept for inspection and for the 2nd pass)
    std::array<double, MAX_PRICING> pricing{};     // ODM output
    std::array<double, MAX_VECTORS> factor{};      // factor prices
    std::array<double, MAX_VECTORS> prior_factor{};  // previous cycle's factor prices
    std::array<double, MAX_VECTORS> ltf{};         // long-term fair (EMA)
    std::array<double, MAX_VECTORS> stf_vec{};     // short-term fair per vector
    std::array<double, MAX_VECTORS> pos_vec{};     // vector exposure from positions
    std::array<double, MAX_VECTORS> scratch{};     // scaled pricing vector
    std::array<double, MAX_PRICING> pricing_stf{}; // risk * stf_vec

    // Market books: ((contract * 2 + side) * 2) + external
    std::array<market_state, MAX_CONTRACTS * SIDE_COUNT * 2> markets{};
    // Quotes: contract * 2 + side
    std::array<quote, MAX_CONTRACTS * SIDE_COUNT> quotes{};

    // Signed positions by contract (used by residuals and the exposure update)
    std::array<double, MAX_CONTRACTS> position{};

    // Cycle counter of the previous EMA step (the EMA advances by elapsed cycles)
    uint64_t last_ema_time = 0;

private:
    std::array<uint64_t, (MAX_CONTRACTS + 63) / 64> dirty_{};
    std::array<uint32_t, MAX_CHANGED> changed_{};
    uint32_t changed_count_ = 0;
};

}  // namespace tachyon::trade::rv
