#pragma once

// Hooks from the engine into the host (the legacy `Market` adjustments and the order path).
//
// Everything is optional — a null pointer means "neutral behaviour" — so the engine runs
// standalone in tests with raw displayed books, exactly like `rv::quote_policy`. The host wires
// these onto `common::liquidity_ledger` (PLAN D9).
//
// Ownership: the policy is a *seam*, not state. The engine never stores it beyond a call, so a
// host can swap its context between cycles without touching engine state.

#include <cstdint>

#include "types.hpp"

namespace tachyon::trade::as {

// Reservation kinds the size query can ignore, mirroring `common::reservation_type`. Only the
// mask is needed here; the ledger owns the reservations themselves.
enum class reservation_kind : uint8_t {
    hedge = 1 << 0,
    aggressive_quote = 1 << 1,
    anticipated_hedge = 1 << 2,
};

// Which reservations a size query ignores. Legacy AS needs two views of the same book:
//   * everything — `Market::getAdjustedSize()` (`ArbieSpreader.h:315-320`), and
//   * everything except its own anticipated-hedge pre-adjustments —
//     `Market::getAdjustedSizeExcludeFAKPreAdj()` (`:897`), used when a lean's size is zero
//     *because AS reserved it* and the path should not be dropped for that reason alone.
struct size_filter {
    uint8_t exclude_mask = 0;

    friend constexpr bool operator==(const size_filter&, const size_filter&) noexcept = default;
};

inline constexpr size_filter all_reservations{};
inline constexpr size_filter exclude_anticipated_hedge{
    static_cast<uint8_t>(reservation_kind::anticipated_hedge)};

struct quote_policy {
    // Displayed size after reservations (legacy `Market::getAdjustedSize`). A null callback
    // means "no ledger": the engine then uses the raw displayed size.
    qty_t (*adjusted_size)(void* ctx, uint32_t contract, side sd, bool external,
                           size_filter filter) noexcept = nullptr;
    void* adjusted_ctx = nullptr;

    // A displayed price level was replaced. Legacy `Market::update` drops that book's FAK
    // adjustments and its AS quote adjustment, which releases liquidity on *other* books
    // (`Common.h:634-646`). The host performs the release and marks whatever it touched dirty.
    void (*price_changed)(void* ctx, uint32_t contract, side sd, bool external,
                          qty_t new_size) noexcept = nullptr;
    void* price_changed_ctx = nullptr;

    // legacy Market::getASQuoteAdjustment() (:813-815): how much AS itself reserved on this book
    // through an aggressive quote. Legacy uses the *amount* (not just its presence) when a fill
    // releases it, so the seam exposes the quantity.
    qty_t (*as_quote_adjustment)(void* ctx, uint32_t contract, side sd, bool external) noexcept =
        nullptr;
    void* as_quote_ctx = nullptr;

    // legacy Market::undoAsQuoteAdjustment(): release up to `qty` (0 == everything) of what an
    // aggressive quote reserved, returning true when something was released.
    bool (*undo_as_quote)(void* ctx, uint32_t contract, side sd, bool external,
                          qty_t qty) noexcept = nullptr;
    void* undo_ctx = nullptr;

    qty_t size(uint32_t contract, side sd, bool external,
               size_filter filter = all_reservations) const noexcept {
        if (adjusted_size == nullptr) return -1;  // "no ledger": caller falls back to the book
        return adjusted_size(adjusted_ctx, contract, sd, external, filter);
    }

    bool has_ledger() const noexcept { return adjusted_size != nullptr; }

    // Without a ledger nothing is reserved, so no size can have been caused by an AS quote.
    qty_t as_quote(uint32_t contract, side sd, bool external) const noexcept {
        return as_quote_adjustment == nullptr
                   ? 0
                   : as_quote_adjustment(as_quote_ctx, contract, sd, external);
    }

    bool aggressive_quote(uint32_t contract, side sd, bool external) const noexcept {
        return as_quote(contract, sd, external) > 0;
    }

    bool undo_aggressive_quote(uint32_t contract, side sd, bool external,
                               qty_t qty) const noexcept {
        return undo_as_quote != nullptr && undo_as_quote(undo_ctx, contract, sd, external, qty);
    }

    void on_price_changed(uint32_t contract, side sd, bool external, qty_t new_size) const noexcept {
        if (price_changed != nullptr) price_changed(price_changed_ctx, contract, sd, external, new_size);
    }
};

}  // namespace tachyon::trade::as
