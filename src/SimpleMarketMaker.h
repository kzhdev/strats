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
        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::qty_t _size = 0;
    };

    void initialize(ConfigContract& contract, hf::qty_t establishedLevel, MarketMakerConfig& config, ExecutorT* executor) {
        _enabled = config._enabled;
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
        _tickSize = contract._tickSize;
        _toMarketMake = true;
    }

    hf::qty_t takeOverFill(uint32_t side, hf::qty_t overFillSize) {
        _logger.logInfo(boost::str(boost::format("Market maker. Take overfill. Size=%1%") % overFillSize));
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        const hf::qty_t newPosition = overFillSize * multiplier + _externallyInitiatedPosition;
        if (std::abs(newPosition) > _maxExternallyInitiatedPosition) {
            hf::qty_t sizeTaken = _maxExternallyInitiatedPosition - std::abs(_externallyInitiatedPosition);
            _externallyInitiatedPosition += sizeTaken * multiplier;
            _logger.logInfo(boost::str(boost::format("Market maker. Could not take the whole overfill. Took=%1%") % sizeTaken));
            return sizeTaken;
        } else {
            _externallyInitiatedPosition = newPosition;
            _logger.logInfo(boost::str(boost::format("Market maker. Took the whole overfill. Took=%1%") % overFillSize));
            return overFillSize;
        }
    }

    bool isHedgingAsk() {
        return _hedges[ASK]._size != 0;
    }

    bool isHedgingBid() {
        return _hedges[BID]._size != 0;
    }

    bool canStartQuotingBidExternally(Book& bidBook) {
        const bool enoughSizeToJoin = bidBook._otherQuantity >= _joinSize;
        return _quoteExternal & enoughSizeToJoin;
    }

    bool canContinueQuotingBidExternally(Book& bidBook) {
        return _quoteExternal & (bidBook._otherQuantity >= _leaveSize);
    }

    bool canContinueQuotingAskExternally(Book& askBook) {
        return _quoteExternal & (askBook._otherQuantity >= _leaveSize);
    }

    bool canStartQuotingAskExternally(Book& askBook) {
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

    bool isEnabled() {
        return _enabled && _executor->_executor->isEnabled();
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged) {
        if (!isEnabled())
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
            Quote& quote = _quotes[BID];
            if (internalPrice != quote._internalPrice) {
                quote._internalPrice = internalPrice;
            }
            if (quote._externalSize == 0) {
                if (canStartQuotingBidExternally(bidBook)) {
                    quote._externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
                    quote._externalSize = getBidExternalQuoteSize();
                }
            } else {
                if (canContinueQuotingBidExternally(bidBook)) {
                    quote._externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
                } else {
                    quote._externalPrice =  hf::NULL_PRICE;
                    quote._externalSize = 0;
                }
            }
            _executor->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }

        {
            const hf::price_t internalPrice = midPoint + _width + _commission;
            Quote& quote = _quotes[ASK];
            if (internalPrice != quote._internalPrice) {
                quote._internalPrice = internalPrice;
            }
            if (quote._externalSize == 0) {
                if (canStartQuotingAskExternally(askBook)) {
                    quote._externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
                    quote._externalSize = getAskExternalQuoteSize();
                }
            } else {
                if (canContinueQuotingAskExternally(askBook)) {
                    quote._externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
                } else {
                    quote._externalPrice =  hf::NULL_PRICE;
                    quote._externalSize = 0;
                }
            }
            _executor->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
        }

        _executor->matchOrders();
        _executor->synchronizeOrders();
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

    void onFill(Order* order, hf::price_t price, hf::qty_t size, bool internal, Book& bidBook, Book& askBook) {
        bool isHedgingFill = order->_type == Order::Type::Hedging;

        _logger.logInfo(boost::str(boost::format("Market maker fill. Contract=%1% Hedging=%2% Price=%3% Qty=%4% Internal=%5%") % _contract._name % isHedgingFill % price % size % internal));

        if (isHedgingFill) {
            Hedge& hedge = _hedges[order->_side];

            if (size <= hedge._size) {
                hedge._size -= size;
            } else {
                hedge._size = 0;
                _logger.logError("Market maker Received fill where fill size is more than hedge has.");
            }

            if (order->_side == BID) {
                hf::qty_t internallyAllocatedPartOfTheFill = 0;
                if (_internallyInitiatedPosition < 0) {
                    internallyAllocatedPartOfTheFill = std::min(-_internallyInitiatedPosition, size);
                    _internallyInitiatedPosition += internallyAllocatedPartOfTheFill;
                }
                const hf::qty_t externallyAllocatedPartOfTheFill = size - internallyAllocatedPartOfTheFill;
                _externallyInitiatedPosition += externallyAllocatedPartOfTheFill;
            }
            else {
                hf::qty_t internallyAllocatedPartOfTheFill = 0;
                if (_internallyInitiatedPosition > 0) {
                    internallyAllocatedPartOfTheFill = std::min(_internallyInitiatedPosition, size);
                    _internallyInitiatedPosition -= internallyAllocatedPartOfTheFill;
                }
                const hf::qty_t externallyAllocatedPartOfTheFill = size - internallyAllocatedPartOfTheFill;
                _externallyInitiatedPosition -= externallyAllocatedPartOfTheFill;
            }

            _logger.logInfo(boost::str(boost::format("Market maker fill. Contract=%1% InternalPosition=%2% ExternalPosition=%3%") % _contract._name % _internallyInitiatedPosition % _externallyInitiatedPosition));
        } else {
            Quote& quote = _quotes[order->_side];
            if (internal) {
                if (size <= quote._internalSize) {
                    quote._internalSize -= size;
                } else {
                    _logger.logError("Market maker received internal fill where fill size is more than quote has.");
                    quote._internalSize = 0;
                }

                if (order->_side == BID) {
                    _internallyInitiatedPosition += size;
                } else {
                    _internallyInitiatedPosition -= size;
                }
            } else {
                if (size <= quote._externalSize) {
                    quote._externalSize -= size;
                } else {
                    _logger.logError("Market maker received external fill where fill size is more than quote has.");
                    quote._externalSize = 0;
                }

                if (order->_side == BID) {
                    _externallyInitiatedPosition += size;
                } else {
                    _externallyInitiatedPosition -= size;
                }
            }

            _logger.logInfo(boost::str(boost::format("Market maker fill. Contract=%1% InternalPosition=%2% ExternalPosition=%3%") % _contract._name % _internallyInitiatedPosition % _externallyInitiatedPosition));

            hedgeFill(size, order->_side, bidBook, askBook);
        }
    }

    void hedgeFill(hf::qty_t size, uint32_t side, Book& bidBook, Book& askBook) {
        const uint32_t otherSide = GetOtherSide(side);
        Hedge& hedge = _hedges[otherSide];

        if (otherSide == BID) {
            calculateBidHedgePrices(hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
        } else {
            calculateAskHedgePrices(hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
        }

        hedge._size += size;

        if (otherSide == BID) {
            _executor->modifyHedgeBidOrder(hedge._id, hedge._internalPrice, hedge._size, hedge._externalPrice, hedge._size);
        } else {
            _executor->modifyHedgeAskOrder(hedge._id, hedge._internalPrice, hedge._size, hedge._externalPrice, hedge._size);
        }
    }

    void calculateBidHedgePrices(hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);
        internalPrice = midPoint + _commission;
        externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
    }

    void calculateAskHedgePrices(hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);
        internalPrice = midPoint - _commission;
        externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
    }

    void onInternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook) {
        onFill(order, price, size, true, bidBook, askBook);
    }

    void onExternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook) {
        onFill(order, price, size, false, bidBook, askBook);
    }

    void onEndOfExternalFills(uint32_t side) {
        calculateBidQuoteSizes();
        calculateAskQuoteSizes();
        _executor->matchOrders();
        _executor->synchronizeOrders();
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
                tryStartQuotingBid(bidBook, midPoint);
                tryStartQuotingAsk(askBook, midPoint);
                _executor->matchOrders();
                _executor->synchronizeOrders();
            }
        }
    }

    void tryStartQuotingBid(Book& bidBook, hf::price_t midPoint) {
        const hf::price_t internalPrice = midPoint - _width - _commission;
        const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
        Quote& quote = _quotes[BID];
        tryStartQuotingBid(quote, bidBook, internalPrice, externalPrice);
    }

    void tryStartQuotingAsk(Book& askBook, hf::price_t midPoint) {
        const hf::price_t internalPrice = midPoint + _width + _commission;
        const hf::price_t externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
        Quote& quote = _quotes[ASK];
        tryStartQuotingAsk(quote, askBook, internalPrice, externalPrice);
    }

    void disableQuotingExternally() {
        if (_toMarketMake) {
            _quoteExternal = false;
            if (_enabled) {
                auto& state = _executor->_executor->_contractStates[_contract._internalId];
                onChangeInOtherBooks(state._books[BID], state._books[ASK], true, true);
            }
        }
    }

    void start(Book& bidBook, Book& askBook) {
        if (_toMarketMake) {
            Quote& bidQuote = _quotes[BID];
            bidQuote._internalSize = getBidInternalQuoteSize();
            Quote& askQuote = _quotes[BID];
            askQuote._internalSize = getAskInternalQuoteSize();
            onChangeInOtherBooks(bidBook, askBook, true, true);
        }
    }

    bool disable() {
        if (_toMarketMake) {
            _executor->getMarketMakerOutOfMarket();
            _quotes[BID]._internalSize = 0;
            _quotes[ASK]._internalSize = 0;
            return true;
        }
        return false;
    }

    void handleParametersUpdate(const proto::GridCell& gridCell) {
        MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(gridCell.col());
        switch (columnType) {
            case MarketMakerParametersColumns::QuoteExternal: {
                bool quoteExternal = 1 == atoi(gridCell.value().c_str());
                if (quoteExternal != _quoteExternal) {
                    _quoteExternal = quoteExternal;
                    _logger.logInfo(boost::str(boost::format("Quote external changed. Contract=%1% Value=%2%") % _contract._name % quoteExternal));
                    if (quoteExternal) {
                        auto& state = _executor->_executor->_contractStates[_contract._internalId];
                        enableQuoting(state._books[BID], state._books[ASK]);
                    } else {
                        disableQuotingExternally();
                    }
                }
                break;
            }
            case MarketMakerParametersColumns::QuoteSize: {
                _quoteSize = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Quote size changed. Contract=%1% Value=%2%") % _contract._name % _quoteSize));
                rePriceAfterParamChange();
                break;
            }
            case MarketMakerParametersColumns::JoinSize: {
                _joinSize = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Join size changed. Contract=%1% Value=%2%") % _contract._name % _joinSize));
                auto& state = _executor->_executor->_contractStates[_contract._internalId];
                const hf::price_t midPoint = calculateMidPoint(state._books[BID], state._books[ASK]);
                if (!isQuotingBid()) {
                    tryStartQuotingBid(state._books[BID], midPoint);
                }
                if (!isQuotingAsk()) {
                    tryStartQuotingAsk(state._books[ASK], midPoint);
                }
                _executor->matchOrders();
                _executor->synchronizeOrders();
                break;
            }
            case MarketMakerParametersColumns::LeaveSize: {
                _leaveSize = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Leave size changed. Contract=%1% Value=%2%") % _contract._name % _leaveSize));
                auto& state = _executor->_executor->_contractStates[_contract._internalId];
                if (isQuotingBid()) {
                    if (!canContinueQuotingBid(state._books[BID])) {
                        Quote& quote = _quotes[BID];
                        stopQuotingBid(quote);
                    }
                }
                if (isQuotingAsk()) {
                    if (!canContinueQuotingAsk(state._books[ASK])) {
                        Quote& quote = _quotes[ASK];
                        stopQuotingAsk(quote);
                    }
                }
                _executor->synchronizeOrders();
                break;
            }
            case MarketMakerParametersColumns::MaxPosInterrnal: {
                _maxInternallyInitiatedPosition = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker MaxPosInternal changed. Contract=%1% Value=%2%") % _contract._name % _maxInternallyInitiatedPosition));
                rePriceAfterParamChange();
                break;
            }
            case MarketMakerParametersColumns::MaxPosExternal: {
                _maxExternallyInitiatedPosition = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker MaxPosExternal changed. Contract=%1% Value=%2%") % _contract._name % _maxExternallyInitiatedPosition));
                rePriceAfterParamChange();
                break;
            }
            case MarketMakerParametersColumns::PayUpOrders: {
                _payUpOrders = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker PayUpOrders changed. Contract=%1% Value=%2%") % _contract._name % _payUpOrders));
                break;
            }
            case MarketMakerParametersColumns::PayUpQty: {
                _payUpQuantity = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker PayUpQty changed. Contract=%1% Value=%2%") % _contract._name % _payUpQuantity));
                break;
            }
            case MarketMakerParametersColumns::PayUpMultiple: {
                _payUpMultiple = atof(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker PayUpMultiple changed. Contract=%1% Value=%2%") % _contract._name % _payUpMultiple));
                break;
            }
            case MarketMakerParametersColumns::Width: {
                _width = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                _logger.logInfo(boost::str(boost::format("Market maker Width changed. Contract=%1% Value=%2%") % _contract._name % _width));
                rePriceAfterParamChange();
                break;
            }
            case MarketMakerParametersColumns::Commission: {
                _commission = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                _logger.logInfo(boost::str(boost::format("Market maker Commission changed. Contract=%1% Value=%2%") % _contract._name % _commission));
                rePriceAfterParamChange();
                break;
            }
            case MarketMakerParametersColumns::Enabled: {
                bool enabled = 1 == atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Enabled changed. Contract=%1% Value=%2%") % _contract._name % enabled));
                if (enabled != _enabled) {
                    _enabled = enabled;
                    if (enabled) {
                        auto& state = _executor->_executor->_contractStates[_contract._internalId];
                        start(state._books[BID], state._books[ASK]);
                    } else {
                        disable();
                    }
                }
                break;
            }
            default:
                BOOST_ASSERT(false);
                break;
        }
    }

    void rePriceAfterParamChange() {
        auto& state = _executor->_executor->_contractStates[_contract._internalId];
        const hf::price_t midPoint = calculateMidPoint(state._books[BID], state._books[ASK]);
        rePriceBidQuoteAfterParamChange(midPoint);
        rePriceAskQuoteAfterParamChange(midPoint);
        _executor->synchronizeOrders();
    }

    void rePriceBidQuoteAfterParamChange(hf::price_t midPoint) {
        if (isQuotingBid()) {
            Quote& quote = _quotes[BID];
            quote._internalSize = getBidInternalQuoteSize();
            quote._externalSize = getBidExternalQuoteSize();
            if (quote._internalSize == 0) {
                stopQuotingBid(quote);
            } else {
                quote._internalPrice = midPoint - _width - _commission;
                quote._externalPrice = RoundToTickSize(quote._internalPrice, _tickSize, BID);
                _executor->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
            }
        }
    }

    void rePriceAskQuoteAfterParamChange(hf::price_t midPoint) {
        if (isQuotingAsk()) {
            Quote& quote = _quotes[ASK];
            quote._externalSize = getAskExternalQuoteSize();
            quote._internalSize = getAskInternalQuoteSize();
            if (quote._internalSize == 0) {
                stopQuotingAsk(quote);
            } else {
                quote._internalPrice = midPoint + _width + _commission;
                quote._externalPrice = RoundToTickSize(quote._internalPrice, _tickSize, ASK);
                _executor->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize);
            }
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
