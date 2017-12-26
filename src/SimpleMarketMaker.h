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
        hf::price_t _price = 0;
        hf::qty_t _size;
        hf::qty_t _scalpingSize = 0;
        hf::qty_t _internalSize = 0;
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

    bool isHedgingAsk() {
        return _hedges[ASK]._externalPrice != hf::NULL_PRICE;
    }

    bool isHedgingBid() {
        return _hedges[BID]._externalPrice != hf::NULL_PRICE;
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

        const bool hasBidMinJoinSize = bidBook._otherQuantity >= _config._joinSize;

        const hf::price_t rawPrice = midPoint - _width;
        const hf::price_t newBidPrice = RoundToTickSize(rawPrice, _tickSize, BID);
        Quote& bidQuote = _quotes[BID];

        if (isQuotingBid()) {
            if (hasBidMinJoinSize) {
                if (newBidPrice != bidQuote._price) {
            //       _internalMatchingEngine.modifyOrder<BID>(bidQuote._id, newBidPrice, false);
                }
            } else {
            //    _internalMatchingEngine.cancelOrder<BID>(bidQuote._id);
            }
        } else {
            if (hasBidMinJoinSize) {
            //    _internalMatchingEngine.addOrder<BID>(_quotes->_id, newBidPrice, bidQuote._size);
                bidQuote._price = newBidPrice;
            }
        }

        const bool hasAskMinJoinSize = askBook._otherQuantity >= _config._joinSize;

        const hf::price_t askRawPrice = midPoint + _width;
        const hf::price_t newAskPrice = RoundToTickSize(askRawPrice, _tickSize, ASK);
        Quote& askQuote = _quotes[ASK];

        if (isQuotingAsk()) {
            if (hasAskMinJoinSize) {
                if (newAskPrice != askQuote._price) {
       //             _internalMatchingEngine.modifyOrder<ASK>(askQuote._id, newAskPrice, false);
                }
            } else {
        //        _internalMatchingEngine.cancelOrder<ASK>(askQuote._id);
            }
        } else {
            if (hasAskMinJoinSize) {
        //        _internalMatchingEngine.addOrder<ASK>(_quotes->_id, newBidPrice, askQuote._size);
                askQuote._price = newAskPrice;
            }
        }

       // _internalMatchingEngine.commit();
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
    Hedge _hedges[2];
    Quote _quotes[2];
    InternalMatchingEngine<ExecutorT> _internalMatchingEngine;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
