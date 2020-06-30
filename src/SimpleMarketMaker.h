#ifndef RHONEOMS_SIMPLE_MARKET_MAKER_H
#define RHONEOMS_SIMPLE_MARKET_MAKER_H

#include <stdint.h>
#include <hf/hf.h>
#include <md/md_event.h>
#include <om/om_event.h>
#include <cmath>
#include "Common.h"
#include "OrderManager.h"
#include "InternalMatchingEngine.h"
#include "ViperConfig.h"
#include "logging/Logger.h"
#include "RingBufferQueue.h"
#include "MarketMaker.h"
#include "OrderRequestManager.h"
#include <atomic>

namespace {
template <uint32_t Side>
inline hf::qty_t tryOffsetPosition(hf::qty_t& position, hf::qty_t remaining, hf::qty_t& allocatedPosition);

template<>
inline hf::qty_t tryOffsetPosition<Python::BID>(hf::qty_t& position, hf::qty_t remaining, hf::qty_t& allocatedPosition) {
    if (position < 0) {
        const hf::qty_t absPosition = -position;
        const hf::qty_t allocated = std::min(absPosition, remaining);
        allocatedPosition += allocated;
        position += allocated;
        return remaining - allocated;
    }
    return remaining;
}

template<>
inline hf::qty_t tryOffsetPosition<Python::ASK>(hf::qty_t& position, hf::qty_t remaining, hf::qty_t& allocatedPosition) {
    if (position > 0) {
        const hf::qty_t allocated = std::min(position, remaining);
        allocatedPosition += allocated;
        position -= allocated;
        return remaining - allocated;
    }
    return remaining;
}

}

namespace Python {

class SimpleMarketMaker {
    friend class MarketMaker;
    OrderRequestManager& _orderRequestManager = OrderRequestManager::get();
    MarketMaker* _marketMaker;
    std::atomic_uint_fast32_t _inFillProcess {0};
    uint32_t _finishedFillProcess = 0;
    ReadyOrderRequests _lastOrderRequests;
    bool _maxTicksWideBreached = false;

    static constexpr uint8_t QUOTE_ORDER_ID = orderId(StrategyId::MarketMaker, Order::Type::Quoting);
    static constexpr uint8_t HEDGE_ORDER_ID = orderId(StrategyId::MarketMaker, Order::Type::Hedging);

public:

    struct Quote {
        OrderId_t _id = QUOTE_ORDER_ID;
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
        OrderId_t _id = HEDGE_ORDER_ID;
        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::qty_t _sizeToQuote = 0;
        bool _inPayUp = false;

        void reset() {
            _externalPrice = hf::NULL_PRICE;
            _internalPrice = hf::NULL_PRICE;
            _sizeToQuote = 0;
            _inPayUp = false;
        }
    };

    void initialize(const ConfigContract& contract,
                    const MarketMakerConfig& config,
                    MarketMaker* marketMaker,
                    double worsenPayupCond) {
        _marketMaker = marketMaker;
        _enabled = config._enabled;
        _quoteExternal = config._quoteExternal;
        _quoteSize = config._quoteSize;
        _widthPercentage = config._widthPercentage[toUInt(Shifts::__current_marker__)];
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
        _contract = contract;
        _tickSize = contract._tickSize;
        _hedgeOffset = config._hedgeOffset;
        _hedgeOffsetPrice = _hedgeOffset * _tickSize;
        _maxTicksWide = config._maxTicksWide < 0 ? std::numeric_limits<int64_t>::max() : config._maxTicksWide * _tickSize;
        _internallyInitiatedPosition = config._internalPosition;
        _externallyInitiatedPosition = config._externalPosition;
        _worsenPayupCondition = worsenPayupCond;
    }

    hf::qty_t takeOverFill(uint32_t side, hf::qty_t overFillSize) {
        LOG(0, "MM {C[i1]}. Take overfill. Size={i2}", getContractName(), overFillSize)
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        hf::qty_t absExternalPosition = std::abs(_externallyInitiatedPosition);
        if (_maxExternallyInitiatedPosition <= absExternalPosition) {
            LOG(0, "MM {C[i1]}. External position reached or exceeded max, could not take anymore.", getContractName())
            return 0;
        }
        const hf::qty_t newPosition = overFillSize * multiplier + _externallyInitiatedPosition;
        if (std::abs(newPosition) > _maxExternallyInitiatedPosition) {
            hf::qty_t sizeTaken = _maxExternallyInitiatedPosition - absExternalPosition;
            _externallyInitiatedPosition += sizeTaken * multiplier;
            LOG(0, "MM {C[i1]}. Could not take the whole overfill. Took={i2}", getContractName(), sizeTaken)
            return sizeTaken;
        }
        else {
            _externallyInitiatedPosition = newPosition;
            LOG(0, "MM {C[i1]}. Took the whole overfill. Took={i2}", getContractName(), overFillSize)
            return overFillSize;
        }
    }

    void forceTakeOverFill(uint32_t side, hf::qty_t overFillSize) {
        LOG(0, "MM {C[i1]}. Forced to take overfill. Size={i2}", getContractName(), overFillSize)
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        _externallyInitiatedPosition += overFillSize * multiplier;
        LOG(0, "MM {C[i1]}. After forced to take fill. ExtInitPosition={i2} MaxExtInitPos={i3}", getContractName(), _externallyInitiatedPosition, _maxExternallyInitiatedPosition)
    }

    bool isHedgingAsk() {
        return _hedges[ASK]._sizeToQuote != 0;
    }

