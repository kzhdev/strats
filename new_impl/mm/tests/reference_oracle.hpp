#pragma once

// Reference oracle for the MarketMaker quoting decisions: a transcription of the legacy source
// (src/SimpleMarketMaker.h) that is deliberately *not* derived from the new engine.
//
// Two properties make it a real differential rather than a mirror:
//   * it runs at the legacy 1e-8 scale (Common.h:267 ToRhonePrice), while the engine runs at
//     1e-9 — so the comparison measures exactly the D2 truncation surface (PLAN.md §4), and
//   * it keeps the legacy control flow and member names (_midPoint, _quotes[Side], ...), so a
//     divergence in structure shows up as a failed decision rather than being shared silently
//     (the lesson from new_impl/rv/PARITY_NOTES §5).
//
// Phase 3 covers the pricing block and updateQuote; hedges and cross checks arrive in phase 5.

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

#include "types.hpp"

namespace tachyon::trade::mm::oracle {

using tachyon::trade::mm::price_t;
using tachyon::trade::mm::qty_t;
using tachyon::trade::mm::side;
using tachyon::trade::mm::to_index;

inline constexpr price_t kNullPrice = std::numeric_limits<price_t>::max();
inline constexpr int64_t LEGACY_SCALE = 100'000'000;  // hf::price_t / ToRhonePrice

// Common.h:267 at the legacy scale.
inline price_t to_rhone_price(double dollars) noexcept {
    return static_cast<price_t>(dollars * static_cast<double>(LEGACY_SCALE));
}

// Common.h:1196 RoundToTickSize, verbatim.
inline price_t round_to_tick(price_t price, price_t tick, side s) noexcept {
    const price_t remainder = price % tick;
    if (!remainder) return price;
    const price_t truncated = price - remainder;
    if (s == side::bid) return remainder > 0 ? truncated : truncated - tick;
    return remainder > 0 ? truncated + tick : truncated;
}

// One legacy book level (md::top_t::level_t, only what the quoting path reads).
struct top {
    price_t pr = kNullPrice;
    qty_t q = 0;
    uint32_t o = 0;
};

// One side of the legacy book, including the implied quantity the feed's order count does not
// cover (`ext.implied.impliedQtyNotTrackedByQueuePosition`, subtracted in :795).
struct book_side {
    static constexpr uint32_t LEVEL_NUM = 10;
    top l[LEVEL_NUM];
    qty_t implied[LEVEL_NUM] = {};
    side s = side::bid;

    // SimpleMarketMaker.h:407-422.
    int32_t getPriceLevelIndex(price_t price) const noexcept {
        const bool better = s == side::bid ? price > l[0].pr : price < l[0].pr;
        if (price == kNullPrice || better) return -1;
        for (uint32_t i = 0; i < LEVEL_NUM; ++i) {
            if (l[i].pr == kNullPrice) return -1;
            if (price == l[i].pr) return static_cast<int32_t>(i);
        }
        return -1;
    }

    qty_t targetSize(uint32_t index) const noexcept { return l[index].q - implied[index]; }

    void set(side sd, uint32_t depth, price_t base, qty_t step_ticks, qty_t qty, uint32_t orders) {
        s = sd;
        for (uint32_t i = 0; i < LEVEL_NUM; ++i) {
            if (i < depth) {
                const price_t offset = static_cast<price_t>(step_ticks) * static_cast<price_t>(i);
                l[i] = top{s == side::bid ? base - offset : base + offset, qty, orders};
            } else {
                l[i] = top{};
            }
            implied[i] = 0;
        }
    }
};

// One of our resting orders, as the legacy `_priceOrders[Side][price]` vector holds it.
struct legacy_order {
    uint32_t order_id = 0;
    price_t price = kNullPrice;
    qty_t qty = 0;
    qty_t qp = -1;  // NOT_SET
};

// One legacy SimpleMarketMaker::Quote (:159-174).
struct quote {
    price_t internal_price = kNullPrice;
    qty_t internal_size = 0;
    price_t external_price = kNullPrice;
    qty_t external_size = 0;
    price_t last_internal_price_sent = kNullPrice;
    qty_t last_external_size_sent = 0;
};

// A legacy SimpleMarketMaker at the legacy scale: the members the quoting path touches, and the
// functions transcribed from it. Everything is public because the gate reads the state the way
// the legacy GUI would.
class legacy_market_maker {
public:
    // ---- configuration (legacy initialize, :192-236, all at 1e-8) ----
    price_t _tickSize = 0;
    price_t _commission = 0;
    price_t _minInternalPriceChange = 0;
    price_t _worsenPayupConditionAmount = 0;
    qty_t _quoteSize = 0;
    qty_t _quoteGap = 0;  // legacy _quoteGap, used by the spray condition (:837)
    qty_t _joinSize = 0;
    qty_t _leaveSize = 0;
    qty_t _payUpQuantity = 0;
    uint32_t _payUpOrders = 0;
    double _payUpMultiple = 0.0;
    qty_t _maxInternallyInitiatedPosition = 0;
    qty_t _maxExternallyInitiatedPosition = 0;
    double _pctTobMaxQuote = 0.0;
    double _widthPercentage = 0.0;
    uint32_t _queueImportance = 0;
    qty_t _orderIncrementQty = 1;  // ConfigContract::toValidQty
    double _emaEstLevel = 0.0;
    uint64_t _estLevelEmaPeriod = 0;
    double _estLevelPctEma = 0.0;

