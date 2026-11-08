#include "market.hpp"

#include <cassert>

using namespace tachyon;
using namespace tachyon::trade;

namespace tachyon::trade {

void market::init(strategy_id strat_id,
                  liquidity_source source,
                  market_key key,
                  liquidity_ledger& ledger) noexcept {
    assert(strat_id != strategy_id::__COUNT__);
    strat_id_ = strat_id;
    source_ = source;
    key_ = key;
    ledger_ = &ledger;
}

bool market::update(price_t price, qty_t size) {
    assert(source_ == liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);
    assert(strat_id_ == strategy_id::RV);

    const bool changed = price_ != price || size_ != size;
    if (price_ != price && ledger_ != nullptr) {
        ledger_->invalidate_market(key_);
    }

    price_ = price;
    size_ = size;
    return changed;
}

bool market::update(price_t price,
                    qty_t size,
                    std::vector<market_key>& affected_markets) {
    assert(source_ == liquidity_source::LIQUIDITY_SOURCE_EXTERNAL);

    const bool changed = price_ != price || size_ != size;
    if (price_ != price && ledger_ != nullptr) {
        ledger_->invalidate_market(key_, &affected_markets);
    }

    price_ = price;
    size_ = size;
    return changed;
}

bool market::update_internal(price_t price, qty_t size) {
    const bool changed = price_ != price || size_ != size;
    price_ = price;
    size_ = size;
    return changed;
}

qty_t market::adjusted_size(reservation_filter filter) const noexcept {
    if (source_ == liquidity_source::__LIQUIDITY_SOURCE_END__ ||
        ledger_ == nullptr || size_ == 0) {
        return 0;
    }

    // Only hedge reservations belonging to the source currently supplying the
    // displayed size are relevant. Quote-group reservations are market-wide.
    filter.source = source_;
    return ledger_->available(key_, size_, filter);
}
}   // end of namespace tachyon::trade
