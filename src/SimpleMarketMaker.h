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

        void reset() {
            _internalPrice = hf::NULL_PRICE;
            _internalSize = 0;
            _externalPrice = hf::NULL_PRICE;
            _externalSize = 0;
        }
    };

    struct Hedge {
        OrderId_t _id = ORDER_ID_MM_HEDGE;
        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::qty_t _sizeToQuote = 0;

        void reset() {
            _externalPrice = hf::NULL_PRICE;
            _internalPrice = hf::NULL_PRICE;
            _sizeToQuote = 0;
        }
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
            recalculateSizesAndApplyUpdate();
            return sizeTaken;
        } else {
            _externallyInitiatedPosition = newPosition;
            LOG(0, "Market maker {C[i1]}. Took the whole overfill. Took={i2}", getContractName(), overFillSize)
            recalculateSizesAndApplyUpdate();
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

    bool hasMarketData(Book& bidBook, Book& askBook) {
        const bool missingMarketData = ((bidBook._price == hf::NULL_PRICE) | (askBook._price == hf::NULL_PRICE));
        if (BOOST_UNLIKELY(missingMarketData)) {
            _executor->sendErrorMessage(boost::str(boost::format("Re-quoting. Not having market data so turning market maker off. %1%") % _contract._name), ErrorType::Critical);
            LOG(0, "Market maker {C[i1]}. Lost market data. Disabling.", getContractName())
            disable();
            return false;
        }
        return true;
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool bidChanged, bool askChanged, bool forceUpdate = false) {
        LOG(0, "Market maker {C[i1]}. Recalculate. Bid: {i2}@{d1} Ask={i3}@{d2}", getContractName(), bidBook._otherQuantity, ToDollars(bidBook._price), askBook._otherQuantity, ToDollars(askBook._price))

        if (BOOST_UNLIKELY(!isEnabled())) {
            return;
        }

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }

        _midPoint = calculateMidPoint(bidBook, askBook);
        LOG(0, "Market maker {C[i1]}. Midpoint. Midpoint={d1}", getContractName(), _midPoint)

        bool somethingChanged = false;

        if (isHedgingAsk()) {
            Hedge& hedge = _hedges[ASK];
            if (isAskInPayUp(hedge._sizeToQuote, bidBook)) {
                const hf::price_t newAskPrice = bidBook._price;
                if (hedge._externalPrice != newAskPrice) {
                    hedge._externalPrice = newAskPrice;
                    LOG(0, "Market maker {C[i1]}. ASK Paying up. Price={d1} Size={i2}", getContractName(), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    _executor->modifyHedgeAskOrder(hedge._id, hedge._externalPrice - _commission, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, true);
                    _payUpAdjustment -= hedge._sizeToQuote;
                    hedge._sizeToQuote = 0;
                    hedge._internalPrice = hf::NULL_PRICE;
                    somethingChanged = true;
                }
            } else {
                const hf::price_t internalPrice = _midPoint;
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
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
                    _payUpAdjustment += hedge._sizeToQuote;
                    hedge._sizeToQuote = 0;
                    hedge._internalPrice = hf::NULL_PRICE;
                    somethingChanged = true;
                }
            }
            else {
                hf::price_t internalPrice = _midPoint;
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = RoundToTickSize(internalPrice - _commission, _tickSize, BID);
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
            const bool isBidBookInPayUpMode = isAskInPayUp(getBidExternalQuoteSize(), bidBook);
            Quote& quote = _quotes[BID];
            bool changeInPrice = false;

            if (!isBidBookInPayUpMode) {
                const hf::price_t internalPrice = _midPoint - width;
                const hf::price_t internalPriceChange = internalPrice - quote._internalPrice;
                changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
                if (changeInPrice) {
                    quote._internalPrice = internalPrice;
                    if (quoteExternally) {
                        quote._externalPrice = RoundToTickSize(quote._internalPrice  - _commission, _tickSize, BID);
                    }
                }
            }
            else {
                const hf::price_t internalPrice = bidBook._price - _commission;
                if (internalPrice != quote._internalPrice) {
                    quote._internalPrice = internalPrice;
                    if (quoteExternally) {
                        quote._externalPrice = RoundToTickSize(quote._internalPrice  - _commission, _tickSize, BID);
                    }
                    changeInPrice = true;
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
            const bool isAskBookInPayUpMode = isBidInPayUp(getAskExternalQuoteSize(), askBook);
            Quote& quote = _quotes[ASK];
            bool changeInPrice = false;

            if (!isAskBookInPayUpMode) {
                const hf::price_t internalPrice = _midPoint + width;
                const hf::price_t internalPriceChange = internalPrice - quote._internalPrice;
                changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
                if (changeInPrice) {
                    quote._internalPrice = internalPrice;
                    if (quoteExternally) {
                        quote._externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
                    }
                }
            }
            else {
                const hf::price_t internalPrice = askBook._price + _commission;
                if (internalPrice != quote._internalPrice) {
                    quote._internalPrice = internalPrice;
                    if (quoteExternally) {
                        quote._externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
                    }
                    changeInPrice = true;
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
            _executor->executeChanges();
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
        const uint32_t side = ToViperSide(order.side);
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        LOG(0, "Market maker {C[i1]}. HUP. Side={i2} OrderId={i3} Size={i4} Filled={i5} Unfilled={i6}", getContractName(), side, order.oid, order.qty, order.executedQty, unfilledSize)
        LOG(0, "Market maker{C[i1]}. HUP. Before. InternalPosition={i2} ExternalPosition={i3} PayUpAdjustment={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, _payUpAdjustment)
        allocateHedgingFill(side, order.executedQty);
        changePayUpAdjustment(side, order.qty);
        LOG(0, "Market maker{C[i1]}. HUP. After allocation. InternalPosition={i2} ExternalPosition={i3} PayUpAdjustment={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, _payUpAdjustment)
        if (unfilledSize > 0) {
            _executor->returnOrderToHedgeManager(order, side);
            auto& state = _executor->_executor->_contractStates[_contract._internalId];
            if (BOOST_UNLIKELY(!hasMarketData(state._books[BID], state._books[ASK]))) {
                return;
            }
            modifyHedgeQuote(state._books[BID], state._books[ASK]);
        }
        calculateBidQuoteSizes();
        calculateAskQuoteSizes();
        _executor->executeChanges();
        LOG(0, "Market maker{C[i1]}. HUP. End. InternalPosition={i2} ExternalPosition={i3} PayUpAdjustment={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, _payUpAdjustment)
    }

    void allocateHedgingFill(uint32_t side, hf::qty_t size) {
        if (side == BID) {
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
    }

    void onFill(Order* order, hf::price_t price, hf::qty_t size, bool internal, Book& bidBook, Book& askBook, bool isPayUp) {
        bool isHedgingFill = order->_type == Order::Type::Hedging;
        LOG(0, "Market maker {C[i1]}. Fill. Hedging={i2} Price={d1} Qty={i3} Internal={i4} Side={i5}", getContractName(), isHedgingFill, ToDollars(price), size, internal, order->_side)
        if (!isHedgingFill) {
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
        }
        else {
            if (isPayUp) {
                if (internal) {
                    changePayUpAdjustment(order->_side, size);
                    allocateHedgingFill(order->_side, size);
                    LOG(0, "Market maker{C[i1]}. Adjusted payUpAdjustment after payup internal fill. InternalPosition={i2} ExternalPosition={i3} PayUpAdjustment={i4}",
                                                     getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, _payUpAdjustment)
                }
            } else {
                allocateHedgingFill(order->_side, size);
            }
        }
    }

    void changePayUpAdjustment(uint32_t side, hf::qty_t size) {
        _payUpAdjustment += side == BID ? -size : size;
    }

    hf::qty_t calculateHedgeQuoteSize() {
        return -(_internallyInitiatedPosition + _externallyInitiatedPosition + _payUpAdjustment);
    }

    void modifyHedgeQuote(Book& bidBook, Book& askBook) {
        hf::qty_t quoteSize = calculateHedgeQuoteSize();
        LOG(0, "Market maker{C[i1]}. Modifying hedge quote. Before. InternalPosition={i2} ExternalPosition={i3} PayUpAdjustment={i4} QuoteSize={i5}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, _payUpAdjustment, quoteSize)
        if (quoteSize > 0) {
            Hedge& hedge = _hedges[BID];
            hedge._sizeToQuote = quoteSize;
            const bool isInPayUp = calculateBidHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
            LOG(0, "Market maker{C[i1]}. BID Hedging fill. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
            _executor->modifyHedgeBidOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, isInPayUp);
            if (isInPayUp) {
                _payUpAdjustment += hedge._sizeToQuote;
                hedge._sizeToQuote = 0;
                hedge._internalPrice = hf::NULL_PRICE;
            }
            Hedge& otherHedge = _hedges[ASK];
            if (otherHedge._sizeToQuote > 0) {
                _executor->modifyHedgeAskOrder(otherHedge._id, hf::NULL_PRICE, 0, otherHedge._externalPrice, 0, false);
                otherHedge._sizeToQuote = 0;
                otherHedge._externalPrice = hf::NULL_PRICE;
                otherHedge._internalPrice = hf::NULL_PRICE;
            }
        }
        else if (quoteSize < 0) {
            quoteSize = -quoteSize;
            Hedge& hedge = _hedges[ASK];
            hedge._sizeToQuote = quoteSize;
            const bool isInPayUp = calculateAskHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
            LOG(0, "Market maker {C[i1]}. ASK Hedging fill. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
            _executor->modifyHedgeAskOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, isInPayUp);
            if (isInPayUp) {
                _payUpAdjustment -= hedge._sizeToQuote;
                hedge._sizeToQuote = 0;
                hedge._internalPrice = hf::NULL_PRICE;
            }
            Hedge& otherHedge = _hedges[BID];
            if (otherHedge._sizeToQuote > 0) {
                _executor->modifyHedgeBidOrder(otherHedge._id, hf::NULL_PRICE, 0, otherHedge._externalPrice, 0, false);
            }
        }
        else {
            Hedge& bidHedge = _hedges[BID];
            if (bidHedge._sizeToQuote > 0) {
                _executor->modifyHedgeBidOrder(bidHedge._id, hf::NULL_PRICE, 0, bidHedge._externalPrice, 0, false);
                bidHedge._sizeToQuote = 0;
                bidHedge._externalPrice = hf::NULL_PRICE;
                bidHedge._internalPrice = hf::NULL_PRICE;
            }
            Hedge& askHedge = _hedges[ASK];
            if (askHedge._sizeToQuote > 0) {
                _executor->modifyHedgeAskOrder(askHedge._id, hf::NULL_PRICE, 0, askHedge._externalPrice, 0, false);
                askHedge._sizeToQuote = 0;
                askHedge._externalPrice = hf::NULL_PRICE;
                askHedge._internalPrice = hf::NULL_PRICE;
            }
        }

        LOG(0, "Market maker{C[i1]}. Modifying hedge quote. End. InternalPosition={i2} ExternalPosition={i3} PayUpAdjustment={i4} QuoteSize={i5}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, _payUpAdjustment, quoteSize)

        sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_HEDGE, _hedges[BID]._internalPrice, _hedges[BID]._sizeToQuote, _hedges[ASK]._internalPrice, _hedges[ASK]._sizeToQuote);
    }

    bool calculateBidHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const bool isInPayUp = isBidInPayUp(hedgeSize, askBook);
        if (isInPayUp) {
            externalPrice = askBook._price;
            internalPrice = externalPrice + _commission;
        } else {
            _midPoint = calculateMidPoint(bidBook, askBook);
            internalPrice = _midPoint;
            externalPrice = RoundToTickSize(internalPrice - _commission, _tickSize, BID);
        }
        return isInPayUp;
    }

    bool calculateAskHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const bool isInPayUp = isAskInPayUp(hedgeSize, bidBook);
        if (isInPayUp) {
            externalPrice = bidBook._price;
            internalPrice = externalPrice - _commission;
        } else {
            _midPoint = calculateMidPoint(bidBook, askBook);
            internalPrice = _midPoint;
            externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
        }
        return isInPayUp;
    }

    void onInternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook, bool isPayUp) {
        onFill(order, price, size, true, bidBook, askBook, isPayUp);
        _hadInternalFill = true;
    }

    void onExternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook, bool isPayUp) {
        onFill(order, price, size, false, bidBook, askBook, isPayUp);
        _hadExternalFill = true;
    }

    void onEndOfFills() {
        auto& state = _executor->_executor->_contractStates[_contract._internalId];
        auto& bidBook = state._books[BID];
        auto& askBook = state._books[ASK];

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }

        modifyHedgeQuote(bidBook, askBook);
        calculateBidQuoteSizes();
        calculateAskQuoteSizes();

        _executor->executeChanges();
    }

    void onEndOfExternalFills(uint32_t side) {
        if (_hadExternalFill) {
            LOG(0, "Market maker {C[i1]}. Side={i2} End of external fills.", getContractName(), side)
            onEndOfFills();
            _hadExternalFill = false;
        }
    }

    void onEndOInternalFills() {
        if (_hadInternalFill) {
            LOG(0, "Market maker {C[i1]}. End of internal fills.", getContractName())
            onEndOfFills();
            _hadInternalFill = false;
        }
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
                modifyHedgeQuote(state._books[BID], state._books[ASK]);
                onChangeInOtherBooks(state._books[BID], state._books[ASK], true, true , true);
            }
        }
    }

    bool isWithinPositionLimits() {
        bool withinLimits = true;
        if (std::abs(_internallyInitiatedPosition) >= _maxInternallyInitiatedPosition) {
            _executor->sendErrorMessage(boost::str(boost::format("MM - at/over max internally initiated position. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "Market maker {C[i1]}. Internally initiated position is at/over limit", getContractName())
            withinLimits = false;
        }
        if (std::abs(_externallyInitiatedPosition >= _maxExternallyInitiatedPosition)) {
            _executor->sendErrorMessage(boost::str(boost::format("MM - at/over max externally initiated position. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "Market maker {C[i1]}. Externally initiated position is at/over limit", getContractName())
            withinLimits = false;
        }
        return withinLimits;
    }

    bool hasMarketData() {
        auto& state = _executor->_executor->_contractStates[_contract._internalId];
        bool hasMD = ((state._books[BID]._price != hf::NULL_PRICE) & (state._books[ASK]._price != hf::NULL_PRICE));
        if (!hasMD) {
            _executor->sendErrorMessage(boost::str(boost::format("MM - does not have market data. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "Market maker {C[i1]}. does not have market data", getContractName())
        }
        return hasMD;
    }

    bool canStart() {
        if (_enabled) {
            bool canStart = true;
            if (!isWithinPositionLimits()) {
                canStart = false;
            }
            if (!hasMarketData()) {
                canStart = false;
            }
            if (!canStart) {
                disable();
                return false;
            }
            return true;
        }
        return false;
    }

    void start(Book& bidBook, Book& askBook) {
        if (_toMarketMake) {
            if (!canStart()) {
                return;
            }
            Quote& bidQuote = _quotes[BID];
            bidQuote._internalSize = getBidInternalQuoteSize();
            bidQuote._externalSize = 0;
            Quote& askQuote = _quotes[ASK];
            askQuote._internalSize = getAskInternalQuoteSize();
            askQuote._externalSize = 0;
            modifyHedgeQuote(bidBook, askBook);
            onChangeInOtherBooks(bidBook, askBook, true, true, true);
        }
    }

    void disable(bool changeFlag = true) {
        if (_toMarketMake) {
            _executor->getMarketMakerOutOfMarket();
            _quotes[BID].reset();
            _quotes[ASK].reset();
            _hedges[BID].reset();
            _hedges[ASK].reset();
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_QUOTE, _quotes[BID]._internalPrice, _quotes[BID]._internalSize, _quotes[ASK]._internalPrice, _quotes[ASK]._internalSize);
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_HEDGE, _hedges[BID]._internalPrice, _hedges[BID]._sizeToQuote, _hedges[ASK]._internalPrice, _hedges[ASK]._sizeToQuote);
            if (changeFlag) {
                _enabled = false;
                _executor->sendGenericMessageToFront(ViperMessageType::MarketMakerEnabled, _contract._internalId, 0);
            }
        }
    }

    void handleParametersUpdate(const proto::GridCell& gridCell) {
        MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(gridCell.col());
        switch (columnType) {
            case MarketMakerParametersColumns::QuoteExternal: {
                bool quoteExternal = 1 == atoi(gridCell.value().c_str());
                _quoteExternal = quoteExternal;
                LOG(0, "Market maker {C[i1]}. Quote external changed. Value={i2}", getContractName(), quoteExternal)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::QuoteSize: {
                _quoteSize = atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. Quote size changed. Value={i2}", getContractName(), _quoteSize)
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::JoinSize: {
                _joinSize = atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. Join size changed. Value={i2}", getContractName(), _joinSize)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::LeaveSize: {
                _leaveSize = atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. Leave size changed. Value={i2}", getContractName(), _leaveSize)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::MaxPosInterrnal: {
                _maxInternallyInitiatedPosition = atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. MaxPosInternal changed. Value={i2}", getContractName(), _maxInternallyInitiatedPosition)
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::MaxPosExternal: {
                _maxExternallyInitiatedPosition = atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. MaxPosExternal changed. Value={i2}", getContractName(), _maxExternallyInitiatedPosition)
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::PayUpOrders: {
                _payUpOrders = atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. PayUpOrders changed. Value={i2}", getContractName(), _payUpOrders)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::PayUpQty: {
                _payUpQuantity = atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. PayUpQty changed. Value={i2}", getContractName(), _payUpQuantity)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::PayUpMultiple: {
                _payUpMultiple = atof(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. PayUpMultiple changed. Value={d1}", getContractName(), _payUpMultiple)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::Width: {
                _widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "Market maker {C[i1]}. Width percent changed. Value={d1}", getContractName(), _widthPercentage)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::MinInternalPriceChange: {
                _minInternalPriceChange = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                LOG(0, "Market maker {C[i1]}. Min internal price change changed. Value={d1}", getContractName(), ToDollars(_minInternalPriceChange))
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::EstablishedLevel: {
                _establishedLevel = boost::lexical_cast<uint32_t>(gridCell.value());
                LOG(0, "Market maker {C[i1]}. Established level changed. Value={i2}", getContractName(), _establishedLevel)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::Commission: {
                _commission = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                LOG(0, "Market maker {C[i1]}. Commission changed. Value={d1}", getContractName(), _commission)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::Enabled: {
                bool enabled = 1 == atoi(gridCell.value().c_str());
                LOG(0, "Market maker {C[i1]}. Enabled changed. Value={i2}", getContractName(), enabled)
                if (enabled != _enabled) {
                    _enabled = enabled;
                    if (enabled) {
                        if (isEnabled()) {
                            auto& state = _executor->_executor->_contractStates[_contract._internalId];
                            start(state._books[BID], state._books[ASK]);
                        }
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
        calculateBidQuoteSizes();
        calculateAskQuoteSizes();
        auto& state = _executor->_executor->_contractStates[_contract._internalId];
        modifyHedgeQuote(state._books[BID], state._books[ASK]);
        _executor->executeChanges();
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
            _executor->sendErrorMessage(boost::str(boost::format("Market maker %1%. Could not take the whole manual fill. Took=%2%") % _contract._name % sizeTaken), ErrorType::Critical);
            _executor->_executor->sendAcceptedManualFillToFront(_contract._internalId, sizeTaken);
            LOG(0, "Market maker {C[i1]}. Could not take the whole manual fill. Took={i2}", getContractName(), sizeTaken)
        } else {
            _internallyInitiatedPosition = newPosition;
            _executor->_executor->sendAcceptedManualFillToFront(_contract._internalId, size);
            LOG(0, "Market maker {C[i1]}. Took the whole manual fill. Took={i2} InternallyInitiatedPosition={i3}", getContractName(), size, _internallyInitiatedPosition)
        }
        recalculateSizesAndApplyUpdate();
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
    hf::qty_t _payUpAdjustment = 0;
    Hedge _hedges[2];
    Quote _quotes[2];
    ExecutorT* _executor;
    ConfigContract _contract;
    bool _toMarketMake = false;
    hf::price_t _midPoint = 0;
    bool _hadInternalFill = false;
    bool _hadExternalFill = false;
    Logger _logger;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
