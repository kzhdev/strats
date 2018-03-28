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
        hf::qty_t _sizeToQuote = 0;
        hf::qty_t  _sizeInPayUp = 0;
    };

    void initialize(ConfigContract& contract, MarketMakerConfig& config, ExecutorT* executor) {
        _enabled = config._enabled;
        _quoteExternal = config._quoteExternal;
        _quoteSize = config._quoteSize;
        _widthPercentage = config._widthPercentage;
        _joinSize = config._joinSize;
        _leaveSize = config._leaveSize;
        _payUpQuantity = config._payUpQty;
        _payUpOrders = config._payUpOrders;
        _payUpMultiple = config._payUpMultiple;
        _maxExternallyInitiatedPosition = config._maxPosExternal;
        _maxInternallyInitiatedPosition = config._maxPosInternal;
        _commission = ToRhonePrice(config._commission);
        _establishedLevel = config._establishedLevel;
        _minInternalPriceChange = ToRhonePrice(config._minInternalPriceChange);
        _executor = executor;
        _contract = contract;
        _tickSize = contract._tickSize;
        _toMarketMake = true;
    }

    hf::qty_t takeOverFill(uint32_t side, hf::qty_t overFillSize) {
        LOG(0, "Market maker {C[i1]}. Take overfill. Size={i2}", getContractName(), overFillSize)
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        const hf::qty_t newPosition = overFillSize * multiplier + _externallyInitiatedPosition;
        if (std::abs(newPosition) > _maxExternallyInitiatedPosition) {
            hf::qty_t sizeTaken = _maxExternallyInitiatedPosition - std::abs(_externallyInitiatedPosition);
            _externallyInitiatedPosition += sizeTaken * multiplier;
            LOG(0, "Market maker {C[i1]}. Could not take the whole overfill. Took={i2}", getContractName(), sizeTaken)
            return sizeTaken;
        } else {
            _externallyInitiatedPosition = newPosition;
            LOG(0, "Market maker {C[i1]}. Took the whole overfill. Took={i2}", getContractName(), overFillSize)
            return overFillSize;
        }
    }

    bool isHedgingAsk() {
        return _hedges[ASK]._sizeToQuote != 0;
    }

    bool isHedgingBid() {
        return _hedges[BID]._sizeToQuote != 0;
    }

    bool isThereEnoughSizeToJoinForBid(Book& bidBook) {
        return bidBook._otherQuantity >= _joinSize;
    }

    bool isThereEnoughSizeToStayForBid(Book& bidBook) {
        return bidBook._otherQuantity >= _leaveSize;
    }

    bool isThereEnoughSizeToJoinForAsk(Book& askBook) {
        return askBook._otherQuantity >= _joinSize;
    }

    bool isThereEnoughSizeToStayForAsk(Book& askBook) {
        return askBook._otherQuantity >= _leaveSize;
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

    hf::qty_t getBidInternalQuoteSize() {
        if (_internallyInitiatedPosition < 0) {
            return _maxInternallyInitiatedPosition;
        }
        if (_maxInternallyInitiatedPosition <= _internallyInitiatedPosition) {
            return 0;
        }
        return _maxInternallyInitiatedPosition - _internallyInitiatedPosition;
    }

    hf::qty_t getAskInternalQuoteSize() {
        if (_internallyInitiatedPosition > 0) {
            return _maxInternallyInitiatedPosition;
        }
        if (_maxInternallyInitiatedPosition <= -_internallyInitiatedPosition) {
            return 0;
        }
        return _maxInternallyInitiatedPosition + _internallyInitiatedPosition;
    }

    bool isAskInPayUp(hf::qty_t hedgeSize, Book& bidBook) {
        const hf::qty_t payUpBasedOnHedgeSize = static_cast<hf::qty_t>(hedgeSize * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnHedgeSize);
        const bool shouldPayup = (bidBook._otherQuantity <= payupQuantity) | (bidBook._otherOrderCount <= _payUpOrders);
        return shouldPayup;
    }

    bool isBidInPayUp(hf::qty_t hedgeSize, Book& askBook) {
        const hf::qty_t payUpBasedOnHedgeSize = static_cast<hf::qty_t>(hedgeSize * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnHedgeSize);
        const bool shouldPayup = (askBook._otherQuantity <= payupQuantity) | (askBook._otherOrderCount <= _payUpOrders);
        return shouldPayup;
    }

    bool isEnabled() {
        return _enabled & _executor->_executor->isEnabled() & canContractTrade();
    }

    bool canQuoteExternally() {
        return _quoteExternal & _executor->_executor->canQuoteExternally();
    }

    bool canContractTrade() {
        return _executor->_contractCanTrade;
    }

    void onGlobalQuoteExternallyChanged() {
        applyUpdates();
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged, bool forceUpdate = false) {
        if (!isEnabled())
            return;

        LOG(0, "Market maker {C[i1]}. Recalculate. Bid: {i2}@{d1} Ask={i3}@{d2}", getContractName(), bidBook._otherQuantity, ToDollars(bidBook._price), askBook._otherQuantity, ToDollars(askBook._price))

        const bool missingMarketData = ((bidBook._price == hf::NULL_PRICE) | (askBook._price == hf::NULL_PRICE));
        if (missingMarketData) {
            _enabled = false;
            _executor->sendGenericMessageToFront(ViperMessageType::MarketMakerEnabled, _contract._internalId, 0);
            std::string message = boost::str(boost::format("Not having market data so turning market maker off. %1%") % _contract._name);
            _executor->sendErrorMessage(message, ErrorType::Critical);
            return;
        }

        const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);
        LOG(0, "Market maker {C[i1]}. Midpoint. Midpoint={d1}", getContractName(), midPoint)

        bool somethingChanged = false;

        if (isHedgingAsk()) {
            Hedge& hedge = _hedges[ASK];
            if (isAskInPayUp(hedge._sizeToQuote, bidBook)) {
                const hf::price_t newAskPrice = bidBook._price;
                if (hedge._externalPrice != newAskPrice) {
                    hedge._externalPrice = newAskPrice;
                    LOG(0, "Market maker {C[i1]}. ASK Paying up. Price={d1} Size={i2}", getContractName(), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    _executor->modifyHedgeAskOrder(hedge._id, hedge._externalPrice - _commission, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, true);
                    hedge._sizeInPayUp += hedge._sizeToQuote;
                    hedge._sizeToQuote = 0;
                    hedge._internalPrice = hf::NULL_PRICE;
                    somethingChanged = true;
                }
            } else {
                const hf::price_t internalPrice = midPoint + _commission;
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
                    LOG(0, "Market maker {C[i1]}. ASK Repriced hedge. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    _executor->modifyHedgeAskOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, false);
                    somethingChanged = true;
                }
            }
        }

        if (isHedgingBid()) {
            Hedge& hedge = _hedges[BID];
            if (isBidInPayUp(hedge._sizeToQuote, askBook)) {
                const hf::price_t newBidPrice = askBook._price;
                if (hedge._externalPrice != newBidPrice) {
                    hedge._externalPrice = newBidPrice;
                    LOG(0, "Market maker {C[i1]}. BID Paying up. Price={d1} Size={i2}", getContractName(), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    _executor->modifyHedgeBidOrder(hedge._id, hedge._externalPrice + _commission, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, true);
                    hedge._sizeInPayUp += hedge._sizeToQuote;
                    hedge._sizeToQuote = 0;
                    hedge._internalPrice = hf::NULL_PRICE;
                    somethingChanged = true;
                }
            }
            else {
                hf::price_t internalPrice = midPoint - _commission;
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
                    LOG(0, "Market maker {C[i1]}. BID Repriced hedge. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    _executor->modifyHedgeBidOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, false);
                    somethingChanged = true;
                }
            }
        }

        const bool quoteExternally = canQuoteExternally();
        const bool persistPrice = quoteExternally;
        const double marketSpread = askBook._price - bidBook._price;
        const hf::price_t width = static_cast<hf::price_t>(marketSpread / 2.0 * _widthPercentage);

        {
            const hf::price_t internalPrice = midPoint - width - _commission;
            Quote& quote = _quotes[BID];

            const hf::price_t internalPriceChange = internalPrice - quote._internalPrice;
            const bool changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
            if (changeInPrice) {
                quote._internalPrice = internalPrice;
                if (quoteExternally) {
                    quote._externalPrice = RoundToTickSize(quote._internalPrice, _tickSize, BID);
                }
            }

            bool changeInSize = false;
            if (quoteExternally) {
                if (quote._externalSize == 0) {
                    if (isThereEnoughSizeToJoinForBid(bidBook)) {
                        quote._externalSize = getBidExternalQuoteSize();
                        changeInSize = true;
                    }
                } else {
                    if (!isThereEnoughSizeToStayForBid(bidBook)) {
                        quote._externalSize = 0;
                        changeInSize = true;
                    }
                }
            } else {
                changeInSize |= quote._externalSize != 0;
                quote._externalSize = 0;
            }

            if (changeInPrice | changeInSize | forceUpdate) {
                _executor->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize, persistPrice);
                somethingChanged = true;
            }
        }

        {
            const hf::price_t internalPrice = midPoint + width + _commission;
            Quote& quote = _quotes[ASK];

            const hf::price_t internalPriceChange = internalPrice - quote._internalPrice;
            const bool changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
            if (changeInPrice) {
                quote._internalPrice = internalPrice;
                if (quoteExternally) {
                    quote._externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
                }
            }

            bool changeInSize = false;
            if (quoteExternally) {
                if (quote._externalSize == 0) {
                    if (isThereEnoughSizeToJoinForAsk(askBook)) {
                        quote._externalSize = getAskExternalQuoteSize();
                        changeInSize = true;
                    }
                } else {
                    if (!isThereEnoughSizeToStayForAsk(askBook)) {
                        quote._externalSize = 0;
                        changeInSize = true;
                    }
                }
            } else {
                changeInSize |= quote._externalSize != 0;
                quote._externalSize = 0;
            }

            if (changeInPrice | changeInSize | forceUpdate) {
                _executor->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize, persistPrice);
                somethingChanged = true;
            }
        }

        if (somethingChanged | forceUpdate) {
            _executor->matchOrders();
            _executor->synchronizeOrders();
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_QUOTE, _quotes[BID]._internalPrice, _quotes[BID]._internalSize, _quotes[ASK]._internalPrice, _quotes[ASK]._internalSize);
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_HEDGE, _hedges[BID]._internalPrice, _hedges[BID]._sizeToQuote, _hedges[ASK]._internalPrice, _hedges[ASK]._sizeToQuote);
        }
    }

    hf::price_t calculateMidPoint(Book& bid, Book& ask) {
        const hf::qty_t askSize = std::min(ask._otherQuantity, _establishedLevel);
        const hf::qty_t bidSize = std::min(bid._otherQuantity, _establishedLevel);
        return (ask._price * bidSize + bid._price * askSize) / (bidSize + askSize);
    }

    void handleUnfilledPayUp(const om::order_t& order) {
        if (order.is_active) {
            hf::qty_t unfilledSize = order.leavesQty;
            uint32_t side = ToViperSide(order.side);
            Hedge& hedge = _hedges[side];
            hedge._sizeInPayUp -= unfilledSize;
            _executor->returnOrderToHedgeManager(order, side);
            auto& state = _executor->_executor->_contractStates[_contract._internalId];
            modifyHedgeQuote(unfilledSize, side, state._books[BID], state._books[ASK]);
        }
    }

    void onFill(Order* order, hf::price_t price, hf::qty_t size, bool internal, Book& bidBook, Book& askBook, bool isPayUp) {
        bool isHedgingFill = order->_type == Order::Type::Hedging;

        LOG(0, "Market maker {C[i1]}. Fill. Hedging={i2} Price={d1} Qty={i3} Internal={i4}", getContractName(), isHedgingFill, ToDollars(price), size, internal)

        if (isHedgingFill) {
            Hedge& hedge = _hedges[order->_side];

           if (isPayUp) {
               hedge._sizeInPayUp -= size;
           } else {
               if (size <= hedge._sizeToQuote) {
                   hedge._sizeToQuote -= size;
               } else {
                   hedge._sizeToQuote = 0;
                   _logger.logError("Market maker Received fill where fill size is more than hedge has.");
               }
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

            LOG(0, "Market maker {C[i1]}. Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
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

            LOG(0, "Market maker {C[i1]}. Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)

            hedgeFill(size, order->_side, bidBook, askBook);
        }
    }

    void hedgeFill(hf::qty_t size, uint32_t side, Book& bidBook, Book& askBook) {
        const uint32_t otherSide = GetOtherSide(side);
        modifyHedgeQuote(size, otherSide, bidBook, askBook);
    }

    void modifyHedgeQuote(hf::qty_t size, uint32_t side, Book& bidBook, Book& askBook) {
        Hedge& hedge = _hedges[side];

        hedge._sizeToQuote += size;

        bool isInPayUp;
        if (side == BID) {
            isInPayUp = calculateBidHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
        } else {
            isInPayUp = calculateAskHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
        }

        if (side == BID) {
            _logger.logInfo(boost::str(boost::format("Market maker. Contract=%1% BID Hedging fill. IntPrice=%2% ExtPrice=%3% Size=%4%") % _contract._name % hedge._internalPrice % hedge._externalPrice % hedge._sizeToQuote));
            _executor->modifyHedgeBidOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, isInPayUp);
        } else {
            _logger.logInfo(boost::str(boost::format("Market maker. Contract=%1% ASK Hedging fill. IntPrice=%2% ExtPrice=%3% Size=%4%") % _contract._name % hedge._internalPrice % hedge._externalPrice % hedge._sizeToQuote));
            _executor->modifyHedgeAskOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, isInPayUp);
        }

        if (isInPayUp) {
            hedge._sizeInPayUp += hedge._sizeToQuote;
            hedge._sizeToQuote = 0;
            hedge._internalPrice = hf::NULL_PRICE;
        }

        sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_HEDGE, _hedges[BID]._internalPrice, _hedges[BID]._sizeToQuote, _hedges[ASK]._internalPrice, _hedges[ASK]._sizeToQuote);
    }

    bool calculateBidHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const bool isInPayUp = isBidInPayUp(hedgeSize, askBook);
        if (isInPayUp) {
            externalPrice = askBook._price;
            internalPrice = externalPrice + _commission;
        } else {
            const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);
            internalPrice = midPoint + _commission;
            externalPrice = RoundToTickSize(internalPrice, _tickSize, BID);
        }
        return isInPayUp;
    }

    bool calculateAskHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const bool isInPayUp = isAskInPayUp(hedgeSize, bidBook);
        if (isInPayUp) {
            externalPrice = bidBook._price;
            internalPrice = externalPrice - _commission;
        } else {
            const hf::price_t midPoint = calculateMidPoint(bidBook, askBook);
            internalPrice = midPoint - _commission;
            externalPrice = RoundToTickSize(internalPrice, _tickSize, ASK);
        }
        return isInPayUp;
    }

    void onInternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook, bool isPayUp) {
        onFill(order, price, size, true, bidBook, askBook, isPayUp);
    }

    void onExternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook, bool isPayUp) {
        onFill(order, price, size, false, bidBook, askBook, isPayUp);
    }

    void onEndOfExternalFills(uint32_t side) {
        calculateBidQuoteSizes();
        calculateAskQuoteSizes();
        _executor->matchOrders();
        _executor->synchronizeOrders();
    }

    void calculateBidQuoteSizes() {
        if (!isEnabled()) {
            return;
        }

        bool update = false;
        Quote& quote = _quotes[BID];

        const hf::qty_t internalSize = getBidInternalQuoteSize();
        update |= internalSize != quote._internalSize;

        const bool quoteExternally = canQuoteExternally();
        auto& state = _executor->_executor->_contractStates[_contract._internalId];
        hf::qty_t externalSize = quote._externalSize;
        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSizeToJoinForBid(state._books[BID])) {
                    externalSize = getBidExternalQuoteSize();
                }
            } else {
                if (!isThereEnoughSizeToStayForBid(state._books[BID])) {
                    externalSize = 0;
                } else {
                    externalSize = getBidExternalQuoteSize();
                }
            }
        }

        update |= externalSize != quote._externalSize;

        if (update) {
            quote._internalSize = internalSize;
            quote._externalSize = externalSize;
            _executor->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize, quoteExternally);
        }
    }

    void calculateAskQuoteSizes() {
        if (!isEnabled()) {
            return;
        }

        bool update = false;
        Quote& quote = _quotes[ASK];

        const hf::qty_t internalSize = getAskInternalQuoteSize();
        update |= internalSize != quote._internalSize;

        const bool quoteExternally = canQuoteExternally();
        auto& state = _executor->_executor->_contractStates[_contract._internalId];
        hf::qty_t externalSize = quote._externalSize;

        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSizeToJoinForAsk(state._books[ASK])) {
                    externalSize = getAskExternalQuoteSize();
                }
            } else {
                if (!isThereEnoughSizeToStayForAsk(state._books[ASK])) {
                    externalSize = 0;
                } else {
                    externalSize = getAskExternalQuoteSize();
                }
            }
        }

        update |= externalSize != quote._externalSize;

        if (update) {
            quote._internalSize = internalSize;
            quote._externalSize = externalSize;
            _executor->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize, quote._externalPrice, quote._externalSize, quoteExternally);
        }
    }

    void applyUpdates() {
        if (_toMarketMake) {
            if (_enabled) {
                auto& state = _executor->_executor->_contractStates[_contract._internalId];
                onChangeInOtherBooks(state._books[BID], state._books[ASK], true, true , true);
            }
        }
    }

    void start(Book& bidBook, Book& askBook) {
        if (_toMarketMake) {
            Quote& bidQuote = _quotes[BID];
            bidQuote._internalSize = getBidInternalQuoteSize();
            bidQuote._externalSize = getBidExternalQuoteSize();
            Quote& askQuote = _quotes[ASK];
            askQuote._internalSize = getAskInternalQuoteSize();
            askQuote._externalSize = getAskExternalQuoteSize();
            onChangeInOtherBooks(bidBook, askBook, true, true, true);
        }
    }

    void disable() {
        if (_toMarketMake) {
            _executor->getMarketMakerOutOfMarket();
            _quotes[BID]._internalSize = 0;
            _quotes[BID]._externalSize = 0;
            _quotes[ASK]._internalSize = 0;
            _quotes[ASK]._externalSize = 0;
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_QUOTE, _quotes[BID]._internalPrice, _quotes[BID]._internalSize, _quotes[ASK]._internalPrice, _quotes[ASK]._internalSize);
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_HEDGE, _hedges[BID]._internalPrice, _hedges[BID]._sizeToQuote, _hedges[ASK]._internalPrice, _hedges[ASK]._sizeToQuote);
        }
    }

    void handleParametersUpdate(const proto::GridCell& gridCell) {
        MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(gridCell.col());
        switch (columnType) {
            case MarketMakerParametersColumns::QuoteExternal: {
                bool quoteExternal = 1 == atoi(gridCell.value().c_str());
                _quoteExternal = quoteExternal;
                _logger.logInfo(boost::str(boost::format("Quote external changed. Contract=%1% Value=%2%") % _contract._name % quoteExternal));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::QuoteSize: {
                _quoteSize = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Quote size changed. Contract=%1% Value=%2%") % _contract._name % _quoteSize));
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::JoinSize: {
                _joinSize = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Join size changed. Contract=%1% Value=%2%") % _contract._name % _joinSize));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::LeaveSize: {
                _leaveSize = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Leave size changed. Contract=%1% Value=%2%") % _contract._name % _leaveSize));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::MaxPosInterrnal: {
                _maxInternallyInitiatedPosition = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker MaxPosInternal changed. Contract=%1% Value=%2%") % _contract._name % _maxInternallyInitiatedPosition));
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::MaxPosExternal: {
                _maxExternallyInitiatedPosition = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker MaxPosExternal changed. Contract=%1% Value=%2%") % _contract._name % _maxExternallyInitiatedPosition));
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::PayUpOrders: {
                _payUpOrders = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker PayUpOrders changed. Contract=%1% Value=%2%") % _contract._name % _payUpOrders));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::PayUpQty: {
                _payUpQuantity = atoi(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker PayUpQty changed. Contract=%1% Value=%2%") % _contract._name % _payUpQuantity));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::PayUpMultiple: {
                _payUpMultiple = atof(gridCell.value().c_str());
                _logger.logInfo(boost::str(boost::format("Market maker PayUpMultiple changed. Contract=%1% Value=%2%") % _contract._name % _payUpMultiple));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::Width: {
                _widthPercentage = boost::lexical_cast<double>(gridCell.value());
                _logger.logInfo(boost::str(boost::format("Market maker Width percent changed. Contract=%1% Value=%2%") % _contract._name % _widthPercentage));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::MinInternalPriceChange: {
                _minInternalPriceChange = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                _logger.logInfo(boost::str(boost::format("Market maker Min internal price change changed. Contract=%1% Value=%2%") % _contract._name % ToDollars(_minInternalPriceChange)));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::EstablishedLevel: {
                _establishedLevel = boost::lexical_cast<uint32_t>(gridCell.value());
                _logger.logInfo(boost::str(boost::format("Market maker Established level changed. Contract=%1% Value=%2%") % _contract._name % _establishedLevel));
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::Commission: {
                _commission = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                _logger.logInfo(boost::str(boost::format("Market maker Commission changed. Contract=%1% Value=%2%") % _contract._name % _commission));
                applyUpdates();
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

    void recalculateSizesAndApplyUpdate() {
        Quote& bidQuote = _quotes[BID];
        bidQuote._internalSize = getBidInternalQuoteSize();
        bidQuote._externalSize = getBidExternalQuoteSize();
        Quote& askQuote = _quotes[ASK];
        askQuote._internalSize = getBidInternalQuoteSize();
        askQuote._externalSize = getBidExternalQuoteSize();
        applyUpdates();
    }

    bool validateParametersUpdate(const proto::GridCell& gridCell) {
        return true;
    }

    bool handleLogin() {
        return true;
    }

    void sendMarketUpdateToRelativeValue(uint32_t internalId, OrderIds id, hf::price_t bidPrice, hf::qty_t bidSize, hf::price_t askPrice, hf::qty_t askSize) {
        _executor->_executor->sendMarketUpdateToRelativeValue(internalId, id, bidPrice, bidSize, askPrice, askSize);
    }

    void addManualPosition(hf::qty_t size, uint32_t side) {
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        const hf::qty_t newPosition = size * multiplier + _internallyInitiatedPosition;
        if (std::abs(newPosition) > _maxInternallyInitiatedPosition) {
            hf::qty_t sizeTaken = _maxInternallyInitiatedPosition - std::abs(_internallyInitiatedPosition);
            _internallyInitiatedPosition += sizeTaken * multiplier;
            std::string message = boost::str(boost::format("Market maker. Could not take the whole manual fill. Took=%1%") % sizeTaken);
            _executor->sendErrorMessage(message, ErrorType::Critical);
            _executor->_executor->sendAcceptedManualFillToFront(_contract._internalId, sizeTaken);
            _logger.logInfo(message);
        } else {
            _internallyInitiatedPosition = newPosition;
            _executor->_executor->sendAcceptedManualFillToFront(_contract._internalId, size);
            _logger.logInfo(boost::str(boost::format("Market maker. Took the whole manual fill. Took=%1% InternallyInitiatedPosition=%2%") % size % _internallyInitiatedPosition));
        }
        recalculateSizesAndApplyUpdate();
    }

    Logger& getLogger() {
        return _logger;
    }

#ifdef IS_DEV
    std::string getContractName() {
        return _contract._name;
    }
#else
    uint32_t getContractName() {
            return _contract._internalId;
        }
#endif

    bool _enabled = false;
    bool _quoteExternal;
    hf::qty_t _quoteSize;
    double _widthPercentage;
    hf::price_t _minInternalPriceChange;
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