    bool isHedgingBid() {
        return _hedges[BID]._sizeToQuote != 0;
    }

    bool isThereEnoughSizeToJoinForBid(Book& bidBook, hf::price_t quotePrice) {
        return ((quotePrice >= bidBook._price) & (bidBook._otherQuantity >= _joinSize)) | (quotePrice < bidBook._price);
    }

    bool isThereEnoughSizeToStayForBid(Book& bidBook, hf::price_t quotePrice) {
        return ((quotePrice >= bidBook._price) & (bidBook._otherQuantity >= _leaveSize)) | (quotePrice < bidBook._price);
    }

    bool isThereEnoughSizeToJoinForAsk(Book& askBook, hf::price_t quotePrice) {
        return ((quotePrice <= askBook._price) & (askBook._otherQuantity >= _joinSize)) | (quotePrice > askBook._price);
    }

    bool isThereEnoughSizeToStayForAsk(Book& askBook, hf::price_t quotePrice) {
        return ((quotePrice <= askBook._price) & (askBook._otherQuantity >= _leaveSize)) | (quotePrice > askBook._price);
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

    bool isInPayUp(hf::qty_t hedgeSize, Book& book) {
        const hf::qty_t payUpBasedOnHedgeSize = static_cast<hf::qty_t>(hedgeSize * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnHedgeSize);
        const bool shouldPayup = (book._otherQuantity <= payupQuantity) | (book._otherOrderCount <= _payUpOrders);
        return shouldPayup;
    }

    bool isEnabled() const {
        return _enabled & _marketMaker->isEnabled() & canContractTrade();
    }

    bool canQuoteExternally() {
        return _quoteExternal & _marketMaker->canQuoteExternally() & _marketMaker->canExchangeQuoteExternally(_contract._venue);
    }

    bool canContractTrade() const {
        return _contractCanTrade & _gatewayActive;
    }

    bool hasMarketData(Book& bidBook, Book& askBook) {
        const bool missingMarketData = ((bidBook._price == hf::NULL_PRICE) | (askBook._price == hf::NULL_PRICE));
        if (BOOST_UNLIKELY(missingMarketData)) {
            if (_contract._toMarketMake & _marketMaker->canExchangeQuoteExternally(_contract._venue)) {
                if (_enabled) {
                    _marketMaker->sendErrorMessage(boost::str(boost::format("MM - Not having market data so turning market maker off. Contract=%1%") % _contract._name), ErrorType::Critical);
                    LOG(0, "MM {C[i1]}. Lost market data. Disabling, turn off external quoting and useGTC. ", getContractName())
                    disable(true, false, true);
                    setQuoteExternalFlagToFalse();
                    _marketMaker->notifyFrontMMExternalQuoteDisabled(_contract._internalId);
                    _marketMaker->sendErrorMessage(boost::str(boost::format("MM - no market data, disable external quoting, disable GTC. Contract=%1%") % _contract._name), ErrorType::Critical);
                }
                else {
                    disableExternalQuotingAndUseGTC(boost::str(boost::format("MM - no market data, disable external quoting, disable GTC. Contract=%1%") % _contract._name), false);
                }
            }
            return false;
        }
        return true;
    }

    void onChangeInOtherBooks(bool forceUpdate = false, bool hedgeSizeChanged = false) {
        auto& bidBook = _books[BID];
        auto& askBook = _books[ASK];

        LOG(0, "MM {C[i1]}. Recalculate. Bid: {i2}({i3})@{d1} Ask={i4}({i5})@{d2} SequenceNumber={i6}", getContractName(), bidBook._otherQuantity, bidBook._otherOrderCount, ToDollars(bidBook._price), askBook._otherQuantity, askBook._otherOrderCount, ToDollars(askBook._price), _lastMdSeqNum)

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }

        if (BOOST_UNLIKELY(!isEnabled())) {
            return;
        }

        bool hasChange = false;
        if (BOOST_UNLIKELY((askBook._price - bidBook._price) > _maxTicksWide)) {
            std::string msg = boost::str(boost::format("MM - MaxTicksWidth breached, disable external quoting, disable GTC. Contract=%1%") % _contract._name);
            disableExternalQuotingAndUseGTC(msg);
            _maxTicksWideBreached = true;
        } else if (_maxTicksWideBreached) {
            hasChange = calculateBidQuoteSizes();
            hasChange |= calculateAskQuoteSizes();
            _maxTicksWideBreached = false;
        }

        _midPoint = calculateMidPoint(bidBook, askBook);
        LOG(0, "MM {C[i1]}. Midpoint. Midpoint={d1}", getContractName(), ToDollars(_midPoint))

        if (BOOST_LIKELY(!_maxTicksWideBreached)) {
            const bool quoteExternally = canQuoteExternally();
            const bool persistPrice = quoteExternally;
            const double marketSpread = askBook._price - bidBook._price;
            const hf::price_t width = static_cast<hf::price_t>(marketSpread / 2.0 * _widthPercentage);

            {
                const bool isBidBookInPayUpMode = isInPayUp(getBidExternalQuoteSize(), bidBook);
                Quote& quote = _quotes[BID];
                bool changeInPrice = false;

                if (!isBidBookInPayUpMode) {
                    const hf::price_t internalPrice = _midPoint - width;
                    const hf::price_t internalPriceChange = internalPrice - quote._internalPrice;
                    changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
                    if (changeInPrice | forceUpdate) {
                        quote._internalPrice = internalPrice;
                        if (quoteExternally) {
                            quote._externalPrice = RoundToTickSize(quote._internalPrice  - _commission, _tickSize, BID);
                            if (IsBetterPrice<BID>(quote._externalPrice, bidBook._price)) {
                                quote._externalPrice = bidBook._price;
                            }
                        }
                    }
                }
                else {
                    const hf::price_t worsenPrice = bidBook._price - _worsenPayupCondition * _tickSize;
                    const hf::price_t ordinaryPrice = _midPoint - width;
                    const hf::price_t internalPrice = std::min(ordinaryPrice, worsenPrice);
                    changeInPrice = internalPrice != quote._internalPrice;
                    if (changeInPrice | forceUpdate) {
                        quote._internalPrice = internalPrice;
                        if (quoteExternally) {
                            quote._externalPrice = RoundToTickSize(quote._internalPrice  - _commission, _tickSize, BID);
                        }
                    }
                }

                bool changeInSize = false;
                if (quoteExternally) {
                    if (quote._externalSize == 0) {
                        if (isThereEnoughSizeToJoinForBid(bidBook, quote._externalPrice)) {
                            quote._externalSize = getBidExternalQuoteSize();
                            changeInSize = true;
                        }
                    } else {
                        if (!isThereEnoughSizeToStayForBid(bidBook, quote._externalPrice)) {
                            quote._externalSize = 0;
                            changeInSize = true;
                        }
                    }
                } else {
                    changeInSize |= quote._externalSize != 0;
                    quote._externalSize = 0;
                }

                if (changeInPrice | changeInSize | forceUpdate) {
                    modifyQuoteOrder<BID>(quote._internalPrice, quote._internalSize + quote._externalSize, quote._externalPrice, quote._externalSize, persistPrice);
                    hasChange = true;
                }
            }

            {
                const bool isAskBookInPayUpMode = isInPayUp(getAskExternalQuoteSize(), askBook);
                Quote& quote = _quotes[ASK];
                bool changeInPrice = false;

                if (!isAskBookInPayUpMode) {
                    const hf::price_t internalPrice = _midPoint + width;
                    const hf::price_t internalPriceChange = internalPrice - quote._internalPrice;
                    changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
                    if (changeInPrice | forceUpdate) {
                        quote._internalPrice = internalPrice;
                        if (quoteExternally) {
                            quote._externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
                            if (IsBetterPrice<ASK>(quote._externalPrice, askBook._price)) {
                                quote._externalPrice = askBook._price;
                            }
                        }
                    }
                }
                else {
                    const hf::price_t worsenPrice = askBook._price + _worsenPayupCondition * _tickSize;
                    const hf::price_t ordinaryPrice = _midPoint + width;
                    const hf::price_t internalPrice = std::max(ordinaryPrice, worsenPrice);
                    changeInPrice = internalPrice != quote._internalPrice;
                    if (changeInPrice | forceUpdate) {
                        quote._internalPrice = internalPrice;
                        if (quoteExternally) {
                            quote._externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
                        }
                    }
                }

                bool changeInSize = false;
                if (quoteExternally) {
                    if (quote._externalSize == 0) {
                        if (isThereEnoughSizeToJoinForAsk(askBook, quote._externalPrice)) {
                            quote._externalSize = getAskExternalQuoteSize();
                            changeInSize = true;
                        }
                    } else {
                        if (!isThereEnoughSizeToStayForAsk(askBook, quote._externalPrice)) {
                            quote._externalSize = 0;
                            changeInSize = true;
                        }
                    }
                } else {
                    changeInSize |= quote._externalSize != 0;
                    quote._externalSize = 0;
                }

                if (changeInPrice | changeInSize | forceUpdate) {
                    modifyQuoteOrder<ASK>(quote._internalPrice, quote._internalSize + quote._externalSize, quote._externalPrice, quote._externalSize, persistPrice);
                    hasChange = true;
                }
            }
        } else {
            _quotes[BID].reset();
            _quotes[ASK].reset();
            modifyQuoteOrder<BID>(hf::NULL_PRICE, 0, hf::NULL_PRICE, 0, false);
            modifyQuoteOrder<ASK>(hf::NULL_PRICE, 0, hf::NULL_PRICE, 0, false);
            hasChange = true;
        }

        if (isHedgingAsk()) {
            Hedge& hedge = _hedges[ASK];
            bool prePayUp = hedge._inPayUp;
            hf::price_t internalPrice;
            hf::price_t externalPrice;
            hedge._inPayUp = calculateAskHedgePrices(hedge._sizeToQuote, internalPrice, externalPrice, bidBook, askBook);
            if (hedge._inPayUp) {
                hedge._internalPrice = internalPrice;
                hedge._externalPrice = externalPrice;
                modifyHedgeOrder<ASK>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, true);
                LOG(0, "MM {C[i1]}. ASK Paying up. IntPrice={d1} ExtPrice={d2} Size={i2} IntPosition={i3} ExtPosition={i4}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote, _internallyInitiatedPosition, _externallyInitiatedPosition)
                hasChange = true;
            } else {
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged || prePayUp != hedge._inPayUp) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = externalPrice;
                    LOG(0, "MM {C[i1]}. ASK Repriced hedge. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    modifyHedgeOrder<ASK>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, false);
                    hasChange = true;
                }
            }
        }

