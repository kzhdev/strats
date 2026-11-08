#pragma once

#include <common/liquidity_ledger.hpp>

#include <cstdint>
#include <vector>

namespace tachyon::trade {

class market {
public:
    void init(strategy_id strat_id,
              liquidity_source source,
              market_key key,
              liquidity_ledger& ledger) noexcept;

    // Internal markets change source as the IME publishes a different best
    // resting order. External markets always use LIQUIDITY_SOURCE_EXTERNAL.
    void set_source(liquidity_source source) noexcept { source_ = source; }

    // RV external-market update.
    bool update(price_t price, qty_t size);

    // AS external-market update. Affected markets are returned as keys; the
    // strategy maps them back to its market objects and recalculates paths.
    bool update(price_t price,
                qty_t size,
                std::vector<market_key>& affected_markets);

    // Internal market-data update. This does not invalidate reservations:
    // changing the displayed internal source is not a price-level replacement.
    bool update_internal(price_t price, qty_t size);

    price_t price() const noexcept { return price_; }

    qty_t unadjusted_size() const noexcept { return size_; }

    qty_t adjusted_size(reservation_filter filter = {}) const noexcept;

    market_key key() const noexcept { return key_; }

    liquidity_source source() const noexcept { return source_; }

    strategy_id strategy() const noexcept { return strat_id_; }

private:
    strategy_id strat_id_ = strategy_id::__COUNT__;
    liquidity_source source_ = liquidity_source::__LIQUIDITY_SOURCE_END__;

    price_t price_ = NULL_PRICE;
    qty_t size_ = 0;

    market_key key_ = 0;
    liquidity_ledger* ledger_ = nullptr;
};

template<order_side side>
inline market& BetterMarket(market& lhs, market& rhs) {
    static_assert(side == order_side::BUY || side == order_side::SELL,
                  "bad side");
    if (lhs.price() == NULL_PRICE) {
        return rhs;
    }
    if (rhs.price() == NULL_PRICE) {
        return lhs;
    }
    if constexpr (side == order_side::BUY) {
        return lhs.price() >= rhs.price() ? lhs : rhs;
    } else {
        return lhs.price() <= rhs.price() ? lhs : rhs;
    }
}

}   // end of namespace tachyon::trade
