#include "strategy.hpp"

namespace tachyon::trade::rv {

void strategy::on_market_data(uint32_t contract, side sd, bool external, price_t price,
                              qty_t size) noexcept {
    market_state& mkt = s_.market(contract, sd, external);
    if (mkt.price == price && mkt.size == size) {
        return;  // no-op update: nothing to recompute
    }
    // A replaced external price level drops the reservations taken against it, exactly as
    // legacy Market::update flushes its FAK adjustments (Common.h:636-644). Internal markets
    // do not flush (updateInternalMarket, :673-679).
    if (external && mkt.price != price && policy_.price_changed != nullptr) {
        policy_.price_changed(policy_.price_changed_ctx, contract, sd, external);
    }
    mkt.price = price;
    mkt.size = size;
    s_.mark_dirty(contract);
}

void strategy::on_fill(uint32_t contract, side sd, qty_t qty) noexcept {
    if (!enabled_) {
        return;
    }
    hedger_.on_fill(contract, sd, qty);
}

void strategy::run_cycle(uint64_t now_cycles) noexcept {
    if (!enabled_) {
        return;
    }
    // 1. midpoints, dirty contracts only — legacy updates every contract that is used in
    //    pricing (weight != 0), including spreads (:1875-1881).
    valuation_.refresh_dirty_mids(s_);

    // 2. dense pricing pipeline (full recompute: one mid change reaches everything)
    valuation_.recompute(s_, now_cycles);

    // 3. quotes
    quotes_.build_all(s_);

    // 4. at most one hedge contract per cycle (S10)
    if (hedger_.has_request()) {
        const bool must_disable = hedger_.run(s_);
        if (must_disable) {
            disable();
        }
        // positions moved: refresh fair values and quotes (legacy :572)
        valuation_.recompute(s_, now_cycles);
        quotes_.build_all(s_);
    }

    s_.clear_dirty();
}

}  // namespace tachyon::trade::rv
