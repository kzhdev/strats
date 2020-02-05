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

template <typename IME_T>
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
        bool _inPayUp = false;

        void reset() {
            _externalPrice = hf::NULL_PRICE;
            _internalPrice = hf::NULL_PRICE;
            _sizeToQuote = 0;
            _inPayUp = false;
        }
    };

    void initialize(ConfigContract& contract, MarketMakerConfig& config, IME_T* executor, double worsenPayupCond) {
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
        _ime = executor;
        _contract = contract;
        _tickSize = contract._tickSize;
        _hedgeOffset = config._hedgeOffset;
        _hedgeOffsetPrice = _hedgeOffset * _tickSize;
        _toMarketMake = true;
        _internallyInitiatedPosition = config._internalPosition;
        _externallyInitiatedPosition = config._externalPosition;
        _worsenPayupCondition = worsenPayupCond;
    }

    hf::qty_t takeOverFill(uint32_t side, hf::qty_t overFillSize) {
        LOG(0, "Market maker {C[i1]}. Take overfill. Size={i2}", getContractName(), overFillSize)
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        hf::qty_t absExternalPosition = std::abs(_externallyInitiatedPosition);
        if (_maxExternallyInitiatedPosition <= absExternalPosition) {
            LOG(0, "Market maker {C[i1]}. External position reached or exceeded max, could not take anymore.", getContractName())
            return 0;
        }
        const hf::qty_t newPosition = overFillSize * multiplier + _externallyInitiatedPosition;
        if (std::abs(newPosition) > _maxExternallyInitiatedPosition) {
            hf::qty_t sizeTaken = _maxExternallyInitiatedPosition - absExternalPosition;
            _externallyInitiatedPosition += sizeTaken * multiplier;
            LOG(0, "Market maker {C[i1]}. Could not take the whole overfill. Took={i2}", getContractName(), sizeTaken)
            recalculateSizesAndApplyUpdate();
            return sizeTaken;
        }
        else {
            _externallyInitiatedPosition = newPosition;
            LOG(0, "Market maker {C[i1]}. Took the whole overfill. Took={i2}", getContractName(), overFillSize)
            recalculateSizesAndApplyUpdate();
            return overFillSize;
        }
    }

    void forceTakeOverFill(uint32_t side, hf::qty_t overFillSize) {
        LOG(0, "Market maker {C[i1]}. Forced to take overfill. Size={i2}", getContractName(), overFillSize)
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        _externallyInitiatedPosition += overFillSize * multiplier;
        LOG(0, "Market maker {C[i1]}. After forced to take fill. ExtInitPosition={i2} MaxExtInitPos={i3}", getContractName(), _externallyInitiatedPosition, _maxExternallyInitiatedPosition)
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
        return _enabled & _ime->_executor->isEnabled() & canContractTrade();
    }

    bool canQuoteExternally() {
        return _quoteExternal & _ime->_executor->canQuoteExternally();
    }

    bool canContractTrade() const {
        return _ime->_contractCanTrade;
    }

    bool hasMarketData(Book& bidBook, Book& askBook) {
        const bool missingMarketData = ((bidBook._price == hf::NULL_PRICE) | (askBook._price == hf::NULL_PRICE));
        if (BOOST_UNLIKELY(missingMarketData)) {
            if (_toMarketMake & _enabled) {
                _ime->_executor->sendErrorMessage(boost::str(boost::format("Re-quoting. Not having market data so turning market maker off. %1%") % _contract._name), ErrorType::Critical);
                LOG(0, "Market maker {C[i1]}. Lost market data. Disabling.", getContractName())
                disable();
            }
            return false;
        }
        return true;
    }

    void onChangeInOtherBooks(Book& bidBook, Book& askBook, bool forceUpdate = false) {
        const uint_fast64_t sequenceNumber = _ime->_executor->_contractStates[_contract._internalId]._mdSequenceNumber;
        LOG(0, "Market maker {C[i1]}. Recalculate. Bid: {i2}({i3})@{d1} Ask={i4}({i5})@{d2} SequenceNumber={i6}", getContractName(), bidBook._otherQuantity, bidBook._otherOrderCount, ToDollars(bidBook._price), askBook._otherQuantity, askBook._otherOrderCount, ToDollars(askBook._price), sequenceNumber)

        if (BOOST_UNLIKELY(!isEnabled())) {
            return;
        }

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }

        _midPoint = calculateMidPoint(bidBook, askBook);
        LOG(0, "Market maker {C[i1]}. Midpoint. Midpoint={d1}", getContractName(), ToDollars(_midPoint))

        bool somethingChanged = false;

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
                const hf::price_t internalPrice = bidBook._price - static_cast<hf::price_t>(_worsenPayupCondition * width);
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
                _ime->modifyQuoteBidOrder(quote._id, quote._internalPrice, quote._internalSize + quote._externalSize, quote._externalPrice, quote._externalSize, persistPrice);
                somethingChanged = true;
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
                const hf::price_t internalPrice = askBook._price + static_cast<hf::price_t>(_worsenPayupCondition * width);
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
                _ime->modifyQuoteAskOrder(quote._id, quote._internalPrice, quote._internalSize + quote._externalSize, quote._externalPrice, quote._externalSize, persistPrice);
                somethingChanged = true;
            }
        }

        if (isHedgingAsk()) {
            Hedge& hedge = _hedges[ASK];
            hedge._inPayUp = isInPayUp(hedge._sizeToQuote, bidBook);
            if (hedge._inPayUp) {
                const hf::price_t newAskPrice = bidBook._price;
                hedge._externalPrice = newAskPrice;
                _ime->modifyHedgeAskOrder(hedge._id, hedge._externalPrice - _commission, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, true);
                LOG(0, "Market maker {C[i1]}. ASK Paying up. Price={d1} Size={i2} InternalPosition={i3} ExternalPosition={i4}", getContractName(), ToDollars(hedge._externalPrice), hedge._sizeToQuote, _internallyInitiatedPosition, _externallyInitiatedPosition)
                somethingChanged = true;
            } else {
                const hf::price_t internalPrice = std::max<hf::price_t>(_quotes[BID]._internalPrice, bidBook._price) + _hedgeOffsetPrice;
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
                    LOG(0, "Market maker {C[i1]}. ASK Repriced hedge. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    _ime->modifyHedgeAskOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, false);
                    somethingChanged = true;
                }
            }
        }

        if (isHedgingBid()) {
            Hedge& hedge = _hedges[BID];
            hedge._inPayUp = isInPayUp(hedge._sizeToQuote, askBook);
            if (hedge._inPayUp) {
                const hf::price_t newBidPrice = askBook._price;
                hedge._externalPrice = newBidPrice;
                _ime->modifyHedgeBidOrder(hedge._id, hedge._externalPrice + _commission, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, true);
                LOG(0, "Market maker {C[i1]}. BID Paying up. Price={d1} Size={i2} InternalPosition={i3} ExternalPosition={i4}", getContractName(), ToDollars(hedge._externalPrice), hedge._sizeToQuote, _internallyInitiatedPosition, _externallyInitiatedPosition)
                somethingChanged = true;
            }
            else {
                const hf::price_t internalPrice = std::min<hf::price_t>(_quotes[ASK]._internalPrice, askBook._price) - _hedgeOffsetPrice;
                const bool priceChanged = std::abs(hedge._internalPrice - internalPrice) > _minInternalPriceChange;
                if (priceChanged) {
                    hedge._internalPrice = internalPrice;
                    hedge._externalPrice = RoundToTickSize(internalPrice - _commission, _tickSize, BID);
                    LOG(0, "Market maker {C[i1]}. BID Repriced hedge. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                    _ime->modifyHedgeBidOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, false);
                    somethingChanged = true;
                }
            }
        }

        if (somethingChanged | forceUpdate) {
            if (_ime->executeChanges(Order::Strategy::MarketMaker)) {
                sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_QUOTE, _quotes[BID]._internalPrice, _quotes[BID]._internalSize + _quotes[BID]._externalSize, _quotes[ASK]._internalPrice, _quotes[ASK]._internalSize + _quotes[ASK]._externalSize);
                sendHedgeUpdateToRelativeValue();
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
            allocatedInternally += remaining;
        } else {
            _externallyInitiatedPosition += signedRemaining;
            allocatedExternally += remaining;
        }
    }

    template <uint32_t Side>
    hf::qty_t doAllocateHedgingFill(hf::qty_t size, bool internal, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        hf::qty_t remaining = tryOffsetPosition<Side>(_internallyInitiatedPosition, size, allocatedInternally);
        LOG(0, "Market maker {C[i1]}. Offset internal position. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        if (remaining > 0) {
            remaining = tryOffsetPosition<Side>(_externallyInitiatedPosition, remaining, allocatedExternally);
            LOG(0, "Market maker {C[i1]}. Offset external position. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        }
        return remaining;
    }

    hf::qty_t allocateHedgingFill(uint32_t side, hf::qty_t size, bool internal, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        LOG(0, "Market maker {C[i1]}. Before Hedging Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
        hf::qty_t remaining;
        if (side == BID) {
            remaining = doAllocateHedgingFill<BID>(size, internal, allocatedInternally, allocatedExternally);
        } else {
            remaining = doAllocateHedgingFill<ASK>(size, internal, allocatedInternally, allocatedExternally);
            allocatedInternally *= -1;
            allocatedExternally *= -1;
        }
        LOG(0, "Market maker {C[i1]}. After Hedging Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
        if (internal && (allocatedInternally != 0 || allocatedExternally != 0)) {
            _hadInternalFill = true;
        } else {
            _hadExternalFill = true;
        }
        return remaining;
    }

    void onFill(Order* order, hf::price_t price, hf::qty_t size, bool internal, Book& bidBook, Book& askBook, bool isPayUp, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        bool isHedgingFill = order->_type == Order::Type::Hedging;
        LOG(0, "Market maker {C[i1]}. Fill. Hedging={i2} Price={d1} Qty={i3} Internal={i4} Side={i5} PayUp={i6}", getContractName(), isHedgingFill, ToDollars(price), size, internal, order->_side, isPayUp)
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
            LOG(0, "Market maker {C[i1]}. Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
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

    void modifyHedgeQuote(Book& bidBook, Book& askBook, bool forceOther = false) {
        hf::qty_t quoteSize = calculateHedgeQuoteSize();
        LOG(0, "Market maker {C[i1]}. Modifying hedge quote. Before. InternalPosition={i2} ExternalPosition={i3} QuoteSize={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, quoteSize)
        if (quoteSize > 0) {
            Hedge& hedge = _hedges[BID];
            hedge._sizeToQuote = quoteSize;
            hedge._inPayUp = calculateBidHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
            LOG(0, "Market maker {C[i1]}. BID Hedge order. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
            _ime->modifyHedgeBidOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, hedge._inPayUp);
            Hedge& otherHedge = _hedges[ASK];
            if ((otherHedge._sizeToQuote > 0) | forceOther) {
                otherHedge.reset();
                _ime->modifyHedgeAskOrder(otherHedge._id, otherHedge._internalPrice, otherHedge._sizeToQuote, otherHedge._externalPrice, otherHedge._sizeToQuote, false);
            }
        }
        else if (quoteSize < 0) {
            quoteSize = -quoteSize;
            Hedge& hedge = _hedges[ASK];
            hedge._sizeToQuote = quoteSize;
            hedge._inPayUp = calculateAskHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
            LOG(0, "Market maker {C[i1]}. ASK Hedge order. IntPrice={d1} ExtPrice={d2} Size={i2}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
            _ime->modifyHedgeAskOrder(hedge._id, hedge._internalPrice, hedge._sizeToQuote, hedge._externalPrice, hedge._sizeToQuote, hedge._inPayUp);
            Hedge& otherHedge = _hedges[BID];
            if ((otherHedge._sizeToQuote > 0) | forceOther) {
                otherHedge.reset();
                _ime->modifyHedgeBidOrder(otherHedge._id, otherHedge._internalPrice, otherHedge._sizeToQuote, otherHedge._externalPrice, otherHedge._sizeToQuote, false);
            }
        }
        else {
            Hedge& bidHedge = _hedges[BID];
            if ((bidHedge._sizeToQuote > 0) | forceOther) {
                bidHedge.reset();
                _ime->modifyHedgeBidOrder(bidHedge._id, bidHedge._internalPrice, bidHedge._sizeToQuote, bidHedge._externalPrice, bidHedge._sizeToQuote, false);
            }
            Hedge& askHedge = _hedges[ASK];
            if ((askHedge._sizeToQuote > 0) | forceOther) {
                askHedge.reset();
                _ime->modifyHedgeAskOrder(askHedge._id, askHedge._internalPrice, askHedge._sizeToQuote, askHedge._externalPrice, askHedge._sizeToQuote, false);
            }
        }

        LOG(0, "Market maker {C[i1]}. Modifying hedge quote. End. InternalPosition={i2} ExternalPosition={i3} QuoteSize={i5}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, quoteSize)

        sendHedgeUpdateToRelativeValue();
    }

    bool calculateBidHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, Book& bidBook, Book& askBook) {
        const bool inPayUp = isInPayUp(hedgeSize, askBook);
        if (inPayUp) {
            externalPrice = askBook._price;
            internalPrice = externalPrice + _commission;
        } else {
            _midPoint = calculateMidPoint(bidBook, askBook);
            internalPrice = _midPoint;
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
            internalPrice = _midPoint;
            externalPrice = RoundToTickSize(internalPrice + _commission, _tickSize, ASK);
        }
        return inPayUp;
    }

    void onInternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook, bool isPayUp, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        onFill(order, price, size, true, bidBook, askBook, isPayUp, allocatedInternally, allocatedExternally);
        _hadInternalFill = true;
    }

    void onExternalFill(Order* order, hf::price_t price, hf::qty_t size, Book& bidBook, Book& askBook, bool isPayUp, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        onFill(order, price, size, false, bidBook, askBook, isPayUp, allocatedInternally, allocatedExternally);
        _hadExternalFill = true;
    }

    void onEndOfFills() {
        auto& state = _ime->_executor->_contractStates[_contract._internalId];
        auto& bidBook = state._books[BID];
        auto& askBook = state._books[ASK];

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }

        modifyHedgeQuote(bidBook, askBook);
        calculateBidQuoteSizes();
        calculateAskQuoteSizes();

        if (_ime->executeChanges(Order::Strategy::MarketMaker)) {
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_QUOTE, _quotes[BID]._internalPrice, _quotes[BID]._internalSize + _quotes[BID]._externalSize, _quotes[ASK]._internalPrice, _quotes[ASK]._internalSize + _quotes[ASK]._externalSize);
            sendHedgeUpdateToRelativeValue();
        }
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
        auto& state = _ime->_executor->_contractStates[_contract._internalId];
        hf::qty_t externalSize = quote._externalSize;
        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSizeToJoinForBid(state._books[BID], quote._externalPrice)) {
                    externalSize = getBidExternalQuoteSize();
                }
            } else {
                if (!isThereEnoughSizeToStayForBid(state._books[BID], quote._externalPrice)) {
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
            _ime->modifyQuoteBidOrder(quote._id, quote._internalPrice, internalSize + externalSize, quote._externalPrice, quote._externalSize, quoteExternally);
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
        auto& state = _ime->_executor->_contractStates[_contract._internalId];
        hf::qty_t externalSize = quote._externalSize;

        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSizeToJoinForAsk(state._books[ASK], quote._externalPrice)) {
                    externalSize = getAskExternalQuoteSize();
                }
            } else {
                if (!isThereEnoughSizeToStayForAsk(state._books[ASK], quote._externalPrice)) {
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
            _ime->modifyQuoteAskOrder(quote._id, quote._internalPrice, internalSize + externalSize, quote._externalPrice, quote._externalSize, quoteExternally);
        }
    }

    void applyUpdates() {
        if (!isEnabled()) {
            return;
        }

        if (_toMarketMake) {
            if (_enabled) {
                auto& state = _ime->_executor->_contractStates[_contract._internalId];
                modifyHedgeQuote(state._books[BID], state._books[ASK]);
                onChangeInOtherBooks(state._books[BID], state._books[ASK], true);
            }
        }
    }

    bool isWithinPositionLimits() {
        bool withinLimits = true;
        if (std::abs(_internallyInitiatedPosition) > _maxInternallyInitiatedPosition) {
            _ime->_executor->sendErrorMessage(boost::str(boost::format("MM - at/over max internally initiated position. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "Market maker {C[i1]}. Internally initiated position is at/over limit", getContractName())
            withinLimits = false;
        }
        if (std::abs(_externallyInitiatedPosition) > _maxExternallyInitiatedPosition) {
            _ime->_executor->sendErrorMessage(boost::str(boost::format("MM - at/over max externally initiated position. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "Market maker {C[i1]}. Externally initiated position is at/over limit", getContractName())
            withinLimits = false;
        }
        return withinLimits;
    }

    bool hasMarketData() {
        auto& state = _ime->_executor->_contractStates[_contract._internalId];
        bool hasMD = ((state._books[BID]._price != hf::NULL_PRICE) & (state._books[ASK]._price != hf::NULL_PRICE));
        if (!hasMD) {
            _ime->_executor->sendErrorMessage(boost::str(boost::format("MM - does not have market data. Contract=%1%") % _contract._name), ErrorType::Critical);
            LOG(0, "Market maker {C[i1]}. does not have market data", getContractName())
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

    void start(Book& bidBook, Book& askBook) {
        if (_toMarketMake) {
            if (!canStart()) {
                return;
            }

            if(!isWithinPositionLimits()) {
                setQuoteExternalFlagToFalse();
                _ime->_executor->sendErrorMessage("MM - isWithinPositionLimits() check failed on start, disabling external quoting", ErrorType::Critical);
                _ime->notifyFrontMMExternalQuoteDisabled();
            }

            Quote& bidQuote = _quotes[BID];
            bidQuote._internalSize = getBidInternalQuoteSize();
            bidQuote._externalSize = 0;
            Quote& askQuote = _quotes[ASK];
            askQuote._internalSize = getAskInternalQuoteSize();
            askQuote._externalSize = 0;
            modifyHedgeQuote(bidBook, askBook);
            onChangeInOtherBooks(bidBook, askBook, true);
        }
    }

    void disable(bool changeFlag = true, bool skipSynch = false) {
        if (_toMarketMake) {
            _ime->getMarketMakerOutOfMarket(skipSynch);
            _quotes[BID].reset();
            _quotes[ASK].reset();
            _hedges[BID].reset();
            _hedges[ASK].reset();
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_QUOTE, _quotes[BID]._internalPrice, _quotes[BID]._internalSize + _quotes[BID]._externalSize, _quotes[ASK]._internalPrice, _quotes[ASK]._internalSize + _quotes[ASK]._externalSize);
            sendHedgeUpdateToRelativeValue();
            if (changeFlag) {
                _enabled = false;
                _ime->_executor->sendGenericMessageToFront(ViperMessageType::MarketMakerEnabled, _contract._internalId, 0);
            }
        }
    }

    bool setQuoteExternalFlagToFalse() {
        bool cachedValue = _quoteExternal;
        _quoteExternal = false;
        LOG(0, "Market maker {C[i1]}. Quote external changed. Value=false", getContractName());
        return cachedValue;
    }

    bool disableExternalQuoting() {
        bool oldValue = setQuoteExternalFlagToFalse();
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
            case MarketMakerParametersColumns::HedgeOffset: {
                double value = boost::lexical_cast<double>(gridCell.value());
                if (value <= 0.0) {
                    LOG(0, "Market maker {C[i1]}. HedgeOffset must be > 0. Set Value back to {d1}", getContractName(), _hedgeOffset)
                    _ime->_executor->sendErrorMessage("Market Maker HedgeOffset Parameter must > 0.", ErrorType::Normal);
                    gridCell.set_value(std::to_string(_hedgeOffset));
                } else {
                    _hedgeOffset =  value;
                    _hedgeOffsetPrice = value * _tickSize;
                    LOG(0, "Market maker {C[i1]}. HedgeOffset changed. Value={d1}", getContractName(), _hedgeOffset)
                    applyUpdates();
                }
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
                            auto& state = _ime->_executor->_contractStates[_contract._internalId];
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

    void handleWorsenPayupConditionUpdate(double value) {
        _worsenPayupCondition = value;
        LOG(0, "Market maker {C[i1]}. Worsen_payup_cond_percent changed Value={d1}", getContractName(), _worsenPayupCondition)
        applyUpdates();
    }

    void recalculateSizesAndApplyUpdate() {
        if (!isEnabled()) {
            return;
        }

        calculateBidQuoteSizes();
        calculateAskQuoteSizes();
        auto& state = _ime->_executor->_contractStates[_contract._internalId];
        modifyHedgeQuote(state._books[BID], state._books[ASK]);

        if (_ime->executeChanges(Order::Strategy::MarketMaker)) {
            sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_QUOTE, _quotes[BID]._internalPrice, _quotes[BID]._internalSize + _quotes[BID]._externalSize, _quotes[ASK]._internalPrice, _quotes[ASK]._internalSize + _quotes[ASK]._externalSize);
            sendHedgeUpdateToRelativeValue();
        }
    }

    bool validateParametersUpdate(const proto::GridCell& gridCell) {
        return true;
    }

    bool handleLogin() {
        return true;
    }

    void sendMarketUpdateToRelativeValue(uint32_t internalId, OrderIds id, hf::price_t bidPrice, hf::qty_t bidSize, hf::price_t askPrice, hf::qty_t askSize) {
        _ime->_executor->sendMarketUpdateToRelativeValue(internalId, id, bidPrice, bidSize, askPrice, askSize);
    }

    void sendHedgeUpdateToRelativeValue() {
        hf::price_t bidPrice = hf::NULL_PRICE;
        hf::price_t askPrice = hf::NULL_PRICE;
        hf::qty_t bidSize = 0;
        hf::qty_t askSize = 0;
        if (!_hedges[BID]._inPayUp) {
            bidPrice = _hedges[BID]._internalPrice;
            bidSize = _hedges[BID]._sizeToQuote;
        }

        if (!_hedges[ASK]._inPayUp) {
            askPrice = _hedges[ASK]._internalPrice;
            askSize = _hedges[ASK]._sizeToQuote;
        }

        _ime->_executor->sendMarketUpdateToRelativeValue(_contract._internalId, OrderIds::ORDER_ID_MM_HEDGE, bidPrice, bidSize, askPrice, askSize);
    }

    void addManualPosition(hf::qty_t size, uint32_t side, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        hf::qty_t remaining = allocateHedgingFill(side, size, true, allocatedInternally, allocatedExternally);
        if (remaining > 0) {
            if (side == BID) {
                allocateHedgingFillRemainder<BID>(true, remaining, allocatedInternally, allocatedExternally);
            } else {
                allocateHedgingFillRemainder<ASK>(true, remaining, allocatedInternally, allocatedExternally);
            }
            LOG(0, "Market maker {C[i1]}. Allocated remainder. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
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
    hf::price_t _hedgeOffsetPrice;
    double _hedgeOffset;
    hf::qty_t _internallyInitiatedPosition = 0;
    hf::qty_t _externallyInitiatedPosition = 0;
    Hedge _hedges[2];
    Quote _quotes[2];
    IME_T* _ime;
    ConfigContract _contract;
    bool _toMarketMake = false;
    hf::price_t _midPoint = 0;
    bool _hadInternalFill = false;
    bool _hadExternalFill = false;
    uint32_t _selfMatchCount = 0;
    double _worsenPayupCondition = 0;
};

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