    // ---- state ----
    price_t _midPoint = 0;
    price_t _width = 0;
    qty_t _establishedLevel = 1;
    double _lastTobQty = 0.0;
    uint64_t _lastEmaTime = 0;
    qty_t _internallyInitiatedPosition = 0;
    qty_t _externallyInitiatedPosition = 0;
    quote _quotes[2];

    qty_t toValidQty(qty_t qty) const noexcept {
        return _orderIncrementQty == 1 ? qty : qty - (qty % _orderIncrementQty);
    }

    // SimpleMarketMaker.h:1792-1799.
    price_t roundToTickSize(side s, price_t price) const noexcept {
        return s == side::bid ? round_to_tick(price - _commission, _tickSize, side::bid)
                              : round_to_tick(price + _commission, _tickSize, side::ask);
    }

    // :911-919.
    price_t calculateMidPoint(const book_side& bid_book, const book_side& ask_book) const noexcept {
        const top& bid = bid_book.l[0];
        const top& ask = ask_book.l[0];
        if (bid.pr + _tickSize == ask.pr) {
            const qty_t askSize = std::min(ask.q, _establishedLevel);
            const qty_t bidSize = std::min(bid.q, _establishedLevel);
            if (bidSize + askSize != 0) {
                return (ask.pr * bidSize + bid.pr * askSize) / (bidSize + askSize);
            }
        }
        return (bid.pr + ask.pr) / 2;
    }

    // :921-942.
    void calculateEstablishedLevel(double qty, uint64_t now) noexcept {
        if (_lastEmaTime && (_estLevelEmaPeriod > 0)) {
            const uint64_t timeInCycles = now - _lastEmaTime;
            const double alpha = static_cast<double>(timeInCycles) / _estLevelEmaPeriod;
            const double mu = std::exp(-alpha);
            const double nu = (1 - mu) / alpha;
            const double lastEma = _emaEstLevel;
            const double newEma = mu * lastEma + (nu - mu) * _lastTobQty + (1 - nu) * qty;
            if (std::isfinite(newEma)) {
                _emaEstLevel = newEma;
                _establishedLevel = std::max<qty_t>(_emaEstLevel * _estLevelPctEma, 1);
            }
        }
        _lastTobQty = qty;
        _lastEmaTime = now;
    }

    // :424-446 — the part of onMarketDataChange that runs before the quote updates.
    void onTopBookChange(const book_side& bid, const book_side& ask, uint64_t now) noexcept {
        if ((bid.l[0].q > 0) && (ask.l[0].q > 0)) {
            calculateEstablishedLevel(
                static_cast<double>(bid.l[0].q + ask.l[0].q) / 2, now);
        }
        _midPoint = calculateMidPoint(bid, ask);
        const double marketSpread = ask.l[0].pr - bid.l[0].pr;
        _width = static_cast<price_t>(marketSpread / 2.0 * _widthPercentage);
    }

    // :105-122.
    qty_t getMaxQuoteSizeBasedOnMarket(side s, const top& book, price_t price) const noexcept {
        if (s == side::bid) {
            return price >= book.pr ? static_cast<qty_t>(book.q * _pctTobMaxQuote)
                                    : std::numeric_limits<qty_t>::max();
        }
        return price <= book.pr ? static_cast<qty_t>(book.q * _pctTobMaxQuote)
                                : std::numeric_limits<qty_t>::max();
    }

