#pragma once

#include <algorithm>
#include <cstdint>

#include "model.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// Legacy FillType (Common.h:963) and FillSource (Common.h:965-979), reduced to what the strategy
// branches on. The adapter maps the Viper enums onto these.
enum class fill_type : uint8_t { internal, external, phantom, negative_scalp_check };

enum class fill_source : uint8_t {
    quote,
    missed_hedge,
    overfill,
    hedge,
    self_match,
    rv_disable,
    rv_hedge_reject,
    viper_off,
    viper_start,
    manual,
    other,
};

// One inbound execution (legacy FillMessage, Common.h:1557-1576), plus the two flags the strategy
// reads: whether the fill came from another strategy, and whether it must be force-taken.
struct fill {
    side sd = side::bid;
    price_t price = 0;
    qty_t qty = 0;
    fill_type type = fill_type::external;
    fill_source source = fill_source::quote;
    bool from_other_strategy = false;  // legacy fill._strategy != StrategyId::MarketMaker
    bool force_over_fill = false;      // legacy fill._forceOverFill
    bool check_limit = false;          // legacy fill._checkLimit
};

// What the book did with a fill: the front-end allocation split, and the two decisions the host
// has to act on.
struct fill_outcome {
    qty_t allocated_internal = 0;
    qty_t allocated_external = 0;
    // Legacy sends `sizeTaken` for a takeover and the fill size otherwise (:292, :1056).
    qty_t reported_size = 0;
    // Legacy forceTakeOverFill(..., checkLimit) and the untaken-overfill path disable external
    // quoting (:309-317, :326-329).
    bool disable_external_quoting = false;
    // The MM could not (or must not) take the whole overfill: legacy hands it to RV/AS, and takes
    // it itself and disables external quoting when none is enabled. The host plays that role.
    qty_t untaken_remaining = 0;
    // A fill from another strategy is followed by an onEndOfFills in the same call (:1000).
    bool ends_cycle_inline = false;
};

// Position bookkeeping and the fill state machine: legacy onFill (:1013-1056), takeOverFill
// (:238-317), forceTakeOverFill (:318-340) and isWithinPositionLimits (:1211-1225).
//
// The two positions are deliberately separate: the internal one is what our quotes did to us and
// the external one is what the market did through our quotes, and the hedge is sized on their sum.
class position_book {
public:
    explicit position_book(const model& m) noexcept : m_(m) {}

    fill_outcome on_fill(contract_state& s, uint32_t c, const fill& f) const noexcept;
    fill_outcome take_over_fill(contract_state& s, uint32_t c, const fill& f) const noexcept;
    void force_take_over_fill(contract_state& s, uint32_t c, const fill& f,
                              fill_outcome& out) const noexcept;
    bool within_limits(const contract_state& s, uint32_t c) const noexcept;

    // ---- helpers, exposed for the tests ----
    // Legacy tryOffsetPosition (:26-48): offset the fill against an opposite-signed position.
    static qty_t try_offset(qty_t& position, qty_t remaining, qty_t& allocated, side sd) noexcept;
    // Legacy offsetPosition (:991-1000): internal first, then external.
    static qty_t offset_position(contract_state& s, side sd, qty_t remaining, qty_t& alloc_internal,
                                 qty_t& alloc_external) noexcept;
    // Legacy allocateExternalQuoteFill (:1058-1072).
    static void allocate_external_quote_fill(contract_state& s, quote_state& quote, qty_t size,
                                             side sd) noexcept;
    // Legacy allocateHedgingFill (:972-990) with doAllocateHedgingFill (:957-970).
    static qty_t allocate_hedging_fill(contract_state& s, side sd, qty_t size,
                                       qty_t& alloc_internal, qty_t& alloc_external) noexcept;
    // Legacy allocateHedgingFillRemainder (:944-955).
    static void allocate_hedging_fill_remainder(contract_state& s, side sd, bool internal,
                                                qty_t remaining, qty_t& alloc_internal,
                                                qty_t& alloc_external) noexcept;

private:
    const model& m_;
};

}  // namespace tachyon::trade::mm
