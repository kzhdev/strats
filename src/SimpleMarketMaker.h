#ifndef RHONEOMS_SIMPLE_MARKET_MAKER_H
#define RHONEOMS_SIMPLE_MARKET_MAKER_H

#include <stdint.h>
#include <hf/hf.h>
#include <md/md_event.h>
#include <om/om_event.h>
#include <cmath>
#include "Common.h"
#include "Stacker.h"
#include "InternalMatchingEngine.h"
#include "ViperConfig.h"

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

template <typename ExecutorT>
class SimpleMarketMaker {

public:

    struct Quote {
        OrderId_t _id;

        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::qty_t _internalSize = 0;

        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::qty_t _externalSize = 0;
    };

    struct Hedge {
        OrderId_t _id;
        hf::price_t _externalPrice;
        hf::price_t _commission;
        hf::qty_t _size;
    };

    void initialize(ImConfig& config, ExecutorT* executor) {
        _internalMatchingEngine.initialize(config, executor);
    }

    void onQuoteOrderFromRv(QuoteOrderMessage& order) {
        if (order._side == BID) {
            Order& o = _internalMatchingEngine.modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, order);
            bool wasFilled = _internalMatchingEngine.matchOrders();
        } else {
            Order& o =_internalMatchingEngine.modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, order);
            bool wasFilled = _internalMatchingEngine.matchOrders();
        }
        _internalMatchingEngine.synchronizeOrders();
    }

    hf::qty_t onHedgeOrderFromRv(HedgeOrderMessage& order) {
        return _internalMatchingEngine.tryMatch(order._side, order._price, order._size);
    }

    bool isHedgingAsk() {
        return _hedges[ASK]._externalPrice != hf::NULL_PRICE;
    }

    bool isHedgingBid() {
        return _hedges[BID]._externalPrice != hf::NULL_PRICE;
    }

    bool isQuotingAsk() {
        return _quotes[ASK]._internalPrice != hf::NULL_PRICE;
    }

    bool isQuotingBid() {
        return _quotes[BID]._internalPrice != hf::NULL_PRICE;
    }

    bool canQuoteBid(Book& bidBook) {
        const bool hasBidMinJoinSize = bidBook._otherQuantity >= _config._joinSize;
        return hasBidMinJoinSize;
    }

    bool canQuoteAsk(Book& askBook) {
        const bool hasAskMinJoinSize = askBook._otherQuantity >= _config._joinSize;
        return hasAskMinJoinSize;
    }

    hf::qty_t getBidExternalQuoteSize() {
        return 0;
    }

    hf::qty_t  getBidInternalQuoteSize() {
        return 0;
    }

    hf::qty_t getAskExternalQuoteSize() {
        return 0;
    }

    hf::qty_t  getAskInternalQuoteSize() {
        return 0;
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged) {
        if (bidChanged) {
            if (isHedgingAsk()) {
                const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(askBook._ourQuantity * _config._payupMultiple);
                const hf::qty_t payupQuantity = std::max(_config._payupQuantity, payUpBasedOnSizeInMarket);
                const bool shouldPayup = (bidBook._otherQuantity < payupQuantity) | (bidBook._otherOrderCount < _config._payupNumberOfOrders);
                if (shouldPayup) {
                    const hf::price_t newAskPrice = bidBook._price;
                    Hedge& hedge = _hedges[ASK];
                    if (hedge._externalPrice != newAskPrice) {
                        hedge._externalPrice = newAskPrice;
                        _internalMatchingEngine.modifyAskOrder(hedge._id, hedge._externalPrice - hedge._commission, hedge._size, hedge._externalPrice, hedge._size);
                    }
                }
            }
        }

        if (askChanged) {
            if (isHedgingBid()) {
                const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(bidBook._ourQuantity * _config._payupMultiple);
                const hf::qty_t payupQuantity = std::max(_config._payupQuantity, payUpBasedOnSizeInMarket);
                const bool shouldPayup = (askBook._otherQuantity < payupQuantity) | (askBook._otherOrderCount < _config._payupNumberOfOrders);
                if (shouldPayup) {
                    const hf::price_t newBidPrice = askBook._price;
                    Hedge& hedge = _hedges[BID];
                    if (hedge._externalPrice != newBidPrice) {
                        hedge._externalPrice = newBidPrice;
                        _internalMatchingEngine.modifyBidOrder(hedge._id, hedge._externalPrice + hedge._commission, hedge._size, hedge._externalPrice, hedge._size);
                    }
                }
            }
        }

        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);

        {
            const hf::price_t internalPrice = midPoint - _width;
            const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
            Quote& quote = _quotes[BID];

            if (isQuotingBid()) {
                if (canQuoteBid(bidBook)) {
                    if (internalPrice != quote._internalPrice) {
                        quote._internalPrice = internalPrice;
                        quote._externalPrice = externalPrice;
                        _internalMatchingEngine.modifyBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                    }
                } else {
                   _internalMatchingEngine.removeBidOrder(quote._id);
                }
            } else {
                if (canQuoteBid(bidBook)) {
                    quote._externalSize = getBidExternalQuoteSize();
                    quote._internalSize = getBidInternalQuoteSize();
                    quote._internalPrice = internalPrice;
                    quote._externalPrice = externalPrice;
                    _internalMatchingEngine.modifyBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                }
            }
        }

        {
            const hf::price_t internalPrice = midPoint + _width;
            const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
            Quote& quote = _quotes[ASK];

            if (isQuotingAsk()) {
                if (canQuoteAsk(askBook)) {
                    if (internalPrice != quote._internalPrice) {
                        _internalMatchingEngine.modifyAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                    }
                } else {
                    _internalMatchingEngine.removeAskOrder(quote._id);
                }
            } else {
                if (canQuoteAsk(askBook)) {
                    quote._externalSize = getAskExternalQuoteSize();
                    quote._internalSize = getAskInternalQuoteSize();
                    quote._internalPrice = internalPrice;
                    quote._externalPrice = externalPrice;
                    _internalMatchingEngine.modifyAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                }
            }

             _internalMatchingEngine.matchOrders();
            _internalMatchingEngine.synchronizeOrders();
        }
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
    Hedge _hedges[2];
    Quote _quotes[2];
    InternalMatchingEngine<ExecutorT> _internalMatchingEngine;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