    // :1748-1766.
    qty_t getInternalQuoteSize(side s, qty_t maxQuoteSize) const noexcept {
        if (s == side::bid) {
            if (_internallyInitiatedPosition < 0) {
                return std::min(_maxInternallyInitiatedPosition, maxQuoteSize);
            }
            if (_maxInternallyInitiatedPosition <= _internallyInitiatedPosition) return 0;
            return std::min(_maxInternallyInitiatedPosition - _internallyInitiatedPosition,
                            maxQuoteSize);
        }
        if (_internallyInitiatedPosition > 0) {
            return std::min(_maxInternallyInitiatedPosition, maxQuoteSize);
        }
        if (_maxInternallyInitiatedPosition <= -_internallyInitiatedPosition) return 0;
        return std::min(_maxInternallyInitiatedPosition + _internallyInitiatedPosition,
                        maxQuoteSize);
    }

    // :1768-1790.
    qty_t getExternalQuoteSize(side s, qty_t quoteSize, qty_t maxQuoteSize) const noexcept {
        if (s == side::bid) {
            if (_externallyInitiatedPosition < 0) {
                return std::min(quoteSize,
                                std::min(_maxExternallyInitiatedPosition, maxQuoteSize));
            }
            if (_maxExternallyInitiatedPosition <= _externallyInitiatedPosition) return 0;
            return std::min(quoteSize,
                            std::min(_maxExternallyInitiatedPosition - _externallyInitiatedPosition,
                                     maxQuoteSize));
        }
        if (_externallyInitiatedPosition > 0) {
            return std::min(quoteSize, std::min(_maxExternallyInitiatedPosition, maxQuoteSize));
        }
        if (_maxExternallyInitiatedPosition <= -_externallyInitiatedPosition) return 0;
        return std::min(quoteSize,
                        std::min(_maxExternallyInitiatedPosition + _externallyInitiatedPosition,
                                 maxQuoteSize));
    }

    // :741-753.
    qty_t calculateCandidateExternalQuoteSize(side s, qty_t quoteSize,
                                              qty_t maxQuoteSizeBasedOnMarket) const noexcept {
        auto candidateSize =
            toValidQty(getExternalQuoteSize(s, quoteSize, maxQuoteSizeBasedOnMarket));
        const quote& q = _quotes[to_index(s)];
        if (candidateSize > q.last_external_size_sent) {
            if (((candidateSize - q.last_external_size_sent) < _quoteSize) &&
                (candidateSize > std::min<qty_t>(_quoteSize, maxQuoteSizeBasedOnMarket))) {
                candidateSize = q.last_external_size_sent;
            }
        }
        return candidateSize;
    }

    // :130-141.
    bool isThereEnoughSize(side s, const top& book, qty_t sizeThreshold,
                           price_t quotePrice) const noexcept {
        if (s == side::bid) {
            return ((quotePrice >= book.pr) & (book.q >= sizeThreshold)) | (quotePrice < book.pr);
        }
        return ((quotePrice <= book.pr) & (book.q >= sizeThreshold)) | (quotePrice > book.pr);
    }

    // :356-361.
    bool isInPayUp(qty_t ourSize, const top& book) const noexcept {
        const qty_t payUpBasedOnSize = static_cast<qty_t>(ourSize * _payUpMultiple);
        const qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnSize);
        return (book.q <= payupQuantity) |
               (static_cast<qty_t>(book.o) <= static_cast<qty_t>(_payUpOrders));
    }

    // :779-788, the calculateInternalPrice lambda.
    std::tuple<price_t, price_t> calculateInternalPrice(side s, price_t price,
                                                        qty_t qp) const noexcept {
        const double pctThroughQueue =
            std::max<double>(std::min<double>(1.0 - (double)qp / _establishedLevel, 1.0), 0.0);
        const double offsetScale =
            (2 / (1 + std::exp(-(double)_queueImportance * (pctThroughQueue - 0.5))) - 1);
        const price_t offset = offsetScale * _width;
        const price_t internalPrice = s == side::bid ? price + offset : price - offset;
        return std::make_tuple(internalPrice, roundToTickSize(s, internalPrice));
    }

