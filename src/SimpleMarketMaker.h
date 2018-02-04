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

    void initialize(ConfigContract& contract, hf::qty_t establishedLevel, MarketMakerConfig& config, ExecutorT* executor) {
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
        _executor = executor;
        _contract = contract;
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
        return _quoteExternal & enoughSizeToJoin;
    }

    bool canContinueQuotingBid(Book& bidBook) {
        return (bidBook._otherQuantity >= _leaveSize);
    }

    bool canContinueQuotingAsk(Book& askBook) {
        return (askBook._otherQuantity >= _leaveSize);
    }

    bool canStartQuotingAsk(Book& askBook) {
        const bool enoughSizeToJoin =  askBook._otherQuantity >= _joinSize;
        return _quoteExternal & enoughSizeToJoin;
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
        const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(askBook._otherQuantity * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnSizeInMarket);
        const bool shouldPayup = (bidBook._otherQuantity < payupQuantity) | (bidBook._otherOrderCount < _payUpOrders);
        return shouldPayup;
    }

    bool isBidInPayUp(Book& bidBook, Book& askBook) {
        const hf::qty_t payUpBasedOnSizeInMarket = static_cast<hf::qty_t>(bidBook._otherQuantity * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnSizeInMarket);
        const bool shouldPayup = (askBook._otherQuantity < payupQuantity) | (askBook._otherOrderCount < _payUpOrders);
        return shouldPayup;
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged) {
        if (!_enabled)
            return;

        const bool missingMarketData = ((bidBook._price == hf::NULL_PRICE) | (askBook._price == hf::NULL_PRICE));
        if (missingMarketData) {
            _enabled = false;
            _executor->sendGenericMessageToFront(ViperMessageType::MarketMakerEnabled, _contract._internalId, 0);
            _executor->sendErrorMessage(boost::str(boost::format("Not having market data so turning market maker off. %1%") % _contract._name), ErrorType::Critical);
            return;
        }

        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);

        if (bidChanged) {
            if (isHedgingAsk()) {
                Hedge& hedge = _hedges[ASK];
                if (isAskInPayUp(bidBook, askBook)) {
                    const hf::price_t newAskPrice = bidBook._price;
                    if (hedge._externalPrice != newAskPrice) {
                        hedge._externalPrice = newAskPrice;
                        _executor->modifyHedgeAskOrder(hedge._id, hedge._externalPrice - _commission, hedge._size, hedge._externalPrice, hedge._size);
                    }
                } else {
                    const hf::price_t internalPrice = midPoint - _commission;
                    if (hedge._internalPrice != internalPrice) {
                        hedge._internalPrice = internalPrice;
                        _executor->modifyHedgeAskOrder(hedge._id, internalPrice, hedge._size, hedge._externalPrice, hedge._size);
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
                        _executor->modifyHedgeBidOrder(hedge._id, hedge._externalPrice + _commission, hedge._size, hedge._externalPrice, hedge._size);
                    } else {
                        hf::price_t internalPrice = midPoint + _commission;
                        if (hedge._internalPrice != internalPrice) {
                            hedge._internalPrice = internalPrice;
                            _executor->modifyHedgeBidOrder(hedge._id, internalPrice, hedge._size, hedge._externalPrice, hedge._size);
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
                        _executor->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                    }
                } else {
                    _executor->removeBidOrder(quote._id);
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
                        _executor->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
                    }
                } else {
                    _executor->removeAskOrder(quote._id);
                }
            } else {
                tryStartQuotingAsk(quote, askBook, internalPrice, externalPrice);
            }

            _executor->matchOrders();
            _executor->synchronizeOrders();
        }
    }

    void tryStartQuotingBid(Quote& quote, Book& bidBook, hf::price_t internalPrice, hf::price_t externalPrice) {
        if (canStartQuotingBid(bidBook)) {
            quote._externalSize = getBidExternalQuoteSize();
            quote._internalSize = getBidInternalQuoteSize();
            quote._internalPrice = internalPrice;
            quote._externalPrice = externalPrice;
            _executor->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }
    }

    void tryStartQuotingAsk(Quote& quote, Book& askBook, hf::price_t internalPrice, hf::price_t externalPrice) {
        if (canStartQuotingAsk(askBook)) {
            quote._externalSize = getAskExternalQuoteSize();
            quote._internalSize = getAskInternalQuoteSize();
            quote._internalPrice = internalPrice;
            quote._externalPrice = externalPrice;
            _executor->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
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
            _executor->synchronizeOrders();
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
            _executor->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
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
            _executor->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }

        return update;
    }

    void enableQuoting(Book& bidBook, Book& askBook) {
        if (_toMarketMake) {
            _quoteExternal = true;
            if (_enabled) {
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
                _executor->matchOrders();
                _executor->synchronizeOrders();
            }
        }
    }

    void disableQuoting() {
        if (_toMarketMake) {
            _quoteExternal = false;
            if (_enabled) {
                Quote& bidQuote = _quotes[BID];
                _executor->removeBidOrder(bidQuote._id);
                Quote& askQuote = _quotes[ASK];
                _executor->removeAskOrder(askQuote._id);
                _executor->synchronizeOrders();
            }
        }
    }

    void enable(Book& bidBook, Book& askBook) {
        if (_toMarketMake) {
            _enabled = true;
            onChangeInOtherBooks(bidBook, askBook, true, true);
        }
    }

    void disable() {
        if (_toMarketMake) {
            _enabled = false;

            Quote& bidQuote = _quotes[BID];
            _executor->removeBidOrder(bidQuote._id);

            Quote& askQuote = _quotes[ASK];
            _executor->removeAskOrder(askQuote._id);

            Hedge& bidHedge = _hedges[BID];
            _executor->removeBidOrder(bidHedge._id);

            Hedge& askHedge = _hedges[ASK];
            _executor->removeAskOrder(askHedge._id);

            _executor->synchronizeOrders();
        }
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
            default:
                BOOST_ASSERT(false);
                break;
        }
    }

    bool validateParametersUpdate(const proto::GridCell& gridCell) {
        return true;
    }

    bool handleLogin() {
        return true;
    }

    bool _enabled = false;

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

    ExecutorT* _executor;
    ConfigContract _contract;

    bool _toMarketMake = false;

    Logger _logger;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
