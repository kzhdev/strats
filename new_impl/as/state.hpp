#pragma once

// The runtime state of one AS engine instance: the two books per contract per side, which of
// them is best, the path runtime (price/size/flags) with its per-(contract, side) order, the
// quotes last sent, the short-term fairs and their offsets, and the dirty set that tells the
// cycle which contracts to revisit.
//
// It is a *record* reached through inline accessors, so the hot path pays nothing for the
// encapsulation while the invariants it does own -- book updates, best-book selection, dirty
// marking, order reset -- cannot be bypassed from outside.
//
// state.cpp holds the parts with branches (book update, best-book selection); everything the
// cycle calls per contract is inline here.

#include <array>
#include <cstdint>

#include "limits.hpp"
#include "model.hpp"
#include "policy.hpp"
#include "types.hpp"

namespace tachyon::trade::as {

class cycle_state {
public:
    cycle_state() noexcept = default;
    explicit cycle_state(const model& m) noexcept { init(m); }

    // Fresh session: empty books, internal book best, paths reset, per-(contract, side) order
    // back to the model's definition order, no quotes, no dirty contracts, no short-term fairs.
    void init(const model& m) noexcept;

    // ---- books -----------------------------------------------------------------
    market_state& book(uint32_t contract, side sd, bool external) noexcept {
        return books_[market_index(contract, sd, external)];
    }
    const market_state& book(uint32_t contract, side sd, bool external) const noexcept {
        return books_[market_index(contract, sd, external)];
    }

    // legacy Market::update for an external book (Common.h:663-686): true when the price or the
    // size moved. A *price* change first runs the policy's price_changed hook -- legacy drops
    // that book's FAK adjustments and its AS quote adjustment there -- before the level lands.
    bool update_external(const quote_policy& policy, uint32_t contract, side sd, price_t price,
                         qty_t size) noexcept;

    // legacy Market::updateInternalMarket (Common.h:688-695): no flush, no invalidation. The
    // caller treats an internal update as a change unconditionally (ArbieSpreader.h:288).
    void update_internal(uint32_t contract, side sd, price_t price, qty_t size) noexcept {
        market_state& b = book(contract, sd, false);
        b.price = price;
        b.size = size;
    }

    // ---- best book -------------------------------------------------------------
    // legacy Contract::updateBestMarket<Side> (:313-341). `updated_external` names the book that
    // moved. The return value is legacy's `changed` predicate -- the updated book is the new
    // best **or** it was the old best -- which is what decides whether paths receive a delta,
    // not merely "the best changed".
    bool refresh_best(const model& m, const quote_policy& policy, uint32_t contract, side sd,
                      bool updated_external) noexcept;

    bool best_is_external(uint32_t contract, side sd) const noexcept {
        return best_external_[key(contract, sd)] != 0;
    }
    const market_state& best_book(uint32_t contract, side sd) const noexcept {
        return book(contract, sd, best_is_external(contract, sd));
    }
    const market_state& other_side_best(uint32_t contract, side sd) const noexcept {
        return best_book(contract, other(sd));
    }

    // legacy getExtCommissionAdjustedBestPrice<Side> (:270-275): an external best price carries
    // the contract's external commission, an internal one does not.
    price_t best_price_adjusted(const model& m, uint32_t contract, side sd) const noexcept {
        const bool external = best_is_external(contract, sd);
        const price_t price = book(contract, sd, external).price;
        if (external && price != NULL_PRICE) {
            return apply_price_offset(sd, price, -m.ext_commission[contract]);
        }
        return price;
    }

    // Displayed size of one book after reservations (legacy Market::getAdjustedSize).
    qty_t adjusted_size(const quote_policy& policy, uint32_t contract, side sd, bool external,
                        size_filter filter = all_reservations) const noexcept {
        if (!policy.has_ledger()) return book(contract, sd, external).size;
        return policy.size(contract, sd, external, filter);
    }

    // ---- path runtime ----------------------------------------------------------
    path_state& path(uint32_t slot) noexcept { return paths_[slot]; }
    const path_state& path(uint32_t slot) const noexcept { return paths_[slot]; }

    // Per-(contract, side) path order: rank 0 is the best path. Storage is the model's
    // side_path range, so the order is a permutation inside a fixed buffer.
    uint32_t order_size(const model& m, uint32_t contract, side sd) const noexcept {
        return m.path_count(contract, sd);
    }
    uint32_t order_at(const model& m, uint32_t contract, side sd, uint32_t rank) const noexcept {
        return order_[m.side_path_begin[key(contract, sd)] + rank];
    }
    uint32_t& order_slot(const model& m, uint32_t contract, side sd, uint32_t rank) noexcept {
        return order_[m.side_path_begin[key(contract, sd)] + rank];
    }
    // Order buffer for one (contract, side): rank 0 first (the model's side_path range).
    uint32_t* order_data(const model& m, uint32_t contract, side sd) noexcept {
        return &order_[m.side_path_begin[key(contract, sd)]];
    }
    // Restore the model's definition order (legacy `_paths[Side]` starts in config order).
    void reset_order(const model& m, uint32_t contract, side sd) noexcept {
        const uint32_t k = key(contract, sd);
        for (uint32_t i = m.side_path_begin[k]; i < m.side_path_begin[k + 1]; ++i) {
            order_[i] = m.side_path_slot[i];
        }
    }