    // :866-872.
    qty_t getSprayQty(side s, qty_t currentCumOrderQty, const top& book,
                      price_t externalPrice) const noexcept {
        auto size = currentCumOrderQty + _quoteSize;
        auto maxQuoteSizeBasedOnMarket =
            getMaxQuoteSizeBasedOnMarket(s, book, externalPrice);
        size = calculateCandidateExternalQuoteSize(s, size, maxQuoteSizeBasedOnMarket);
        return std::max<qty_t>(size - currentCumOrderQty, 0);
    }

    // :756-864. `price_orders` is the legacy `_priceOrders[Side]` map (price -> resting
    // orders, in insertion order), so the lookup below is the transcription of :796.
    std::tuple<price_t, price_t, qty_t> calculateQuotePriceAndSizeByOrders(
        side s, const book_side& book,
        const std::map<price_t, std::vector<legacy_order>>& price_orders, price_t price,
        bool quoteExternally) const noexcept {
        if (!quoteExternally) {
            return std::make_tuple(price, roundToTickSize(s, price), _quoteSize);
        }

        auto targetPrice = roundToTickSize(s, price);
        const top& touch = book.l[0];
        const bool better = s == side::bid ? targetPrice > touch.pr : targetPrice < touch.pr;
        if (better) {
            targetPrice = touch.pr;
            price = s == side::bid ? targetPrice + _commission : targetPrice - _commission;
        }

        if (_establishedLevel == 0) {
            return std::make_tuple(price, targetPrice, _quoteSize);
        }
        if (_queueImportance == 0) {
            return std::make_tuple(price, targetPrice, _quoteSize);
        }

        const auto targetLevel = book.getPriceLevelIndex(targetPrice);
        const qty_t targetSize = targetLevel == -1
                                     ? 0
                                     : book.targetSize(static_cast<uint32_t>(targetLevel));

        auto iter = price_orders.find(targetPrice);
        if (iter == price_orders.end() || iter->second.empty()) {
            price_t internalPrice;
            price_t externalPrice;
            std::tie(internalPrice, externalPrice) =
                calculateInternalPrice(s, price, targetSize);
            const bool target_worse = s == side::bid ? targetPrice > externalPrice
                                                     : targetPrice < externalPrice;
            if (target_worse) {
                return std::make_tuple(price, targetPrice, qty_t{0});
            }
            if (externalPrice != targetPrice) {
                return std::make_tuple(price, targetPrice, _quoteSize);
            }
            return std::make_tuple(internalPrice, externalPrice, _quoteSize);
        }

        price_t cost = 0;
        qty_t qty = 0;
        bool orderUnwanted = false;
        const auto& orders = iter->second;
        for (const legacy_order& order : orders) {
            qty_t qp = order.qp;
            if (order.qp == -1) {  // queue_pos_t::NOT_SET
                qp = targetSize;
            }
            price_t internalPrice;
            price_t externalPrice;
            std::tie(internalPrice, externalPrice) = calculateInternalPrice(s, price, qp);
            const bool betterOrEqual = s == side::bid ? externalPrice >= targetPrice
                                                      : externalPrice <= targetPrice;
            if (betterOrEqual) {
                cost += internalPrice * order.qty;
                qty += order.qty;
            } else {
                orderUnwanted = true;
                break;
            }
        }

        if (!orderUnwanted && orders.back().qp >= 0) {
            const qty_t qtyBehind = targetSize - orders.back().qp;
            if (qtyBehind >= _quoteGap) {
                price_t internalPrice;
                price_t externalPrice;
                std::tie(internalPrice, externalPrice) =
                    calculateInternalPrice(s, price, targetSize);
                const bool betterOrEqual = s == side::bid ? externalPrice >= targetPrice
                                                          : externalPrice <= targetPrice;
                if (betterOrEqual) {
                    const qty_t sprayQty = getSprayQty(s, qty, book.l[0], externalPrice);
                    if (sprayQty) {
                        cost += internalPrice * sprayQty;
                        qty += sprayQty;
                    }
                }
            }
        }

        if (qty) {
            const price_t internalPrice = cost / qty;
            const price_t externalPrice = roundToTickSize(s, internalPrice);
            const bool target_worse = s == side::bid ? targetPrice > externalPrice
                                                     : targetPrice < externalPrice;
            if (target_worse) {
                return std::make_tuple(price, targetPrice, qty_t{0});
            }
            if (externalPrice != targetPrice) {
                return std::make_tuple(price, targetPrice, qty);
            }
            return std::make_tuple(internalPrice, externalPrice, qty);
        }
        return std::make_tuple(price, targetPrice, qty_t{0});
    }

