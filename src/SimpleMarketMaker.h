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
#include "Logger.h"

namespace Python {

struct MMConfig {
    hf::qty_t _quoteSize;
    hf::price_t _width;
    hf::price_t _commission;
    hf::qty_t _sizeCap;
    hf::qty_t _joinSize;
    hf::qty_t _leaveSize;
    hf::qty_t _payupQuantity;
    double _payupMultiple;
    md::order_count_t _payupNumberOfOrders;
    hf::qty_t _maxInternallyInitiatedPosition;
    hf::qty_t _maxExternallyInitiatedPosition;
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
        hf::price_t _internalPrice;
        hf::qty_t _size;
    };

    void initialize(ImConfig& config, ExecutorT* executor) {
        _internalMatchingEngine.initialize(config, executor);
    }

    void onQuoteOrderFromRv(QuoteOrderMessage& order) {
        bool updateQuotes;

        if (order._side == BID) {
            _internalMatchingEngine.modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, order);
            updateQuotes = _internalMatchingEngine.matchOrders();
        } else {
            _internalMatchingEngine.modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, order);
            updateQuotes = _internalMatchingEngine.matchOrders();
        }

        if (updateQuotes) {
            calculateBidQuoteSizes();
            calculateAskQuoteSizes();
        }

        _internalMatchingEngine.synchronizeOrders();
    }

    hf::qty_t onHedgeOrderFromRv(HedgeOrderMessage& order) {
        return _internalMatchingEngine.tryMatch(order._side, order._price, order._size);
    }

    bool isHedgingAsk() {
        return _hedges[ASK]._size != 0;
    }

    bool isHedgingBid() {
        return _hedges[BID]._size != 0;
    }

    bool isQuotingAsk() {
        return _quotes[ASK]._internalSize != 0;
    }

    bool isQuotingBid() {
        return _quotes[BID]._internalSize != 0;
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
        if (_externallyInitiatedPosition < 0) {
            return _config._quoteSize;
        }
        if (_config._maxExternallyInitiatedPosition <= _externallyInitiatedPosition) {
            return 0;
        }
        return std::min(_config._quoteSize, _config._maxExternallyInitiatedPosition - _externallyInitiatedPosition);
    }

    hf::qty_t getAskExternalQuoteSize() {
        if (_externallyInitiatedPosition > 0) {
            return _config._quoteSize;
        }
        if (_config._maxExternallyInitiatedPosition <= -_externallyInitiatedPosition) {
            return 0;
        }
        return std::min(_config._quoteSize, _config._maxExternallyInitiatedPosition + _externallyInitiatedPosition);
    }

    hf::qty_t  getBidInternalQuoteSize() {
        if (_internallyInitiatedPosition < 0) {
            return _config._maxInternallyInitiatedPosition;
        }
        if (_config._maxInternallyInitiatedPosition <= _internallyInitiatedPosition) {
            return 0;
        }
        return _config._maxInternallyInitiatedPosition - _internallyInitiatedPosition;
    }

    hf::qty_t  getAskInternalQuoteSize() {
        if (_internallyInitiatedPosition > 0) {
            return _config._maxExternallyInitiatedPosition;
        }
        if (_config._maxInternallyInitiatedPosition <= -_internallyInitiatedPosition) {
            return 0;
        }
        return _config._maxInternallyInitiatedPosition + _internallyInitiatedPosition;
    }

    bool isAskInPayUp(Book& bidBook, Book& askBook) {
        const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(askBook._ourQuantity * _config._payupMultiple);
        const hf::qty_t payupQuantity = std::max(_config._payupQuantity, payUpBasedOnSizeInMarket);
        const bool shouldPayup = (bidBook._otherQuantity < payupQuantity) | (bidBook._otherOrderCount < _config._payupNumberOfOrders);
        return shouldPayup;
    }

    bool isBidInPayUp(Book& bidBook, Book& askBook) {
        const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(bidBook._ourQuantity * _config._payupMultiple);
        const hf::qty_t payupQuantity = std::max(_config._payupQuantity, payUpBasedOnSizeInMarket);
        const bool shouldPayup = (askBook._otherQuantity < payupQuantity) | (askBook._otherOrderCount < _config._payupNumberOfOrders);
        return shouldPayup;
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged) {
        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);

        if (bidChanged) {
            if (isHedgingAsk()) {
                Hedge& hedge = _hedges[ASK];
                if (isAskInPayUp(bidBook, askBook)) {
                    const hf::price_t newAskPrice = bidBook._price;
                    if (hedge._externalPrice != newAskPrice) {
                        hedge._externalPrice = newAskPrice;
                        _internalMatchingEngine.modifyAskOrder(hedge._id, hedge._externalPrice - _commission, hedge._size, hedge._externalPrice, hedge._size);
                    }
                } else {
                    const hf::price_t internalPrice = midPoint - _commission;
                    if (hedge._internalPrice != internalPrice) {
                        hedge._internalPrice = internalPrice;
                        _internalMatchingEngine.modifyAskOrder(hedge._id, internalPrice, hedge._size, hedge._externalPrice, hedge._size);
                    }
                }
            }
        }

        if (askChanged) {
            if (isHedgingBid()) {
                if (isBidInPayUp(bidBook, askBook)) {
                    const hf::price_t newBidPrice = askBook._price;
                    Hedge& hedge = _hedges[BID];
                    if (hedge._externalPrice != newBidPrice) {
                        hedge._externalPrice = newBidPrice;
                        _internalMatchingEngine.modifyBidOrder(hedge._id, hedge._externalPrice + _commission, hedge._size, hedge._externalPrice, hedge._size);
                    } else {
                        hf::price_t internalPrice = midPoint + _commission;
                        if (hedge._internalPrice != internalPrice) {
                            hedge._internalPrice = internalPrice;
                            _internalMatchingEngine.modifyAskOrder(hedge._id, internalPrice, hedge._size, hedge._externalPrice, hedge._size);
                        }
                    }
                }
            }
        }

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

    void onFill(Order* order, hf::price_t price, hf::qty_t size) {
        if (order->_type == Order::Type::Hedging) {
            Hedge& hedge = _hedges[order->_side];

            if (size <= hedge._size) {
                hedge._size -= size;
            } else {
                hedge._size = 0;
                _logger.logError("Received fill where fill size is more than hedge has.");
            }

            if (order->_side == BID) {
                hf::qty_t interInitFill = 0;
                if (_internallyInitiatedPosition < 0) {
                    interInitFill = std::min(-_internallyInitiatedPosition, size);
                    _internallyInitiatedPosition += interInitFill;
                }
                const hf::qty_t externInitFill = size - interInitFill;
                _externallyInitiatedPosition += externInitFill;
            }
            else {
                hf::qty_t interInitFill = 0;
                if (_internallyInitiatedPosition > 0) {
                    interInitFill = std::min(_internallyInitiatedPosition, size);
                    _internallyInitiatedPosition -= interInitFill;
                }
                const hf::qty_t externInitFill = size - interInitFill;
                _externallyInitiatedPosition -= externInitFill;
            }
        } else {
            Quote& quote = _quotes[order->_side];
            if (size <= quote._internalSize) {
                quote._internalSize -= size;
            } else {
                _logger.logError("Received fill where fill size is more than quote has.");
                quote._internalSize = 0;
            }

            if (order->_side == BID) {
                _internallyInitiatedPosition += size;
            } else {
                _internallyInitiatedPosition -= size;
            }
        }

    }

    void onInternalFill(Order* order, hf::price_t price, hf::qty_t size) {
        onFill(order, price, size);
    }

    void onExternalFill(Order* order, hf::price_t price, hf::qty_t size) {
        onFill(order, price, size);
    }

    void onEndOfExternalFills() {
        bool update = false;
        update |= calculateBidQuoteSizes();
        update |= calculateAskQuoteSizes();

        if (update) {
            _internalMatchingEngine.synchronizeOrders();
        }
    }

    bool calculateBidQuoteSizes() {
        bool update = false;
        Quote& quote = _quotes[BID];

        const hf::qty_t internalSize = getBidInternalQuoteSize();
        update |= internalSize != quote._internalSize;

        const hf::qty_t externalSize = getBidExternalQuoteSize();
        update |= externalSize != quote._externalSize;

        if (update) {
            quote._internalSize = internalSize;
            quote._externalSize = externalSize;
            _internalMatchingEngine.modifyBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }

        return update;
    }

    bool calculateAskQuoteSizes() {
        bool update = false;
        Quote& quote = _quotes[ASK];

        const hf::qty_t internalSize = getAskInternalQuoteSize();
        update |= internalSize != quote._internalSize;

        const hf::qty_t externalSize = getAskExternalQuoteSize();
        update |= externalSize != quote._externalSize;

        if (update) {
            quote._internalSize = internalSize;
            quote._externalSize = externalSize;
            _internalMatchingEngine.modifyAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }

        return update;
    }

    MMConfig _config;
    hf::price_t _tickSize;
    hf::price_t _commission;
    hf::qty_t _internallyInitiatedPosition = 0;
    hf::qty_t _externallyInitiatedPosition = 0;
    hf::price_t _width;
    Hedge _hedges[2];
    Quote _quotes[2];
    InternalMatchingEngine<ExecutorT> _internalMatchingEngine;
    Logger _logger;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