        if (isHedgingBid()) {
            Hedge& hedge = _hedges[BID];
            bool prePayUp = hedge._inPayUp;

            hf::price_t internalPrice;
            hf::price_t externalPrice;
            hedge._inPayUp = calculateBidHedgePrices(hedge._sizeToQuote, internalPrice, externalPrice, bidBook, askBook);

            if (hedge._inPayUp) {
                hedge._internalPrice = internalPrice;
                hedge._externalPrice = externalPrice;
                modifyHedgeOrder<BID>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, true);
                LOG(0, "MM {C[i1]}. BID Paying up. IntPrice={d1} ExtPrice={d2} Size={i2} IntPosition={i3} ExtPosition={i4}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote, _internallyInitiatedPosition, _externallyInitiatedPosition)
                hasChange = true;
            }
            else {
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged || prePayUp != hedge._inPayUp) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = externalPrice;
                    LOG(0, "MM {C[i1]}. BID Repriced hedge. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    modifyHedgeOrder<BID>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, false);
                    hasChange = true;
                }
            }
        }

        if (!isInFillProcess() && (hasChange | hedgeSizeChanged)) {
            notifyOrderRequest();
        }
    }

    void notifyOrderRequest() {
        _orderRequestManager.notifyOrderRequest(_contract._internalId, StrategyId::MarketMaker, _lastOrderRequests);
    }

    template<uint32_t Side>
    void modifyQuoteOrder(hf::price_t internalPrice,
                          hf::qty_t internalSize,
                          hf::price_t externalPrice,
                          hf::qty_t externalSize,
                          bool persist) {
        _lastOrderRequests._quote[Side] = _orderRequestManager.sendQuoteRequest(_contract._internalId,
                                                                                QUOTE_ORDER_ID,
                                                                                Side, internalPrice, internalSize,
                                                                                externalPrice,
                                                                                externalSize, persist);
        DLOG(0, "MM {C[i1]}. Send quote. Side={i2} Internal: {i3}@{d1} External: {i4}@{d2} persist={i5} reqId={i6}",
             getContractName(), Side, internalSize, ToDollars(internalPrice), externalSize, ToDollars(externalPrice),
             persist, _lastOrderRequests._quote[Side])
    }

    void disableExternalQuotingAndUseGTC(const std::string& message, bool placeHedgeBack = true) {
        bool msgSend = false;
        auto oldValue = setQuoteExternalFlagToFalse();
        if (oldValue) {
            _marketMaker->notifyFrontMMExternalQuoteDisabled(_contract._internalId);
            _marketMaker->sendErrorMessage(message, ErrorType::Critical);
            msgSend = true;
        }

        if (*_contract._useGTC) {
            _marketMaker->sendGenericMessageToExecutor(ViperMessageType::DisableUseGTC, _contract._internalId);
            if (!msgSend) {
                _marketMaker->sendErrorMessage(message, ErrorType::Critical);
            }
        }
    }

    hf::price_t calculateMidPoint(Book& bid, Book& ask) {
        const hf::qty_t askSize = std::min(ask._otherQuantity, _establishedLevel);
        const hf::qty_t bidSize = std::min(bid._otherQuantity, _establishedLevel);
        return (ask._price * bidSize + bid._price * askSize) / (bidSize + askSize);
    }

    template <uint32_t Side>
    void allocateHedgingFillRemainder(bool internal, hf::qty_t remaining, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        const hf::qty_t signedRemaining = Side == BID ? remaining : -remaining;
        if (internal) {
            _internallyInitiatedPosition += signedRemaining;
            allocatedInternally += signedRemaining;
        } else {
            _externallyInitiatedPosition += signedRemaining;
            allocatedExternally += signedRemaining;
        }
    }

    template <uint32_t Side>
    hf::qty_t doAllocateHedgingFill(hf::qty_t size, bool internal, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        hf::qty_t remaining = tryOffsetPosition<Side>(_internallyInitiatedPosition, size, allocatedInternally);
        LOG(0, "MM {C[i1]}. Offset internal position. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        if (remaining > 0) {
            remaining = tryOffsetPosition<Side>(_externallyInitiatedPosition, remaining, allocatedExternally);
            LOG(0, "MM {C[i1]}. Offset external position. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        }
        return remaining;
    }

    hf::qty_t allocateHedgingFill(uint32_t side, hf::qty_t size, bool internal, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        LOG(0, "MM {C[i1]}. Allocate Hedging Fill. Side={i2} Size={i3}", getContractName(), side, size)
        LOG(0, "MM {C[i1]}. Before Hedging Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
        hf::qty_t remaining;
        if (side == BID) {
            remaining = doAllocateHedgingFill<BID>(size, internal, allocatedInternally, allocatedExternally);
        } else {
            remaining = doAllocateHedgingFill<ASK>(size, internal, allocatedInternally, allocatedExternally);
            allocatedInternally *= -1;
            allocatedExternally *= -1;
        }
        LOG(0, "MM {C[i1]}. After Hedging Fill. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        return remaining;
    }

    void onFill(Order* order, hf::price_t price, hf::qty_t size, bool internal, bool isPayUp, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        bool isHedgingFill = order->_type == Order::Type::Hedging;
        LOG(0, "MM {C[i1]}. Fill. Hedging={i2} Price={d1} Qty={i3} Internal={i4} Side={i5} PayUp={i6}", getContractName(), isHedgingFill, ToDollars(price), size, internal, order->_side, isPayUp)
        if (!isHedgingFill) {
            Quote& quote = _quotes[order->_side];
            hf::qty_t remainingSize = size;
            if (internal) {
                if (order->_side == BID) {
                    remainingSize = tryOffsetPosition<BID>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
                    remainingSize = tryOffsetPosition<BID>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
                }
                else {
                    remainingSize = tryOffsetPosition<ASK>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
                    remainingSize = tryOffsetPosition<ASK>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
                }

                if (remainingSize > 0) {
                    hf::qty_t overfill = std::max<hf::qty_t>(remainingSize - quote._internalSize, 0);
                    if (overfill > 0) {
                        // Assign the overfill to external.
                        allocateExternalQuoteFill(quote, overfill, order->_side);
                        allocatedExternally += overfill;
                        remainingSize = quote._internalSize;
                    }
                    quote._internalSize -= remainingSize;
                    if (order->_side == BID) {
                        _internallyInitiatedPosition += remainingSize;
                    } else {
                        _internallyInitiatedPosition -= remainingSize;
                    }
                    allocatedInternally += remainingSize;
                }
            } else {
                if (order->_side == BID) {
                    remainingSize = tryOffsetPosition<BID>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
                    remainingSize = tryOffsetPosition<BID>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
                }
                else {
                    remainingSize = tryOffsetPosition<ASK>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
                    remainingSize = tryOffsetPosition<ASK>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
                }

                if (remainingSize > 0) {
                    allocateExternalQuoteFill(quote, remainingSize, order->_side);
                    allocatedExternally += remainingSize;
                }
            }
            LOG(0, "MM {C[i1]}. Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
        }
        else {
            auto remaining = allocateHedgingFill(order->_side, size, internal, allocatedInternally, allocatedExternally);
            BOOST_ASSERT(internal);
            if (remaining > 0 && internal) {
                if (order->_side == BID) {
                    allocateHedgingFillRemainder<BID>(internal, remaining, allocatedInternally, allocatedExternally);
                } else {
                    allocateHedgingFillRemainder<ASK>(internal, remaining, allocatedInternally, allocatedExternally);
                }
            }
        }
    }

    void allocateExternalQuoteFill(Quote& quote, hf::qty_t size, uint32_t side) {
        if (BOOST_LIKELY(size <= quote._externalSize)) {
            quote._externalSize -= size;
        } else {
            logging::GetLogger().logError(boost::str(boost::format(
                    "Market maker received external fill where fill size %1% is more than quote has %2%.")
                    % size % quote._externalSize));
            quote._externalSize = 0;
        }
        if (side == BID) {
            _externallyInitiatedPosition += size;
        } else {
            _externallyInitiatedPosition -= size;
        }
    }

    hf::qty_t calculateHedgeQuoteSize() {
        return -(_internallyInitiatedPosition + _externallyInitiatedPosition);
    }

    bool modifyHedgeQuote(bool forceOther = false) {
        auto& bidBook = _books[BID];
        auto& askBook = _books[ASK];
        bool hasHedge = false;
        hf::qty_t quoteSize = calculateHedgeQuoteSize();
        LOG(0, "MM {C[i1]}. Modifying hedge quote. Before. InternalPosition={i2} ExternalPosition={i3} QuoteSize={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, quoteSize)
        if (quoteSize > 0) {
            Hedge& hedge = _hedges[BID];
            hedge._sizeToQuote = quoteSize;
            hedge._inPayUp = calculateBidHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
            LOG(0, "MM {C[i1]}. BID Hedge order. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
            modifyHedgeOrder<BID>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, hedge._inPayUp);
            hasHedge = true;
            Hedge& otherHedge = _hedges[ASK];
            if ((otherHedge._sizeToQuote > 0) | forceOther) {
                otherHedge.reset();
                modifyHedgeOrder<ASK>(otherHedge._internalPrice, otherHedge._externalPrice, otherHedge._sizeToQuote, false);
            }
        }
        else if (quoteSize < 0) {
            quoteSize = -quoteSize;
            Hedge& hedge = _hedges[ASK];
            hedge._sizeToQuote = quoteSize;
            hedge._inPayUp = calculateAskHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
            LOG(0, "MM {C[i1]}. ASK Hedge order. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
            modifyHedgeOrder<ASK>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, hedge._inPayUp);
            hasHedge = true;
            Hedge& otherHedge = _hedges[BID];
            if ((otherHedge._sizeToQuote > 0) | forceOther) {
                otherHedge.reset();
                modifyHedgeOrder<BID>(otherHedge._internalPrice, otherHedge._externalPrice, otherHedge._sizeToQuote, false);
            }
        }
        else {
            Hedge& bidHedge = _hedges[BID];
            if ((bidHedge._sizeToQuote > 0) | forceOther) {
                bidHedge.reset();
                modifyHedgeOrder<BID>(bidHedge._internalPrice, bidHedge._externalPrice, bidHedge._sizeToQuote, false);
                hasHedge = true;
            }
            Hedge& askHedge = _hedges[ASK];
            if ((askHedge._sizeToQuote > 0) | forceOther) {
                askHedge.reset();
                modifyHedgeOrder<ASK>(askHedge._internalPrice, askHedge._externalPrice, askHedge._sizeToQuote, false);
                hasHedge = true;
            }
        }

        LOG(0, "MM {C[i1]}. Modifying hedge quote. End. InternalPosition={i2} ExternalPosition={i3} QuoteSize={i4}",
            getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, quoteSize)
        return hasHedge;
    }

    template<uint32_t Side>
    void modifyHedgeOrder(hf::price_t internalPrice, hf::price_t externalPrice, hf::qty_t hedgeSize, bool inPayUp) {
        _lastOrderRequests._hedge[Side] = _orderRequestManager.sendHedgeRequest(_contract._internalId, HEDGE_ORDER_ID,
                                                                                Side, internalPrice, externalPrice,
                                                                                hedgeSize, true, inPayUp);
        DLOG(0,
             "MM {C[i1]}. Send hedge. Side={i2} Size={i3} InternalPrice={d1} ExternalPrice={d2} PayUp={i4} reqId={i5}",
             getContractName(), Side, hedgeSize, ToDollars(internalPrice), ToDollars(externalPrice), inPayUp,
             _lastOrderRequests._hedge[Side])
    }

    bool calculateBidHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const bool inPayUp = isInPayUp(hedgeSize, askBook);
        if (inPayUp) {
            externalPrice = askBook._price;
            internalPrice = externalPrice + _commission;
        } else {
            _midPoint = calculateMidPoint(bidBook, askBook);
            hf::price_t ref_price = (_quotes[ASK]._internalPrice != hf::NULL_PRICE) ? std::min<hf::price_t>(_quotes[ASK]._internalPrice, askBook._price) : askBook._price;
            internalPrice = ref_price - _hedgeOffsetPrice;;
            externalPrice = RoundToTickSize(internalPrice - _commission, _tickSize, BID);
        }
        return inPayUp;
    }

    bool calculateAskHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const bool inPayUp = isInPayUp(hedgeSize, bidBook);
        if (inPayUp) {
            externalPrice = bidBook._price;
            internalPrice = externalPrice - _commission;
        } else {
            _midPoint = calculateMidPoint(bidBook, askBook);
            hf::price_t ref_price = (_quotes[BID]._internalPrice != hf::NULL_PRICE) ? std::max<hf::price_t>(_quotes[BID]._internalPrice, bidBook._price) : bidBook._price;
            internalPrice = ref_price + _hedgeOffsetPrice;
            externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
        }
        return inPayUp;
    }

    void onEndOfFills(uint32_t fillId, bool isManualFill = false) {
        ++_finishedFillProcess;
        LOG(0, "MM {C[i1]}. EndOfFill. fillId={i2} finishedFillProcess={i3} inFillProcess={i4} IsInFillProcess={i5} internalPosition={i6} externalPosition={i7}",
            getContractName(), fillId, _finishedFillProcess, _inFillProcess.load(std::memory_order_relaxed),
            isInFillProcess(), _internallyInitiatedPosition, _externallyInitiatedPosition)

        auto& bidBook = _books[BID];
        auto& askBook = _books[ASK];

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }

        //Likely offline GTC fill
        if(BOOST_UNLIKELY(!isManualFill & !(_marketMaker->isEnabled() || _marketMaker->isRVEnabled()))) {
            [this]() NEVER_INLINE COLD {
                LOG(0, "MM {C[i1]}. Fill received while MM and RV disabled (maybe a gtc?). Check fills window!", getContractName());
                _marketMaker->sendErrorMessage("Market maker " + _contract._name + ". Fill received while MM and RV disabled. Check fills window!", ErrorType::Critical);
            }();
            return;
        }

        recalculateSizesAndApplyUpdate();
    }

    void onEndOfExternalFills(uint32_t side, uint32_t fillId) {
        LOG(0, "MM {C[i1]}. Side={i2} End of external fills. FillId={i3}", getContractName(), side, fillId)
        onEndOfFills(fillId);
    }

    void onEndOfInternalFills(uint32_t fillId, bool isManualFill) {
        LOG(0, "MM {C[i1]}. End of internal fills. FillId={i2}", getContractName(), fillId)
        onEndOfFills(fillId, isManualFill);
    }

    bool calculateBidQuoteSizes() {
        if (!isEnabled()) {
            return false;
        }

        bool update = false;
        Quote& quote = _quotes[BID];

        const hf::qty_t internalSize = getBidInternalQuoteSize();
        update |= internalSize != quote._internalSize;

        const bool quoteExternally = canQuoteExternally();
        hf::qty_t externalSize = quote._externalSize;
        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSizeToJoinForBid(_books[BID], quote._externalPrice)) {
                    externalSize = getBidExternalQuoteSize();
                }
            } else {
                if (!isThereEnoughSizeToStayForBid(_books[BID], quote._externalPrice)) {
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
            // Use the sum of internalSize and externalSize as order's internal size to resolve a situation where
            // MM internal size less than it's external size, after IME internal matching MM's leftover external
            // order and RV quote are ended on same price.
            modifyQuoteOrder<BID>(quote._internalPrice, internalSize + externalSize, quote._externalPrice, quote._externalSize, quoteExternally);
        }
        return update;
    }

    bool calculateAskQuoteSizes() {
        if (!isEnabled()) {
            return false;
        }

        bool update = false;
        Quote& quote = _quotes[ASK];

        const hf::qty_t internalSize = getAskInternalQuoteSize();
        update |= internalSize != quote._internalSize;

        const bool quoteExternally = canQuoteExternally();
        hf::qty_t externalSize = quote._externalSize;

        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSizeToJoinForAsk(_books[ASK], quote._externalPrice)) {
                    externalSize = getAskExternalQuoteSize();
                }
            } else {
                if (!isThereEnoughSizeToStayForAsk(_books[ASK], quote._externalPrice)) {
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
            // Use the sum of internalSize and externalSize as order's internal size to resolve a situation where
            // MM internal size less than it's external size, after IME internal matching MM's leftover external
            // order and RV quote are ended on same price.
            modifyQuoteOrder<ASK>(quote._internalPrice, internalSize + externalSize, quote._externalPrice, quote._externalSize, quoteExternally);
        }
        return update;
    }

    void applyUpdates() {
        if (!isEnabled() || !_contract._toMarketMake || isInFillProcess()) {
            return;
        }

        bool hasHedgeChange = modifyHedgeQuote();
        onChangeInOtherBooks(true, hasHedgeChange);
    }

    bool isWithinPositionLimits() {
        bool withinLimits = true;
        if (std::abs(_internallyInitiatedPosition) > _maxInternallyInitiatedPosition) {
            _marketMaker->sendErrorMessage(boost::str(boost::format("MM - at/over max internally initiated position. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "MM {C[i1]}. Internally initiated position is at/over limit", getContractName())
            withinLimits = false;
        }
        if (std::abs(_externallyInitiatedPosition) > _maxExternallyInitiatedPosition) {
            _marketMaker->sendErrorMessage(boost::str(boost::format("MM - at/over max externally initiated position. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "MM {C[i1]}. Externally initiated position is at/over limit", getContractName())
            withinLimits = false;
        }
        return withinLimits;
    }

    bool hasMarketData() {
        bool hasMD = ((_books[BID]._price != hf::NULL_PRICE) & (_books[ASK]._price != hf::NULL_PRICE));
        if (!hasMD) {
            _marketMaker->sendErrorMessage(boost::str(boost::format("MM - does not have market data. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "MM {C[i1]}. does not have market data", getContractName())
        }
        return hasMD;
    }

    bool canStart() {
        if (_enabled) {
            if (!hasMarketData()) {
                disable();
                return false;
            }
            return true;
        }
        return false;
    }

    void start() {
        if (_contract._toMarketMake) {
            if (!canStart()) {
                return;
            }

            if(!isWithinPositionLimits()) {
                setQuoteExternalFlagToFalse();
                _marketMaker->sendErrorMessage("MM - isWithinPositionLimits() check failed on start, disabling external quoting", ErrorType::Critical);
                _marketMaker->notifyFrontMMExternalQuoteDisabled(_contract._internalId);
            }

            Quote& bidQuote = _quotes[BID];
            bidQuote._internalSize = getBidInternalQuoteSize();
            bidQuote._externalSize = 0;
            Quote& askQuote = _quotes[ASK];
            askQuote._internalSize = getAskInternalQuoteSize();
            askQuote._externalSize = 0;
            bool hasHedge = modifyHedgeQuote();
            onChangeInOtherBooks(true, hasHedge);
        }
    }

    void disable(bool changeFlag = true, bool skipCancelRequest = false, bool disableGTC = false) {
        if (_contract._toMarketMake) {
            if (!skipCancelRequest) {
                _orderRequestManager.sendCancelRequest(StrategyId::MarketMaker, _contract._internalId, disableGTC);
            }
            _quotes[BID].reset();
            _quotes[ASK].reset();
            _hedges[BID].reset();
            _hedges[ASK].reset();

            if (changeFlag) {
                _enabled = false;
                _marketMaker->sendGenericMessageToFront(ViperMessageType::MarketMakerEnabled, _contract._internalId, 0);
            }
        }
    }

    bool setQuoteExternalFlagToFalse(bool skip_logging = false) {
        bool cachedValue = _quoteExternal;
        _quoteExternal = false;
        if (!skip_logging & cachedValue) {
            LOG(0, "MM {C[i1]}. Quote external changed. Value=false", getContractName());
        }
        return cachedValue;
    }

    bool disableExternalQuoting(bool skip_logging = false) {
        bool oldValue = setQuoteExternalFlagToFalse(skip_logging);
        if (oldValue) {
            applyUpdates();
        }
        return oldValue;
    }

    void handleParametersUpdate(proto::GridCell& gridCell) {
        MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(gridCell.col());
        switch (columnType) {
            case MarketMakerParametersColumns::QuoteExternal: {
                bool quoteExternal = 1 == atoi(gridCell.value().c_str());
                _quoteExternal = quoteExternal;
                LOG(0, "MM {C[i1]}. Quote external changed. Value={i2}", getContractName(), quoteExternal)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::QuoteSize: {
                _quoteSize = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. Quote size changed. Value={i2}", getContractName(), _quoteSize)
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::HedgeOffset: {
                double value = boost::lexical_cast<double>(gridCell.value());
                if (value <= 0.0) {
                    LOG(0, "MM {C[i1]}. HedgeOffset must be > 0. Set Value back to {d1}", getContractName(), _hedgeOffset)
                    _marketMaker->sendErrorMessage("MM HedgeOffset Parameter must > 0.", ErrorType::Normal);
                    gridCell.set_value(std::to_string(_hedgeOffset));
                } else {
                    _hedgeOffset =  value;
                    _hedgeOffsetPrice = value * _tickSize;
                    LOG(0, "MM {C[i1]}. HedgeOffset changed. Value={d1}", getContractName(), _hedgeOffset)
                    applyUpdates();
                }
                break;
            }
            case MarketMakerParametersColumns::JoinSize: {
                _joinSize = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. Join size changed. Value={i2}", getContractName(), _joinSize)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::LeaveSize: {
                _leaveSize = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. Leave size changed. Value={i2}", getContractName(), _leaveSize)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::MaxPosInterrnal: {
                _maxInternallyInitiatedPosition = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. MaxPosInternal changed. Value={i2}", getContractName(), _maxInternallyInitiatedPosition)
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::MaxPosExternal: {
                _maxExternallyInitiatedPosition = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. MaxPosExternal changed. Value={i2}", getContractName(), _maxExternallyInitiatedPosition)
                recalculateSizesAndApplyUpdate();
                break;
            }
            case MarketMakerParametersColumns::PayUpOrders: {
                _payUpOrders = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. PayUpOrders changed. Value={i2}", getContractName(), _payUpOrders)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::PayUpQty: {
                _payUpQuantity = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. PayUpQty changed. Value={i2}", getContractName(), _payUpQuantity)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::PayUpMultiple: {
                _payUpMultiple = atof(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. PayUpMultiple changed. Value={d1}", getContractName(), _payUpMultiple)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::AsiaWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. AsiaWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::Asia) {
                    updateWidthPercent(widthPercentage);
                }
                break;
            }
            case MarketMakerParametersColumns::EuroWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. EuroWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::Europe) {
                    updateWidthPercent(widthPercentage);
                }
                break;
            }
            case MarketMakerParametersColumns::USWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. USWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::US) {
                    updateWidthPercent(widthPercentage);
                }
                break;
            }
            case MarketMakerParametersColumns::MinInternalPriceChange: {
                _minInternalPriceChange = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                LOG(0, "MM {C[i1]}. Min internal price change changed. Value={d1}", getContractName(), ToDollars(_minInternalPriceChange))
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::EstablishedLevel: {
                _establishedLevel = boost::lexical_cast<uint32_t>(gridCell.value());
                LOG(0, "MM {C[i1]}. Established level changed. Value={i2}", getContractName(), _establishedLevel)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::Commission: {
                _commission = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                LOG(0, "MM {C[i1]}. Commission changed. Value={d1}", getContractName(), _commission)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::Enabled: {
                bool enabled = 1 == atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. Enabled changed. Value={i2}", getContractName(), enabled)
                if (enabled != _enabled) {
                    _enabled = enabled;
                    if (enabled) {
                        if (isEnabled()) {
                            start();
                        }
                    } else {
                        disable();
                    }
                }
                break;
            }
            case MarketMakerParametersColumns::MaxTicksWide: {
                int32_t value = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. MaxTicksWide changed. Value={i2}", getContractName(), value)
                if (value  < 0) {
                    _maxTicksWide = std::numeric_limits<int64_t>::max();
                    LOG(0, "MM {C[i1]}. Ignore MaxTicksWide checking.", getContractName())
                } else {
                    _maxTicksWide = value * _tickSize;
                }
                applyUpdates();
                break;
            }
            default:
                BOOST_ASSERT(false);
                break;
        }
    }

    void updateWidthPercent(double widthPercentage) {
        _widthPercentage = widthPercentage;
        LOG(0, "MM {C[i1]}. Update MMWidthPercentage to {d1}", getContractName(), _widthPercentage)
        applyUpdates();
    }

    void handleWorsenPayupConditionUpdate(double value) {
        _worsenPayupCondition = value;
        LOG(0, "MM {C[i1]}. Worsen_payup_cond_percent changed Value={d1}", getContractName(), _worsenPayupCondition)
        applyUpdates();
    }

    void recalculateSizesAndApplyUpdate() {
        if (!isEnabled() || _maxTicksWideBreached) {
            return;
        }

        bool hasChange = calculateBidQuoteSizes();
        hasChange |= calculateAskQuoteSizes();
        hasChange |= modifyHedgeQuote();

        if (!isInFillProcess() && hasChange) {
            notifyOrderRequest();
        }
    }

    bool validateParametersUpdate(const proto::GridCell& gridCell) {
        return true;
    }

    void addManualPosition(hf::qty_t size, uint32_t side, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        LOG(0, "MM {C[i1]}. Adding position. Side={i2} Size={i3}", getContractName(), side, size)
        hf::qty_t remaining = allocateHedgingFill(side, size, true, allocatedInternally, allocatedExternally);
        if (remaining > 0) {
            if (side == BID) {
                allocateHedgingFillRemainder<BID>(true, remaining, allocatedInternally, allocatedExternally);
            } else {
                allocateHedgingFillRemainder<ASK>(true, remaining, allocatedInternally, allocatedExternally);
            }
            LOG(0, "MM {C[i1]}. Allocated remainder. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        }
        recalculateSizesAndApplyUpdate();
    }

    uint32_t beginFillProcess() noexcept {
        auto fillId = _inFillProcess.fetch_add(1, std::memory_order_acq_rel) + 1;
        LOG(0, "MM {C[i1]}. Begin fill process {i2}", getContractName(), fillId)
        return fillId;
    }

private:
    bool isInFillProcess() const noexcept {
        return _inFillProcess.load(std::memory_order_relaxed) != _finishedFillProcess;
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

public:
    bool _contractCanTrade = false;
    bool _contractCanCancel = false;
    bool _gatewayActive = true;
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
    hf::price_t _hedgeOffsetPrice;
    hf::price_t _maxTicksWide;
    double _hedgeOffset;
    hf::qty_t _internallyInitiatedPosition = 0;
    hf::qty_t _externallyInitiatedPosition = 0;
    Hedge _hedges[2];
    Quote _quotes[2];
    ConfigContract _contract;
    hf::price_t _midPoint = 0;
    uint32_t _selfMatchCount = 0;
    double _worsenPayupCondition = 0;
    uint_fast64_t _lastMdSeqNum = 0;
    hf::qty_t _pendingOverFill = 0;

    Book _books[2];
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
