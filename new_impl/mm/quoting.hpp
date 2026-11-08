#pragma once

#include <cstdint>

#include "book.hpp"
#include "intents.hpp"
#include "model.hpp"
#include "order_queue.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::mm {

// The pricing half of the cycle: the established-level EMA, the midpoint, the width, and the
// quote price/size computation of updateQuote (SimpleMarketMaker.h:661-739) including the
// queue-position walk of calculateQuotePriceAndSizeByOrders (:756-864).
//
// Stateful only in the sense that it holds the immutable model; every method takes the
// contract state (and, where the queue matters, the order book) it works on, so the maths stays
// a pure function of (model, state) and can be exercised without a host, transport or clock.
class quote_engine {
public:
    explicit quote_engine(const model& m) noexcept : m_(m) {}

    struct price_and_size {
        price_t internal_price = 0;
        price_t external_price = 0;
        qty_t size = 0;
    };

    // Internal and external price for a given queue position (legacy's calculateInternalPrice
    // lambda, :779-788).
    struct scaled_price {
        price_t internal_price = 0;
        price_t external_price = 0;
    };

    // ---- pricing block of onMarketDataChange (:436-446) ----
    // Called only when the top book changed, which is what legacy gates the whole block on.
    void refresh_market(contract_state& s, uint32_t c, uint64_t now_cycles) const noexcept;

    // Legacy calculateMidPoint (:911-919): the size-weighted mid when the spread is exactly
    // one tick, otherwise the plain mid.
    price_t mid_point(const contract_state& s, uint32_t c) const noexcept;

    // Legacy calculateEstablishedLevel (:921-942).
    void update_established_level(contract_state& s, uint32_t c, double tob_qty,
                                  uint64_t now_cycles) const noexcept;

    // ---- quote construction ----
    // Legacy updateQuotesBySide (:603-628): decide whether this side's quote must be
    // recalculated at all, and recalculate it if so.
    bool update_side(contract_state& s, const order_queue& q, uint32_t c, side sd,
                     bool top_book_changed, bool quote_externally, bool force_update = false,
                     intent_log* log = nullptr) const noexcept;

    // Legacy updateQuote (:661-739). Returns true when the quote changed and a request must be
    // emitted; the "last sent" bookkeeping is updated at that point, exactly where legacy's
    // modifyQuoteOrder updates it.
    bool build_quote(contract_state& s, const order_queue& q, uint32_t c, side sd,
                     bool quote_externally, bool force_update,
                     intent_log* log = nullptr) const noexcept;

    // Legacy modifyQuoteOrder (:878-891): record the intent and remember what was sent.
    void emit_quote(intent_log& log, uint32_t c, side sd, quote_state& quote,
                    bool persist) const noexcept;

    // Legacy checkQuoteCross (:512-537): after one side's quote changed, force the other side out
    // if it would now cross.
    void cross_quote(contract_state& s, intent_log& log, uint32_t c, bool quote_externally,
                     bool& bid_changed, bool& ask_changed) const noexcept;

    // Legacy calculateQuotePriceAndSizeByOrders (:756-864): the bypass paths plus the
    // queue-position walk over our resting orders at the target price.
    price_and_size price_of(const contract_state& s, const order_queue& q, uint32_t c, side sd,
                            price_t price, bool quote_externally) const noexcept;

    // Legacy calculateInternalPrice (:779-788): the S-curve that turns a queue position into a
    // price offset. `inv_established` is 1/establishedLevel, hoisted out of the per-order loop
    // (see MM_HOIST_EST_DIV in CMakeLists and PARITY_NOTES §4).
    scaled_price price_at_queue_position(const contract_state& s, uint32_t c, side sd,
                                         price_t price, qty_t queue_position,
                                         double inv_established) const noexcept;

    // ---- size helpers, exposed for the oracle and the tests ----
    // Legacy getMaxQuoteSizeBasedOnMarket (:105-122).
    qty_t max_quote_size_based_on_market(uint32_t c, side sd, const level& book, double pct,
                                         price_t price) const noexcept;
    // Legacy getInternalQuoteSize (:1748-1766) / getExternalQuoteSize (:1768-1790).
    qty_t internal_quote_size(const contract_state& s, uint32_t c, side sd,
                              qty_t max_size) const noexcept;
    qty_t external_quote_size(const contract_state& s, uint32_t c, side sd, qty_t quote_size,
                              qty_t max_size) const noexcept;
    // Legacy calculateCandidateExternalQuoteSize (:741-753).
    qty_t candidate_external_size(const contract_state& s, uint32_t c, side sd, qty_t size,
                                  qty_t max_size) const noexcept;
    // Legacy getSprayQty (:866-872).
    qty_t spray_quantity(const contract_state& s, uint32_t c, side sd, qty_t current_qty,
                         const level& book, price_t external_price) const noexcept;
    // Legacy isThereEnoughSize (:130-141): the join/leave hysteresis.
    static bool enough_size(side sd, const level& book, qty_t threshold, price_t price) noexcept;
    // Legacy isInPayUp (:356-361).
    bool in_payup(uint32_t c, qty_t our_size, const level& book) const noexcept;
    // Legacy calculateWorsenPrice (:76-87) and calculatePayUpInternalPrice (:89-100).
    static price_t worsen_price(side sd, price_t price, price_t amount) noexcept;
    static price_t payup_internal_price(side sd, price_t ordinary, price_t worsened) noexcept;

private:
    const model& m_;
};

}  // namespace tachyon::trade::mm