    // ---- quotes ----------------------------------------------------------------
    quote& quote_of(uint32_t contract, side sd) noexcept {
        return quotes_[contract * SIDE_COUNT + to_index(sd)];
    }
    const quote& quote_of(uint32_t contract, side sd) const noexcept {
        return quotes_[contract * SIDE_COUNT + to_index(sd)];
    }
    quote& last_sent(uint32_t contract, side sd) noexcept {
        return last_sent_[contract * SIDE_COUNT + to_index(sd)];
    }
    const quote& last_sent(uint32_t contract, side sd) const noexcept {
        return last_sent_[contract * SIDE_COUNT + to_index(sd)];
    }
    bool force_update(uint32_t contract, side sd) const noexcept {
        return force_[contract * SIDE_COUNT + to_index(sd)] != 0;
    }
    void set_force_update(uint32_t contract, side sd, bool on) noexcept {
        force_[contract * SIDE_COUNT + to_index(sd)] = on ? 1 : 0;
    }

    // ---- short-term fairs ------------------------------------------------------
    // Short-term fair per contract (price units). NULL_PRICE means "not received yet", which
    // legacy distinguishes from a real value in the offset calculation (:199-243).
    price_t stf(uint32_t contract) const noexcept { return stf_[contract]; }
    bool has_stf(uint32_t contract) const noexcept { return stf_[contract] != NULL_PRICE; }
    void set_stf(uint32_t contract, price_t fair) noexcept { stf_[contract] = fair; }

    price_t offset_value(uint32_t offset) const noexcept { return offset_value_[offset]; }
    bool has_offset_value(uint32_t offset) const noexcept {
        return offset_value_[offset] != NULL_PRICE;
    }
    void set_offset_value(uint32_t offset, price_t value) noexcept {
        offset_value_[offset] = value;
    }

    // ---- per-book facts the calculation reads -----------------------------------
    // legacy Contract::_lastPriceInPathCalculation[side] (:195): the lean price a path was last
    // priced at. canHedge vetoes a path whose lean has since moved *better* (:35-48), so this
    // is a staleness guard, not bookkeeping.
    price_t last_path_price(uint32_t contract, side sd) const noexcept {
        return last_path_price_[contract * SIDE_COUNT + to_index(sd)];
    }
    void set_last_path_price(uint32_t contract, side sd, price_t price) noexcept {
        last_path_price_[contract * SIDE_COUNT + to_index(sd)] = price;
    }

    // Which contracts' internal books are RV's own quote market. Legacy compares the market id
    // with MarketIds::Market_ID_RV_QUOTE (ArbieSpreader.h:755); the new engine carries the same
    // fact as a flag the host sets when it publishes the internal book.
    bool internal_is_rv_quote(uint32_t contract, side sd) const noexcept {
        return rv_quote_[contract * SIDE_COUNT + to_index(sd)] != 0;
    }
    void set_internal_is_rv_quote(uint32_t contract, side sd, bool on) noexcept {
        rv_quote_[contract * SIDE_COUNT + to_index(sd)] = on ? 1 : 0;
    }

    // ---- flags -----------------------------------------------------------------
    bool can_trade(uint32_t contract) const noexcept { return can_trade_[contract] != 0; }
    void set_can_trade(uint32_t contract, bool on) noexcept { can_trade_[contract] = on ? 1 : 0; }

    // ---- dirty tracking (bitset + packed list, first-touch order) ----------------
    void mark_dirty(uint32_t contract) noexcept {
        const uint64_t bit = uint64_t(1) << (contract & 63u);
        uint64_t& word = dirty_[contract >> 6u];
        if ((word & bit) == 0) {
            word |= bit;
            dirty_list_[dirty_count_++] = contract;
        }
    }
    bool is_dirty(uint32_t contract) const noexcept {
        return (dirty_[contract >> 6u] & (uint64_t(1) << (contract & 63u))) != 0;
    }
    uint32_t dirty_count() const noexcept { return dirty_count_; }
    uint32_t dirty_at(uint32_t index) const noexcept { return dirty_list_[index]; }
    void clear_dirty() noexcept {
        for (uint32_t i = 0; i < dirty_count_; ++i) dirty_[dirty_list_[i] >> 6u] = 0;
        dirty_count_ = 0;
    }

private:
    static constexpr uint32_t key(uint32_t contract, side sd) noexcept {
        return contract * SIDE_COUNT + to_index(sd);
    }

    std::array<market_state, MAX_CONTRACTS * SIDE_COUNT * 2> books_{};
    std::array<uint8_t, MAX_CONTRACTS * SIDE_COUNT> best_external_{};
    std::array<path_state, MAX_PATH_SLOTS> paths_{};
    std::array<uint32_t, MAX_PATH_SLOTS> order_{};
    std::array<quote, MAX_CONTRACTS * SIDE_COUNT> quotes_{};
    std::array<quote, MAX_CONTRACTS * SIDE_COUNT> last_sent_{};
    std::array<uint8_t, MAX_CONTRACTS * SIDE_COUNT> force_{};
    std::array<price_t, MAX_CONTRACTS> stf_{};
    std::array<price_t, MAX_CONTRACTS * SIDE_COUNT> last_path_price_{};
    std::array<uint8_t, MAX_CONTRACTS * SIDE_COUNT> rv_quote_{};
    std::array<price_t, MAX_OFFSETS> offset_value_{};
    std::array<uint8_t, MAX_CONTRACTS> can_trade_{};
    std::array<uint64_t, CONTRACT_WORDS> dirty_{};
    std::array<uint32_t, MAX_DIRTY> dirty_list_{};
    uint32_t dirty_count_ = 0;
};

}  // namespace tachyon::trade::as