    // :661-739.
    bool updateQuote(side s, const book_side& book,
                     const std::map<price_t, std::vector<legacy_order>>& price_orders,
                     bool quoteExternally, bool forceUpdate) noexcept {
        quote& q = _quotes[to_index(s)];
        price_t price = s == side::bid ? _midPoint - _width : _midPoint + _width;

        price_t internalPrice;
        price_t externalPrice;
        qty_t quoteSize;
        std::tie(internalPrice, externalPrice, quoteSize) =
            calculateQuotePriceAndSizeByOrders(s, book, price_orders, price, quoteExternally);

        qty_t maxQuoteSizeBasedOnMarket = getMaxQuoteSizeBasedOnMarket(s, book.l[0], externalPrice);
        const bool isBookInPayUpMode =
            isInPayUp(getExternalQuoteSize(s, quoteSize, maxQuoteSizeBasedOnMarket), book.l[0]);

        bool changeInPrice = false;
        if (!isBookInPayUpMode) {
            changeInPrice =
                static_cast<price_t>(abs_diff_legacy(internalPrice, q.last_internal_price_sent)) >
                _minInternalPriceChange;
            q.internal_price = internalPrice;
            if (quoteExternally) q.external_price = externalPrice;
        } else {
            const price_t worsenPrice = s == side::bid
                                            ? book.l[0].pr - _worsenPayupConditionAmount
                                            : book.l[0].pr + _worsenPayupConditionAmount;
            const price_t ordinaryPrice =
                s == side::bid ? _midPoint - _width : _midPoint + _width;
            internalPrice = s == side::bid ? std::min(ordinaryPrice, worsenPrice)
                                           : std::max(ordinaryPrice, worsenPrice);
            auto extPrice = roundToTickSize(s, internalPrice);
            if (externalPrice != extPrice) {
                std::tie(internalPrice, externalPrice, quoteSize) =
                    calculateQuotePriceAndSizeByOrders(s, book, price_orders, internalPrice, quoteExternally);
            }
            changeInPrice = internalPrice != q.internal_price;
            q.internal_price = internalPrice;
            if (quoteExternally) q.external_price = extPrice;
        }

        bool changeInSize = false;
        maxQuoteSizeBasedOnMarket = getMaxQuoteSizeBasedOnMarket(s, book.l[0], q.external_price);
        if (quoteExternally) {
            if (q.external_size == 0) {
                if (isThereEnoughSize(s, book.l[0], _joinSize, q.external_price)) {
                    q.external_size = calculateCandidateExternalQuoteSize(
                        s, quoteSize, maxQuoteSizeBasedOnMarket);
                    changeInSize |= q.external_size != 0;
                }
            } else if (!isThereEnoughSize(s, book.l[0], _leaveSize, q.external_price)) {
                q.external_size = 0;
                changeInSize = true;
            } else {
                const qty_t candidateSize = calculateCandidateExternalQuoteSize(
                    s, quoteSize, maxQuoteSizeBasedOnMarket);
                if (candidateSize != q.external_size) {
                    q.external_size = candidateSize;
                    changeInSize = true;
                }
            }
        } else {
            changeInSize |= q.external_size != 0;
            q.external_size = 0;
        }

        const qty_t candidateInternalSize =
            toValidQty(getInternalQuoteSize(s, maxQuoteSizeBasedOnMarket));
        if (candidateInternalSize != q.internal_size) {
            q.internal_size = candidateInternalSize;
            changeInSize = true;
        }

        if (changeInPrice | changeInSize | forceUpdate) {
            q.last_internal_price_sent = q.internal_price;
            q.last_external_size_sent = q.external_size;
            return true;
        }
        return false;
    }

private:
    static uint64_t abs_diff_legacy(price_t a, price_t b) noexcept {
        const __int128 d = static_cast<__int128>(a) - static_cast<__int128>(b);
        return static_cast<uint64_t>(d < 0 ? -d : d);
    }
};

}  // namespace tachyon::trade::mm::oracle

