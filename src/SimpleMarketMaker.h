#ifndef RHONEOMS_SIMPLE_MARKET_MAKER_H
#define RHONEOMS_SIMPLE_MARKET_MAKER_H

#include <stdint.h>
#include <hf/hf.h>
#include <md/md_event.h>
#include <om/om_event.h>
#include <cmath>
#include "Common.h"
#include "Stacker.h"
#include "Executor.h"
#include "InternalMatchingEngine.h"

namespace Python {

struct MarketMakerConfig {
    hf::price_t _width;
    hf::qty_t _joinSize;
    hf::qty_t _quoteSize;
    hf::qty_t _payupQuantity;
    md::order_count_t _payupNumberOfOrders;
    hf::qty_t _sizeCap;
    hf::qty_t _maxInternalPosition;
    hf::qty_t _maxScalpPosition;
    hf::price_t _commision;
    double _payupMultiple;
};

/*
 * MM - market
 * liquidating market = size_to_liquidate@liquidate_price (liquidate_price = scalp_price - commission)
 * internal_market = available_internal_size@size_weighted_price
 *
 * MM will have 2 quotes
 *
 */


class SimpleMarketMaker {

public:

    struct Quote {
        hf::price_t _price = 0;
        hf::qty_t _scalpingSize = 0;
        hf::qty_t _internalSize = 0;
    };

    struct Hedges {
        Order _order;
        hf::qty_t _scalp;
        hf::qty_t _other;
    };

    void initialize(MarketMakerConfig& config) {
        _hedges[BID]._order._id = 1;
        _hedges[ASK]._order._id = 2;
    }

    void onInternalFill(Order& inOrder, hf::qty_t size) {
        if (inOrder._type == Order::Type::Hedging) {
            processHedgingFill(inOrder, size);
        } else {
            processViperLiquidatingFill(inOrder, size);
        }
    }

    void processHedgingFill(Order& inOrder, hf::qty_t size) {
        const hf::qty_t positionChange = (inOrder._side == BID ? size : -size);
        Hedges& hedges = _hedges[inOrder._side];

    }

    void processViperLiquidatingFill(Order& inOrder, hf::qty_t size) {
        Order& order = _liquidatingOrder[inOrder._side];
        _scalpPosition += (order._side == BID ? size : -size);
        adjustQuotingSizeAfterLiquidatingFill();
    }

    void adjustQuotingSizeAfterLiquidatingFill() {
        hf::qty_t bidSize = getBidScalpQuoteSize();
        Quote& bidScalpingQuote = _quotes[BID];
        if (bidScalpingQuote._scalpingSize != bidSize) {
            bidScalpingQuote._scalpingSize = bidSize;
        }

        hf::qty_t askSize = getAskScalpQuoteSize();
        Quote& askScalpingQuote = _quotes[ASK];
        if (askScalpingQuote._scalpingSize != askSize) {
            askScalpingQuote._scalpingSize = askSize;
        }
    }

    hf::qty_t getBidScalpQuoteSize() {
        if (_scalpPosition > 0) {
            return std::max(_config._maxScalpPosition - _scalpPosition, _config._quoteSize);
        }
        return _config._quoteSize - _scalpPosition;
    }

    hf::qty_t getAskScalpQuoteSize() {
        if (_scalpPosition < 0) {
            return std::max(_config._maxScalpPosition + _scalpPosition, _config._quoteSize);
        }
        return _config._quoteSize + _scalpPosition;
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged) {
        if (bidChanged) {
            if (isQuotingAsk()) {
                const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(askBook._ourQuantity * _config._payupMultiple);
                const hf::qty_t payupQuantity = std::max(_config._payupQuantity, payUpBasedOnSizeInMarket);
                const bool shouldPayup = (bidBook._otherQuantity < payupQuantity) | (bidBook._otherOrderCount < _config._payupNumberOfOrders);
                if (shouldPayup) {
                    const hf::price_t newOfferPrice = bidBook._price;
                    Order& hedgeOrder = _hedges[ASK];
                    _internalMatchingEngine.modifyOrder(hedgeOrder._id, newOfferPrice);
                }
            }
        }

        if (askChanged) {
            if (isQuotingBid()) {
                const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(bidBook._ourQuantity * _config._payupMultiple);
                const hf::qty_t payupQuantity = std::max(_config._payupQuantity, payUpBasedOnSizeInMarket);
                const bool shouldPayup = (askBook._otherQuantity < payupQuantity) | (askBook._otherOrderCount < _config._payupNumberOfOrders);
                if (shouldPayup) {
                  // TODO
                }

            }
        }




        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);

        const bool hasBidMinJoinSize = bidBook._otherQuantity >= _config._joinSize;

        if (isQuotingBid()) {
            const hf::price_t rawPrice = midPoint - _width;
            const hf::price_t newBidPrice = RoundToTickSize(rawPrice, _tickSize, BID);
            if (hasBidMinJoinSize) {
                if (newBidPrice != bidBook._price) {
                    _stacker.modify(newBidPrice);
                }
            } else {
                _stacker.modify(newBidPrice, 0);
            }
        }
    }

    bool isQuotingAsk() {
        return _quotes[ASK]._price != hf::NULL_PRICE;
    }

    bool isQuotingBid() {
        return _quotes[BID]._price != hf::NULL_PRICE;
    }

    hf::price_t calculateMidPoint(Book& bid, Book& ask) {
        const hf::qty_t bidSize = std::max(bid._otherQuantity, _config._sizeCap);
        const hf::qty_t askSize = std::max(ask._otherQuantity, _config._sizeCap);
        return (bid._price * askSize + ask._price * bidSize) / (bidSize + askSize);
    }

    MarketMakerConfig _config;
    hf::price_t _tickSize;
    hf::qty_t _internalPosition = 0;
    hf::qty_t _scalpPosition = 0;
    hf::price_t _width;
    Stacker _stacker;
    Hedges _hedges[2];
    Quote _quotes[2];
    InternalMatchingEngine& _internalMatchingEngine;
    Executor& _executor;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
