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

template <typename ExecutorT>
class SimpleMarketMaker {

public:

    struct Quote {
        OrderId_t _id = ORDER_ID_MM_QUOTE;

        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::qty_t _internalSize = 0;

        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::qty_t _externalSize = 0;
    };

    struct Hedge {
        OrderId_t _id = ORDER_ID_MM_HEDGE;
        hf::price_t _externalPrice;
        hf::price_t _internalPrice;
        hf::qty_t _size;
    };

    void initialize(uint32_t internalId, hf::price_t tickSize, hf::qty_t establishedLevel, MarketMakerConfig& config, StackerConfig& stackerConfig, ExecutorT* executor) {
        _internalMatchingEngine.initialize(internalId, tickSize, stackerConfig, executor);
        _quoteExternal = config._quoteExternal;
        _quoteSize = config._quoteSize;
        _width = ToRhonePrice(config._width);
        _joinSize = config._joinSize;
        _leaveSize = config._leaveSize;
        _payUpQuantity = config._payUpQty;
        _payUpOrders  = config._payUpOrders;
        _payUpMultiple = config._payUpMultiple;
        _maxExternallyInitiatedPosition = config._maxPosExternal;
        _maxInternallyInitiatedPosition = config._maxPosInternal;
        _commission = ToRhonePrice(config._commission);
        _establishedLevel = establishedLevel;
        _tickSize = tickSize;
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

    bool canStartQuotingBid(Book& bidBook) {
        const bool enoughSizeToJoin = bidBook._otherQuantity >= _joinSize;
        return _quoteExternal & enoughSizeToJoin & !_missingMarketData;
    }

    bool canContinueQuotingBid(Book& bidBook) {
        return (bidBook._otherQuantity >= _leaveSize) & !_missingMarketData;
    }

    bool canContinueQuotingAsk(Book& askBook) {
        return (askBook._otherQuantity >= _leaveSize) & !_missingMarketData;
    }

    bool canStartQuotingAsk(Book& askBook) {
        const bool enoughSizeToJoin =  askBook._otherQuantity >= _joinSize;
        return _quoteExternal & enoughSizeToJoin & !_missingMarketData;
    }

    hf::qty_t getBidExternalQuoteSize() {
        if (_externallyInitiatedPosition < 0) {
            return _quoteSize;
        }
        if (_maxExternallyInitiatedPosition <= _externallyInitiatedPosition) {
            return 0;
        }
        return std::min(_quoteSize, _maxExternallyInitiatedPosition - _externallyInitiatedPosition);
    }

    hf::qty_t getAskExternalQuoteSize() {
        if (_externallyInitiatedPosition > 0) {
            return _quoteSize;
        }
        if (_maxExternallyInitiatedPosition <= -_externallyInitiatedPosition) {
            return 0;
        }
        return std::min(_quoteSize, _maxExternallyInitiatedPosition + _externallyInitiatedPosition);
    }

    hf::qty_t  getBidInternalQuoteSize() {
        if (_internallyInitiatedPosition < 0) {
            return _maxInternallyInitiatedPosition;
        }
        if (_maxInternallyInitiatedPosition <= _internallyInitiatedPosition) {
            return 0;
        }
        return _maxInternallyInitiatedPosition - _internallyInitiatedPosition;
    }

    hf::qty_t  getAskInternalQuoteSize() {
        if (_internallyInitiatedPosition > 0) {
            return _maxInternallyInitiatedPosition;
        }
        if (_maxInternallyInitiatedPosition <= -_internallyInitiatedPosition) {
            return 0;
        }
        return _maxInternallyInitiatedPosition + _internallyInitiatedPosition;
    }

    bool isAskInPayUp(Book& bidBook, Book& askBook) {
        const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(askBook._ourQuantity * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnSizeInMarket);
        const bool shouldPayup = (bidBook._otherQuantity < payupQuantity) | (bidBook._otherOrderCount < _payUpOrders);
        return shouldPayup;
    }

    bool isBidInPayUp(Book& bidBook, Book& askBook) {
        const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(bidBook._ourQuantity * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnSizeInMarket);
        const bool shouldPayup = (askBook._otherQuantity < payupQuantity) | (askBook._otherOrderCount < _payUpOrders);
        return shouldPayup;
    }

    void sendCriticalMessage(const std::string& message) {
        //TODO
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged) {
        if (!_enabled)
            return;

        _missingMarketData = ((bidBook._price == hf::NULL_PRICE) | (askBook._price == hf::NULL_PRICE));

        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);

        if (bidChanged) {
            if (isHedgingAsk()) {
                if (BOOST_UNLIKELY(bidBook._price == hf::NULL_PRICE)) {
                    disable();
                    sendCriticalMessage("Hedging Ask but Bid gone. Turning off MM.");
                } else {
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
        }

        if (askChanged) {
            if (isHedgingBid()) {
                if (BOOST_UNLIKELY(askBook._price == hf::NULL_PRICE)) {
                    disable();
                    sendCriticalMessage("Hedging Ask but Bid gone. Turning off MM.");
                } else {
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
        }

        {
            const hf::price_t internalPrice = midPoint - _width - _commission;
            const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
            Quote& quote = _quotes[BID];

            if (isQuotingBid()) {
                if (canContinueQuotingBid(bidBook)) {
                    if (internalPrice != quote._internalPrice) {
                        quote._internalPrice = internalPrice;
                        quote._externalPrice = externalPrice;
                        _internalMatchingEngine.modifyBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                    }
                } else {
                   _internalMatchingEngine.removeBidOrder(quote._id);
                }
            } else {
                tryStartQuotingBid(quote, bidBook, internalPrice, externalPrice);
            }
        }

        {
            const hf::price_t internalPrice = midPoint + _width + _commission;
            const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
            Quote& quote = _quotes[ASK];

            if (isQuotingAsk()) {
                if (canContinueQuotingAsk(askBook)) {
                    if (internalPrice != quote._internalPrice) {
                        quote._internalPrice = internalPrice;
                        quote._externalPrice = externalPrice;
                        _internalMatchingEngine.modifyAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                    }
                } else {
                    _internalMatchingEngine.removeAskOrder(quote._id);
                }
            } else {
                tryStartQuotingAsk(quote, askBook, internalPrice, externalPrice);
            }

             _internalMatchingEngine.matchOrders();
            _internalMatchingEngine.synchronizeOrders();
        }
    }

    void tryStartQuotingBid(Quote& quote, Book& bidBook, hf::price_t internalPrice, hf::price_t externalPrice) {
        if (canStartQuotingBid(bidBook)) {
            quote._externalSize = getBidExternalQuoteSize();
            quote._internalSize = getBidInternalQuoteSize();
            quote._internalPrice = internalPrice;
            quote._externalPrice = externalPrice;
            _internalMatchingEngine.modifyBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }
    }

    void tryStartQuotingAsk(Quote& quote, Book& askBook, hf::price_t internalPrice, hf::price_t externalPrice) {
        if (canStartQuotingAsk(askBook)) {
            quote._externalSize = getAskExternalQuoteSize();
            quote._internalSize = getAskInternalQuoteSize();
            quote._internalPrice = internalPrice;
            quote._externalPrice = externalPrice;
            _internalMatchingEngine.modifyAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }
    }

    hf::price_t calculateMidPoint(Book& bid, Book& ask) {
        const hf::qty_t bidSize = std::max(bid._otherQuantity, _establishedLevel);
        const hf::qty_t askSize = std::max(ask._otherQuantity, _establishedLevel);
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

        if (quote._internalPrice == hf::NULL_PRICE) {
            return false;
        }

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

        if (quote._internalPrice == hf::NULL_PRICE) {
            return false;
        }

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

    void enableQuoting(Book& bidBook, Book& askBook) {
        _quoteExternal = true;
        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);
        {
            const hf::price_t internalPrice = midPoint - _width - _commission;
            const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
            Quote& quote = _quotes[BID];
            tryStartQuotingBid(quote, bidBook, internalPrice, externalPrice);
        }
        {
            const hf::price_t internalPrice = midPoint + _width + _commission;
            const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
            Quote& quote = _quotes[ASK];
            tryStartQuotingAsk(quote, askBook, internalPrice, externalPrice);
        }
        _internalMatchingEngine.matchOrders();
        _internalMatchingEngine.synchronizeOrders();
    }

    void disableQuoting() {
        _quoteExternal = false;

        Quote& bidQuote = _quotes[BID];
        _internalMatchingEngine.removeBidOrder(bidQuote._id);

        Quote& askQuote = _quotes[ASK];
        _internalMatchingEngine.removeAskOrder(askQuote._id);

        _internalMatchingEngine.synchronizeOrders();
    }

    void enable(Book& bidBook, Book& askBook) {
        _enabled = true;
        onChangeInOtherBooks(bidBook, askBook, true, true);
    }

    void disable() {
        _enabled = false;

        Quote& bidQuote = _quotes[BID];
        _internalMatchingEngine.removeBidOrder(bidQuote._id);

        Quote& askQuote = _quotes[ASK];
        _internalMatchingEngine.removeAskOrder(askQuote._id);

        Hedge& bidHedge = _hedges[BID];
        _internalMatchingEngine.removeBidOrder(bidHedge._id);

        Hedge& askHedge = _hedges[ASK];
        _internalMatchingEngine.removeAskOrder(askHedge._id);

        _internalMatchingEngine.synchronizeOrders();
    }

    void handleParametersUpdate(const proto::GridCell& gridCell) {
        MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(gridCell.col());
        switch (columnType) {
            case MarketMakerParametersColumns::QuoteExternal: {
                _quoteExternal = 1 == atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::QuoteSize: {
                _quoteSize = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::JoinSize: {
                _joinSize = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::LeaveSize: {
                _leaveSize = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::MaxPosInterrnal: {
                _maxInternallyInitiatedPosition = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::MaxPosExternal: {
                _maxExternallyInitiatedPosition = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::PayUpOrders: {
                _payUpOrders = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::PayUpQty: {
                _payUpQuantity = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::PayUpMultiple: {
                _payUpMultiple = atof(gridCell.value().c_str());
            }
            case MarketMakerParametersColumns::Width: {
                _width = atoi(gridCell.value().c_str());
                break;
            }
            case MarketMakerParametersColumns::Commission: {
                _commission = atoi(gridCell.value().c_str());
                break;
            }
        }
    }

    bool validateParametersUpdate(const proto::GridCell& gridCell) {
        return true;
    }

    bool handleLogin() {
        //TODO
        return true;
    }

    bool _quoteExternal;
    hf::qty_t _quoteSize;
    hf::price_t _width;
    hf::qty_t _joinSize;
    hf::qty_t _leaveSize;
    hf::qty_t _payUpQuantity;
    md::order_count_t _payUpOrders;
    double _payUpMultiple;
    hf::qty_t _maxExternallyInitiatedPosition;
    hf::qty_t _maxInternallyInitiatedPosition;
    hf::price_t _commission;
    hf::qty_t _establishedLevel;
    hf::price_t _tickSize;

    hf::qty_t _internallyInitiatedPosition = 0;
    hf::qty_t _externallyInitiatedPosition = 0;
    Hedge _hedges[2];
    Quote _quotes[2];
    InternalMatchingEngine<ExecutorT> _internalMatchingEngine;

    bool _enabled = false;
    bool _missingMarketData = false;

    Logger _logger;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
