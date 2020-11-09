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

namespace Python {
class SimpleMarketMaker;
}

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

template<uint32_t Side>
inline hf::price_t calculatePriceFromMidPointAndWidth(hf::price_t midPoint, hf::price_t width);

template<>
inline hf::price_t calculatePriceFromMidPointAndWidth<Python::BID>(hf::price_t midPoint, hf::price_t width) {
    return midPoint - width;
}

template<>
inline hf::price_t calculatePriceFromMidPointAndWidth<Python::ASK>(hf::price_t midPoint, hf::price_t width) {
    return midPoint + width;
}

template<uint32_t Side>
inline hf::price_t applyPriceOffset(hf::price_t price, hf::price_t offset);

template<>
inline hf::price_t applyPriceOffset<Python::BID>(hf::price_t price, hf::price_t offset) { return price + offset; }

template<>
inline hf::price_t applyPriceOffset<Python::ASK>(hf::price_t price, hf::price_t offset) { return price - offset; }

template<uint32_t Side>
inline hf::price_t calculateWorsenPrice(hf::price_t price, hf::price_t worsenPayupConditionAmount);

template<>
inline hf::price_t calculateWorsenPrice<Python::BID>(hf::price_t price, hf::price_t worsenPayupConditionAmount) {
    return price - worsenPayupConditionAmount;
}

template<>
inline hf::price_t calculateWorsenPrice<Python::ASK>(hf::price_t price, hf::price_t worsenPayupConditionAmount) {
    return price + worsenPayupConditionAmount;
}

template<uint32_t>
inline hf::price_t calculatePayUpInternalPrice(hf::price_t ordinaryPrice, hf::price_t worsenPrice);

template<>
inline hf::price_t calculatePayUpInternalPrice<Python::BID>(hf::price_t ordinaryPrice, hf::price_t worsenPrice) {
    return std::min(ordinaryPrice, worsenPrice);
}

template<>
inline hf::price_t calculatePayUpInternalPrice<Python::ASK>(hf::price_t ordinaryPrice, hf::price_t worsenPrice) {
    return std::max(ordinaryPrice, worsenPrice);
}

template<uint32_t Side>
inline hf::qty_t getInternalQuoteSize(Python::SimpleMarketMaker& mm, hf::qty_t maxQuoteSize);

template<uint32_t Side>
inline hf::qty_t getMaxQuoteSizeBasedOnMarket(Python::SimpleMarketMaker& mm, hf::price_t price);

template<uint32_t Side>
inline hf::qty_t getExternalQuoteSize(Python::SimpleMarketMaker& mm, hf::qty_t maxQuoteSize);

template<uint32_t Side>
inline hf::price_t roundToTickSize(Python::SimpleMarketMaker& mm, hf::price_t price);

template<uint32_t Side>
inline bool isThereEnoughSizeToJoin(Python::SimpleMarketMaker& mm, hf::price_t quotePrice);

template<uint32_t Side>
inline bool isThereEnoughSizeToStay(Python::SimpleMarketMaker& mm, hf::price_t quotePrice);

}   // end of anonymous namespace

namespace Python {

class SimpleMarketMaker {
    friend class MarketMaker;
    OrderRequestManager& _orderRequestManager = OrderRequestManager::get();
    MarketMaker* _marketMaker;
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
                    MarketMaker* marketMaker) {
        _marketMaker = marketMaker;
        _enabled = config._enabled;
        _quoteExternal = config._quoteExternal;
        _quoteSize = config._quoteSize;
        _pctTobMaxQuote = config._quotePctTob;
        _widthPercentage = config._widthPercentage[toUInt(Shifts::__current_marker__)];
        _joinSize = config._joinSize;
        _leaveSize = config._leaveSize;
        _payUpQuantity = config._payUpQty;
        _payUpOrders = config._payUpOrders;
        _payUpMultiple = config._payUpMultiple;
        _maxExternallyInitiatedPosition = config._maxPosExternal;
        _maxInternallyInitiatedPosition = config._maxPosInternal;
        _commission = ToRhonePrice(config._commission);
        _emaEstLevel = config._establishedLevel;
        _estLevelEmaPeriod = GetCyclesFromSeconds(config._estLevelEMASeconds, _marketMaker->_frequency);
        _estLevelPctEma = config._estLevelPctEMA;
        _minInternalPriceChange = ToRhonePrice(config._minInternalPriceChange);
        _contract = contract;
        _tickSize = contract._tickSize;
        _hedgeOffset = config._hedgeOffset;
        _hedgeOffsetPrice = _hedgeOffset * _tickSize;
        _maxTicksWide = config._maxTicksWide < 0 ? std::numeric_limits<int64_t>::max() : config._maxTicksWide * _tickSize;
        _internallyInitiatedPosition = config._internalPosition;
        _externallyInitiatedPosition = config._externalPosition;
        _worsenPayupConditionAmount = config._worsenPayupCondPercent * _tickSize;
        _queueImportance = config._queueImportance;

        auto netPos = _internallyInitiatedPosition + _externallyInitiatedPosition;
        if (netPos) {
            _marketMaker->sendFillToFront(contract._internalId, (netPos > 0 ? BID : ASK),
                                          _marketMaker->_viperConfig._mmLastFillPrices[contract._marketMakeIndex],
                                          std::abs(netPos), FillType::Phantom, FillSource::ViperStart,
                                          _internallyInitiatedPosition, _externallyInitiatedPosition);
        }
    }

    void takeOverFill(const FillMessage& fill) {
        if (fill._forceOverFill) {
            forceTakeOverFill(fill._side, fill._price, fill._qty, fill._source, fill._checkLimit);
        } else {
            hf::qty_t remainingSize = fill._qty;
            auto takenSize = takeOverFill(fill._side, fill._qty);
            if (takenSize > 0) {
                _marketMaker->sendFillToFront(fill._contractId, fill._side, fill._price, fill._qty, fill._type, fill._source, 0, takenSize);
            }
            remainingSize -= takenSize;
            if (remainingSize > 0) {
                bool overFillSendToOtherStrategy = false;
                for (uint32_t i = toUInt(StrategyId::RelativeValue); i < toUInt(StrategyId::Execution); ++i) {
                    if (_marketMaker->_strategies[i]->isEnabled()) {
                        _marketMaker->_strategies[i]->sendMessage<FillMessage>(
                                ViperMessageType::Fill,
                                [&fill, remainingSize, i](FillMessage& message) {
                                    message._contractId = fill._contractId;
                                    message._side = fill._side;
                                    message._qty = remainingSize;
                                    message._strategy = (StrategyId) i;
                                    message._type = FillType::External;
                                    message._price = fill._price;
                                    message._source = FillSource::Overfill;
                                    message._internalAllocation = 0;
                                    message._externalAllocation = 0;
                                    message._forceOverFill = false;
                                });
                        overFillSendToOtherStrategy = true;
                        break;
                    }
                }

                if (!overFillSendToOtherStrategy) {
                    forceTakeOverFill(fill._side, fill._price, remainingSize, FillSource::Overfill);
                    disableExternalQuoting();

                    std::string message = boost::str(
                            boost::format("Disabled MM quote external. Contract=%1% Reason=External overfill. remainingSize after MM takes=%2% Price=%3%") %
                            _contract._name % remainingSize % ToDollars(fill._price));
                    _marketMaker->sendErrorMessage(message, ErrorType::Critical);
                    LOG(0, "MM {C[i1]}. Disabling external quoting. remainingSize after MM takes={i2} Price={d1}",
                        getContractName(), remainingSize, ToDollars(fill._price))
                }
            }
        }
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

    void forceTakeOverFill(uint32_t side, hf::price_t price, hf::qty_t overFillSize, FillSource source, bool checkLimit = false) {
        LOG(0, "MM {C[i1]}. Forced to take overfill. Size={i2} Source={i3}", getContractName(), overFillSize, toUInt(source))
        const hf::qty_t multiplier = side == BID ? 1 : -1;
        LOG(0, "MM {C[i1]}. Before forced to take fill. ExtInitPosition={i2} MaxExtInitPos={i3}", getContractName(), _externallyInitiatedPosition, _maxExternallyInitiatedPosition)
        _externallyInitiatedPosition += overFillSize * multiplier;
        LOG(0, "MM {C[i1]}. After forced to take fill. ExtInitPosition={i2} MaxExtInitPos={i3}", getContractName(), _externallyInitiatedPosition, _maxExternallyInitiatedPosition)

        _marketMaker->sendFillToFront(_contract._internalId, side, price, overFillSize, FillType::External, source, 0, overFillSize);
        if (checkLimit && !isWithinPositionLimits()) {
            disableExternalQuoting();
        }
    }

    bool isHedgingAsk() {
        return _hedges[ASK]._sizeToQuote != 0;
    }

    bool isHedgingBid() {
        return _hedges[BID]._sizeToQuote != 0;
    }

    hf::qty_t getBidExternalQuoteSize(hf::qty_t maxQuoteSize) {
        if (_externallyInitiatedPosition < 0) {
            return std::min(_quoteSize, maxQuoteSize);
        }
        if (_maxExternallyInitiatedPosition <= _externallyInitiatedPosition) {
            return 0;
        }
        return std::min(_quoteSize, std::min(_maxExternallyInitiatedPosition - _externallyInitiatedPosition, maxQuoteSize));
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

        if (BOOST_LIKELY((bidBook._otherQuantity > 0) && (askBook._otherQuantity > 0))) {
            calculateEstablishedLevelEma((double)(bidBook._otherQuantity + askBook._otherQuantity) / 2);
        }

        if (BOOST_UNLIKELY(!isEnabled())) {
            return;
        }

        bool hasChange = false;
        bool maxTickWideAlreadyBreached = _maxTicksWideBreached;
        if (BOOST_UNLIKELY((askBook._price - bidBook._price) > _maxTicksWide)) {
            LOG(0, "MM {C[i1]}. MaxTicksWidth breached, disable external quoting, disable GTC.", getContractName())
            std::string msg = boost::str(boost::format("MM - MaxTicksWidth breached, disable external quoting, disable GTC. Contract=%1%") % _contract._name);
            disableExternalQuotingAndUseGTC(msg);
            _maxTicksWideBreached = true;
        } else if (_maxTicksWideBreached) {
            _maxTicksWideBreached = false;
        }

        _midPoint = calculateMidPoint(bidBook, askBook);
        LOG(0, "MM {C[i1]}. Midpoint. Midpoint={d1}", getContractName(), ToDollars(_midPoint))

        if (BOOST_LIKELY(!_maxTicksWideBreached)) {
            const bool quoteExternally = canQuoteExternally();
            const double marketSpread = askBook._price - bidBook._price;
            _width = static_cast<hf::price_t>(marketSpread / 2.0 * _widthPercentage);
            hasChange |= updateQuote<BID>(quoteExternally, forceUpdate);
            hasChange |= updateQuote<ASK>(quoteExternally, forceUpdate);
        } else if (!maxTickWideAlreadyBreached) {
            _quotes[BID].reset();
            _quotes[ASK].reset();
            modifyQuoteOrder<BID, 2>(hf::NULL_PRICE, 0, hf::NULL_PRICE, 0, false, false);
            modifyQuoteOrder<ASK, 3>(hf::NULL_PRICE, 0, hf::NULL_PRICE, 0, false, false);
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

        if (hasChange | hedgeSizeChanged) {
            notifyOrderRequest();
        }
    }

    template<uint32_t Side>
    bool updateQuote(bool quoteExternally, bool forceUpdate = false) {
        auto quoteBasedOnOrders = calculateQuotePriceAndSizeByOrders<Side>();
        LOG(0, "MM {C[i1]}. Quote based on orders: price={d1} qty={i1}", getContractName(), ToDollars(quoteBasedOnOrders.first), quoteBasedOnOrders.second)
        auto& quote = _quotes[Side];
        const bool isBookInPayUpMode = isInPayUp(getExternalQuoteSize<Side>(*this, getMaxQuoteSizeBasedOnMarket<Side>(*this, quote._externalPrice)), _books[Side]);
        bool changeInPrice = false;
        bool hasChange = false;

        if (!isBookInPayUpMode) {
            const hf::price_t internalPriceChange = quoteBasedOnOrders.first - quote._internalPrice;
            changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
            if (changeInPrice | forceUpdate) {
                quote._internalPrice = quoteBasedOnOrders.first;
                if (quoteExternally) {
                    quote._externalPrice = roundToTickSize<Side>(*this, quote._internalPrice);
                    if (IsBetterPrice<Side>(quote._externalPrice, _books[Side]._price)) {
                        quote._externalPrice = _books[Side]._price;
                    }
                }
            }
        }
        else {
            const hf::price_t worsenPrice = calculateWorsenPrice<Side>(_books[Side]._price, _worsenPayupConditionAmount);
            const hf::price_t ordinaryPrice = calculatePriceFromMidPointAndWidth<Side>(_midPoint, _width);
            const hf::price_t internalPrice = calculatePayUpInternalPrice<Side>(ordinaryPrice, worsenPrice);
            changeInPrice = internalPrice != quote._internalPrice;
            if (changeInPrice | forceUpdate) {
                quote._internalPrice = internalPrice;
                if (quoteExternally) {
                    quote._externalPrice = roundToTickSize<Side>(*this, quote._internalPrice);
                }
            }
        }

        bool changeInSize = false;
        hf::qty_t maxQuoteSizeBasedOnMarket = getMaxQuoteSizeBasedOnMarket<Side>(*this, quote._externalPrice);
        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSizeToJoin<Side>(*this, quote._externalPrice)) {
                    quote._externalSize = std::min<hf::qty_t>(getExternalQuoteSize<Side>(*this, maxQuoteSizeBasedOnMarket), quoteBasedOnOrders.second);
                    changeInSize |= quote._externalSize != 0;
                }
            } else {
                if (!isThereEnoughSizeToStay<Side>(*this, quote._externalPrice)) {
                    quote._externalSize = 0;
                    changeInSize = true;
                }
                else {
                    hf::qty_t candidateSize = std::min<hf::qty_t>(getExternalQuoteSize<Side>(*this, maxQuoteSizeBasedOnMarket), quoteBasedOnOrders.second);
                    if(candidateSize != quote._externalSize) {
                        quote._externalSize = candidateSize;
                        changeInSize = true;
                    }
                }
            }
        } else {
            changeInSize |= quote._externalSize != 0;
            quote._externalSize = 0;
        }

        hf::qty_t candidateInternalSize = getInternalQuoteSize<Side>(*this, maxQuoteSizeBasedOnMarket);
        if(candidateInternalSize != quote._internalSize) {
            quote._internalSize = candidateInternalSize;
            changeInSize = true;
        }

        if (changeInPrice | changeInSize | forceUpdate) {
            modifyQuoteOrder<Side, 0>(quote._internalPrice, quote._internalSize + quote._externalSize,
                                     quote._externalPrice, quote._externalSize, isBookInPayUpMode,
                                     quoteExternally);
            hasChange = true;
        }
        return hasChange;
    }

    template<uint32_t Side>
    std::pair<hf::price_t, hf::qty_t> calculateQuotePriceAndSizeByOrders() {
        hf::price_t price = calculatePriceFromMidPointAndWidth<Side>(_midPoint, _width);
        DLOG(0, "MM {C[i1]}. calculateQuotePriceAndSizeByOrders Side={i2} TOB={d1} price={d2}", getContractName(), Side, ToDollars(_books[Side]._price), ToDollars(price));
        if (BOOST_UNLIKELY(_establishedLevel == 0)) {
            DLOG(0, "MM {C[i1]}. establishedLevel == 0, bypass queue position evaluation", getContractName())
            return std::make_pair(price, std::numeric_limits<hf::qty_t>::max());
        }
        if (_queueImportance == 0) {
            DLOG(0, "MM {C[i1]}. pctQueueImportance == 0, bypass queue position evaluation",getContractName())
            return std::make_pair(price, std::numeric_limits<hf::qty_t>::max());
        }
        auto iter = _priceOrders[Side].find(_books[Side]._price);
        if (iter == _priceOrders[Side].end() || iter->second.empty()) {
            DLOG(0, "MM {C[i1]}. No orders placed at {d1} yet, assume qp at the back", getContractName(), ToDollars(_books[Side]._price));
            double pctThroughQueue = std::max<double>(std::min<double>(1.0 - (double)_books[Side]._otherQuantity / _establishedLevel, 1.0), 0.0);
            double offsetScale = (2.0 / ( 1.0 + std::exp( -(double)_queueImportance * (pctThroughQueue - 0.5) ) ) - 1.0);
            hf::price_t offset = offsetScale * _width;
            auto internalPrice = applyPriceOffset<Side>(price, offset);
            DLOG(0, "MM {C[i1]}. calculateQuotePriceAndSizeByOrders. qp={i2} establishedLevel={i3} queueImportance={i4} pctThroughQueue={d1} offsetScale={d2} offset={d3} internalPrice={d4}", getContractName(),
                 _books[Side]._otherQuantity, _establishedLevel, _queueImportance, pctThroughQueue, offsetScale, ToDollars(offset), ToDollars(internalPrice))
            return std::make_pair(internalPrice, std::numeric_limits<hf::qty_t>::max());
        }

        hf::price_t s_tob = 0, s = 0;
        hf::qty_t q_tob = 0, q = 0;
        hf::price_t best_price = hf::NULL_PRICE;
        auto& orders = iter->second;
        for (auto* order : orders) {
            auto qp = order->qp;
            if (order->qp < 0) {
                DLOG(0, "MM {C[i1]}. Order {i2} queue position not updated yet, assume at the back", getContractName(), order->orderId);
                qp = _books[Side]._otherQuantity;
            }
            double pctThroughQueue = std::max<double>(std::min<double>(1.0 - (double)qp / _establishedLevel, 1.0), 0.0);
            double offsetScale = (2 / (1 + std::exp(-(double)_queueImportance * (pctThroughQueue - 0.5))) - 1);
            hf::price_t offset = offsetScale * _width;
            hf::price_t internalPrice = applyPriceOffset<Side>(price, offset);
            hf::price_t externalPrice = roundToTickSize<Side>(*this, internalPrice);
            DLOG(0, "MM {C[i1]}. calculateQuotePriceAndSizeByOrders. Order {i2} ord_price={d1} qty={i3} qp={i4} establishedLevel={i5} queueImportance={i6} pctThroughQueue={d2} offsetScale={d3} offset={d4} internalPrice={d5} roundedPrice={d6}", getContractName(),
                 order->orderId, ToDollars(order->price), order->qty, qp, _establishedLevel, _queueImportance, pctThroughQueue, offsetScale, ToDollars(offset), ToDollars(internalPrice), ToDollars(externalPrice))
            if (IsPriceBetterOrEqual<Side>(externalPrice, _books[Side]._price)) {
                s_tob += internalPrice * order->qty;
                q_tob += order->qty;
            }
            else if (!q_tob) {
                // if all orders fell off the TOB, use orders with best price
                if (best_price == hf::NULL_PRICE || IsBetterPrice<Side>(externalPrice, best_price)) {
                    best_price = externalPrice;
                    s = 0;
                    q = 0;
                }
                if (best_price == externalPrice) {
                    s += internalPrice * order->qty;
                    q += order->qty;
                }
            }
        }

        return q_tob ? std::make_pair(s_tob/q_tob, q_tob) : std::make_pair(s/q, q);
    }

    void notifyOrderRequest() {
        _orderRequestManager.notifyOrderRequest(_contract._internalId, StrategyId::MarketMaker, _lastOrderRequests);
    }

    template<uint32_t Side, uint32_t source>
    void modifyQuoteOrder(hf::price_t internalPrice,
                          hf::qty_t internalSize,
                          hf::price_t externalPrice,
                          hf::qty_t externalSize,
                          bool inPayUp,
                          bool persist) {
        _lastOrderRequests._quote[Side] = _orderRequestManager.sendQuoteRequest(_contract._internalId,
                                                                                QUOTE_ORDER_ID,
                                                                                Side, internalPrice, internalSize,
                                                                                externalPrice,
                                                                                externalSize, persist);
        LOG(0, "MM {C[i1]}. Update Quote. Side={i2} Internal: {i3}@{d1} External: {i4}@{d2} inPayUp={i5} persist={i6} reqId={i7} source={i8}",
             getContractName(), Side, internalSize, ToDollars(internalPrice), externalSize, ToDollars(externalPrice),
             inPayUp, persist, _lastOrderRequests._quote[Side], source)
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
        _establishedLevel = std::max<hf::qty_t>(_emaEstLevel * _estLevelPctEma, 1);
        //Weighted average in ungapped Market
        if(bid._price + _tickSize == ask._price) {
            const hf::qty_t askSize = std::min(ask._otherQuantity, _establishedLevel);
            const hf::qty_t bidSize = std::min(bid._otherQuantity, _establishedLevel);
            return (ask._price * bidSize + bid._price * askSize) / (bidSize + askSize);
        }
        return (bid._price + ask._price) / 2;
    }

    void calculateEstablishedLevelEma(double qty) {
        const uint64_t now = clockUtil::GetCycles();
        if (BOOST_LIKELY(_lastEmaTime && (_estLevelEmaPeriod > 0))) {
            auto timeInCycles = now - _lastEmaTime;
            const double alpha = static_cast<double>(timeInCycles) / _estLevelEmaPeriod;
            const double mu = std::exp(-alpha);
            const double nu = (1 - mu) / alpha;
            const double lastEma = _emaEstLevel;
            const double newEma = mu * lastEma + (nu - mu) * _lastTobQty + (1 - nu) * qty;
            if(BOOST_LIKELY(std::isfinite(newEma))) {
                if (BOOST_UNLIKELY((newEma < 1) && (lastEma >= 1))) {
                    LOG(0, "timeInCycles={i1} alpha={d1} mu={d2} nu={d3} lastQty={d4} qty={d5} lastEma={d6} newEma={d7}", timeInCycles,
                        alpha, mu, nu, _lastTobQty, qty, lastEma, newEma)
                }
                _emaEstLevel = newEma;
            }
        }
        _lastTobQty = qty;
        _lastEmaTime = now;
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
    hf::qty_t doAllocateHedgingFill(hf::qty_t size, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        hf::qty_t remaining = tryOffsetPosition<Side>(_internallyInitiatedPosition, size, allocatedInternally);
        LOG(0, "MM {C[i1]}. Offset internal position. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        if (remaining > 0) {
            remaining = tryOffsetPosition<Side>(_externallyInitiatedPosition, remaining, allocatedExternally);
            LOG(0, "MM {C[i1]}. Offset external position. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        }
        return remaining;
    }

    hf::qty_t allocateHedgingFill(uint32_t side, hf::qty_t size, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        LOG(0, "MM {C[i1]}. Allocate Hedging Fill. Side={i2} Size={i3}", getContractName(), side, size)
        LOG(0, "MM {C[i1]}. Before Hedging Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
        hf::qty_t remaining;
        if (side == BID) {
            remaining = doAllocateHedgingFill<BID>(size, allocatedInternally, allocatedExternally);
        } else {
            remaining = doAllocateHedgingFill<ASK>(size, allocatedInternally, allocatedExternally);
            allocatedInternally *= -1;
            allocatedExternally *= -1;
        }
        LOG(0, "MM {C[i1]}. After Hedging Fill. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        return remaining;
    }

    void onFill(const FillMessage& fill) {
        if (fill._source != FillSource::Quote && fill._source != FillSource::Hedge) {
            if (fill._strategy != StrategyId::MarketMaker) {
                LOG(0, "MM {C[i1]}. Handling fill returned from strategy {i2}. Side={i3} Price={d1} Size={i4}",
                    getContractName(), strategyName(fill._strategy), fill._side, ToDollars(fill._price), fill._qty)
                forceTakeOverFill(fill._side, fill._price, fill._qty, fill._source, true);
                onEndOfFills();
            } else {
                takeOverFill(fill);
            }
            return;
        }

        hf::qty_t allocatedInternally = 0;
        hf::qty_t allocatedExternally = 0;

        bool isHedgingFill = fill._source == FillSource::Hedge;
        bool internal = fill._type == FillType::Internal;
        LOG(0, "MM {C[i1]}. Fill. Hedging={i2} Price={d1} Qty={i3} Internal={i4} Side={i5} Source={i6}", getContractName(),
            isHedgingFill, ToDollars(fill._price), fill._qty, internal, fill._side, toUInt(fill._source));
        if (!isHedgingFill) {
            Quote& quote = _quotes[fill._side];
            hf::qty_t remainingSize = fill._qty;
            if (internal) {
                if (fill._side == BID) {
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
                        allocateExternalQuoteFill(quote, overfill, fill._side);
                        allocatedExternally += overfill;
                        remainingSize = quote._internalSize;
                    }
                    quote._internalSize -= remainingSize;
                    if (fill._side == BID) {
                        _internallyInitiatedPosition += remainingSize;
                    } else {
                        _internallyInitiatedPosition -= remainingSize;
                    }
                    allocatedInternally += remainingSize;
                }
            } else {
                if (fill._side == BID) {
                    remainingSize = tryOffsetPosition<BID>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
                    remainingSize = tryOffsetPosition<BID>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
                }
                else {
                    remainingSize = tryOffsetPosition<ASK>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
                    remainingSize = tryOffsetPosition<ASK>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
                }

                if (remainingSize > 0) {
                    allocateExternalQuoteFill(quote, remainingSize, fill._side);
                    allocatedExternally += remainingSize;
                }
            }
            LOG(0, "MM {C[i1]}. Fill. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
        }
        else {
            auto remaining = allocateHedgingFill(fill._side, fill._qty, allocatedInternally, allocatedExternally);
            if (remaining > 0) {
                if (fill._side == BID) {
                    allocateHedgingFillRemainder<BID>(internal, remaining, allocatedInternally, allocatedExternally);
                } else {
                    allocateHedgingFillRemainder<ASK>(internal, remaining, allocatedInternally, allocatedExternally);
                }
            }
        }
        _marketMaker->sendFillToFront(fill._contractId, fill._side, fill._price, fill._qty, fill._type, fill._source, allocatedInternally, allocatedExternally);
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

    bool modifyHedgeQuote(bool updateSizeOnly, bool forceOther = false) {
        auto& bidBook = _books[BID];
        auto& askBook = _books[ASK];
        bool hasHedge = false;
        hf::qty_t quoteSize = calculateHedgeQuoteSize();
        LOG(0, "MM {C[i1]}. Modifying hedge quote. Before. InternalPosition={i2} ExternalPosition={i3} QuoteSize={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, quoteSize)
        if (quoteSize > 0) {
            Hedge& hedge = _hedges[BID];
            hedge._sizeToQuote = quoteSize;
            if (!updateSizeOnly) {
                hedge._inPayUp = calculateBidHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
                LOG(0, "MM {C[i1]}. BID Hedge order. IntPrice={d1} ExtPrice={d2} Size={i2} inPayUp={i3}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote, hedge._inPayUp)
                modifyHedgeOrder<BID>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, hedge._inPayUp);
                hasHedge = true;
            }
            Hedge& otherHedge = _hedges[ASK];
            if ((otherHedge._sizeToQuote > 0) | forceOther) {
                otherHedge.reset();
                modifyHedgeOrder<ASK>(otherHedge._internalPrice, otherHedge._externalPrice, otherHedge._sizeToQuote, false);
                hasHedge = true;
            }
        }
        else if (quoteSize < 0) {
            quoteSize = -quoteSize;
            Hedge& hedge = _hedges[ASK];
            hedge._sizeToQuote = quoteSize;
            if (!updateSizeOnly) {
                hedge._inPayUp = calculateAskHedgePrices(hedge._sizeToQuote, hedge._internalPrice, hedge._externalPrice, bidBook, askBook);
                LOG(0, "MM {C[i1]}. ASK Hedge order. IntPrice={d1} ExtPrice={d2} Size={i2} inPayUp={i3}", getContractName(), ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote, hedge._inPayUp)
                modifyHedgeOrder<ASK>(hedge._internalPrice, hedge._externalPrice, hedge._sizeToQuote, hedge._inPayUp);
                hasHedge = true;
            }
            Hedge& otherHedge = _hedges[BID];
            if ((otherHedge._sizeToQuote > 0) | forceOther) {
                otherHedge.reset();
                modifyHedgeOrder<BID>(otherHedge._internalPrice, otherHedge._externalPrice, otherHedge._sizeToQuote, false);
                hasHedge = true;
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
        DLOG(0, "MM {C[i1]}. Send hedge. Side={i2} Size={i3} InternalPrice={d1} ExternalPrice={d2} PayUp={i4} reqId={i5}",
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

    void onEndOfFills(bool isManualFill = false) {
        LOG(0, "MM {C[i1]}. EndOfFill. internalPosition={i2} externalPosition={i3}", getContractName(),
            _internallyInitiatedPosition, _externallyInitiatedPosition)

        //Likely offline GTC fill
        if(BOOST_UNLIKELY(!isManualFill && !(_marketMaker->isEnabled() || _marketMaker->isRVEnabled()))) {
            [this]() NEVER_INLINE COLD {
                LOG(0, "MM {C[i1]}. Fill received while MM and RV disabled (maybe a gtc?). Check fills window!", getContractName());
                _marketMaker->sendErrorMessage("Market maker " + _contract._name + ". Fill received while MM and RV disabled. Check fills window!", ErrorType::Critical);
            }();
            return;
        }

        recalculateSizesAndApplyUpdate();
    }

    void onEndOfExternalFills(uint32_t side) {
        LOG(0, "MM {C[i1]}. Side={i2} End of external fills.", getContractName(), side)
        onEndOfFills();
    }

    void onEndOfInternalFills() {
        LOG(0, "MM {C[i1]}. End of internal fills. FillId={i2}", getContractName())
        onEndOfFills();
    }

    void applyUpdates() {
        if (!isEnabled() || !_contract._toMarketMake) {
            return;
        }

        bool hasHedgeChange = modifyHedgeQuote(true);
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
            bidQuote._externalSize = 0;
            Quote& askQuote = _quotes[ASK];
            askQuote._externalSize = 0;
            bool hasHedge = modifyHedgeQuote(true);
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
            case MarketMakerParametersColumns::QuotePctTob: {
                double candidateValue = boost::lexical_cast<double>(gridCell.value().c_str());

                if(candidateValue < 0) {
                    LOG(0, "MM {C[i1]}. PctTobMaxQuoteChange rejected, value<0. oldValue={d1} candidateValue={d2}", getContractName(), _pctTobMaxQuote, candidateValue);
                    _marketMaker->sendErrorMessage("MM %TobMaxQuoteChange must be >0", ErrorType::Critical);
                    gridCell.set_value(std::to_string(_pctTobMaxQuote));
                }
                else {
                    _pctTobMaxQuote = candidateValue;
                    LOG(0, "MM {C[i1]}. PctTobMaxQuote changed.  Value={i2}", getContractName(), candidateValue);
                    recalculateSizesAndApplyUpdate();
                }
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
            case MarketMakerParametersColumns::SafeModeWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. SafeModeWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::SafeMode) {
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
            case MarketMakerParametersColumns::EstLevelEMASeconds: {
                auto value = boost::lexical_cast<uint32_t>(gridCell.value());
                _estLevelEmaPeriod = GetCyclesFromSeconds(value, _marketMaker->_frequency);
                LOG(0, "MM {C[i1]}. Established level EMA seconds changed. Value={i2}", getContractName(), value)
                applyUpdates();
                break;
            }
            case MarketMakerParametersColumns::EstLevelPctEMA: {
                _estLevelPctEma = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. Established level PctEMA changed. Value={d1}", getContractName(), _estLevelPctEma)
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
            case MarketMakerParametersColumns::WorsenPayupCondPercent: {
                double worsenPayupConditionValue = boost::lexical_cast<double>(gridCell.value());
                if(worsenPayupConditionValue < 0) {
                    LOG(0, "MM {C[i1]}. WorsenPayupCond% < -, refusing to change.  Value={d1} Amount={i2}", getContractName(), (_worsenPayupConditionAmount / static_cast<double>(_tickSize)), _worsenPayupConditionAmount);
                    _marketMaker->sendErrorMessage("MM WorsenPayupCond% must be >= 0", ErrorType::Critical);
                    gridCell.set_value(std::to_string(_worsenPayupConditionAmount / static_cast<double>(_tickSize)));
                }
                else {
                    _worsenPayupConditionAmount = worsenPayupConditionValue * _tickSize;
                    LOG(0, "MM {C[i1]}. WorsenPayupCond% changed. Value={d1} Amount={i2}", getContractName(), worsenPayupConditionValue, _worsenPayupConditionAmount);
                    applyUpdates();
                }
                break;
            }
            case MarketMakerParametersColumns::QueueImportance: {
                auto value = boost::lexical_cast<uint32_t>(gridCell.value());
                if (value > 10) {
                    value = 0;
                }
                _queueImportance = value;
                LOG(0, "MM {C[i1]}. QueueImportance changed. Value={d1}", getContractName(), value);
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

    void recalculateSizesAndApplyUpdate() {
        if (!isEnabled()) {
            return;
        }

        bool hasChange = false;

        auto& bidBook = _books[BID];
        auto& askBook = _books[ASK];

        if (BOOST_LIKELY(hasMarketData(bidBook, askBook))) {
            if (isEnabled() && _maxTicksWideBreached) {
                const bool quoteExternally = canQuoteExternally();
                hasChange |= updateQuote<BID>(quoteExternally);
                hasChange |= updateQuote<ASK>(quoteExternally);
            }
            hasChange |= modifyHedgeQuote(false);
        } else {
            hasChange |= modifyHedgeQuote(true);
        }

        if (hasChange) {
            notifyOrderRequest();
        }
    }

    bool validateParametersUpdate(const proto::GridCell& gridCell) {
        return true;
    }

    void addManualPosition(hf::qty_t size, uint32_t side, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        LOG(0, "MM {C[i1]}. Adding Manual Position. Side={i2} Size={i3}", getContractName(), side, size)
        hf::qty_t remaining = allocateHedgingFill(side, size, allocatedInternally, allocatedExternally);
        if (remaining > 0) {
            if (side == BID) {
                allocateHedgingFillRemainder<BID>(true, remaining, allocatedInternally, allocatedExternally);
            } else {
                allocateHedgingFillRemainder<ASK>(true, remaining, allocatedInternally, allocatedExternally);
            }
            LOG(0, "MM {C[i1]}. Allocated remainder. InternalPosition={i2} ExternalPosition={i3} Remaining={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, remaining)
        }
    }

    void onOrderUpdate(const OrderUpdate& msg) {
        auto removeOrderFromPriceLevel = [this](uint32_t side, const OrderInfo& order) {
            auto it = _priceOrders[side].find(order.price);
            if (it != _priceOrders[side].end()) {
                auto& orders = it->second;
                for (auto orderIt = orders.begin(); orderIt != orders.end(); ++orderIt) {
                    if ((*orderIt)->orderId ==  order.orderId) {
                        orders.erase(orderIt);
                        break;
                    }
                }
                if (orders.empty()) {
                    _priceOrders[side].erase(it);
                }
            }
        };
        auto iter = _orders.find(msg.orderId);
        if (iter == _orders.end()) {
            iter = _orders.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(msg.orderId),
                                   std::forward_as_tuple(msg.price, msg.qty, msg.orderId, msg.side)).first;
            _priceOrders[msg.side][msg.price].emplace_back(&(iter->second));
        } else {
            auto& order = iter->second;
            if (order.price != msg.price) {
                // price changed, remove order from previous price level
                removeOrderFromPriceLevel(msg.side, order);
                if (msg.qty) {
                    order.price = msg.price;
                    order.qty = msg.qty;
                    order.qp = -1;  // qp update should follow
                    // add order to new price level
                    _priceOrders[msg.side][order.price].emplace_back(&order);
                } else {
                    // Order filled
                    _orders.erase(iter);
                }
            } else if (msg.qty == 0) {
                // Order Canceled or filled
                removeOrderFromPriceLevel(msg.side, order);
                _orders.erase(iter);
            } else {
                // only qty changed.
                order.qty = msg.qty;
            }
        }
    }

    void onQueuePositionUpdate(const QueuePositionUpdate& msg) {
        auto orderIter = _orders.find(msg.orderId);
        if (orderIter == _orders.end()) {
            return;
        }

        auto& order = orderIter->second;
        order.qp = msg.qp;
        if (order.qp < 0) {
            order.qp = 0;
        }

        if (_books[order.side]._price == order.price) {
            if (order.side == BID) {
                updateQuote<BID>(canQuoteExternally());
            } else {
                updateQuote<ASK>(canQuoteExternally());
            }
        }
    }

private:

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
    double _pctTobMaxQuote;
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
    double _emaEstLevel;
    double _lastTobQty = 0;
    hf::qty_t _establishedLevel;
    uint64_t _estLevelEmaPeriod;
    uint64_t _lastEmaTime = 0;
    double _estLevelPctEma;
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
    hf::price_t _width = 0;
    uint32_t _selfMatchCount = 0;
    hf::price_t _worsenPayupConditionAmount = 0;
    uint_fast64_t _lastMdSeqNum = 0;
    uint32_t _queueImportance = 0;
    Book _books[2];

    struct OrderInfo {
        hf::price_t price;
        hf::qty_t qty;
        hf::qty_t qp = -1;
        uint32_t orderId;
        uint32_t side;

        OrderInfo(hf::price_t p, hf::qty_t q, uint32_t id, uint32_t s)
            : price(p), qty(q), orderId(id), side(s)
        {}
    };
    std::unordered_map<hf::qty_t, std::vector<OrderInfo*>> _priceOrders[2];
    std::unordered_map<uint32_t, OrderInfo> _orders;
};

}

namespace {

template<>
inline hf::qty_t getInternalQuoteSize<Python::BID>(Python::SimpleMarketMaker& mm, hf::qty_t maxQuoteSize) {
    if (mm._internallyInitiatedPosition < 0) {
        return std::min(mm._maxInternallyInitiatedPosition, maxQuoteSize);
    }
    if (mm._maxInternallyInitiatedPosition <= mm._internallyInitiatedPosition) {
        return 0;
    }
    return std::min(mm._maxInternallyInitiatedPosition - mm._internallyInitiatedPosition, maxQuoteSize);
}

template<>
inline hf::qty_t getInternalQuoteSize<Python::ASK>(Python::SimpleMarketMaker& mm, hf::qty_t maxQuoteSize) {
    if (mm._internallyInitiatedPosition > 0) {
        return std::min(mm._maxInternallyInitiatedPosition, maxQuoteSize);
    }
    if (mm._maxInternallyInitiatedPosition <= -mm._internallyInitiatedPosition) {
        return 0;
    }
    return std::min(mm._maxInternallyInitiatedPosition + mm._internallyInitiatedPosition, maxQuoteSize);
}

template<>
inline hf::qty_t getMaxQuoteSizeBasedOnMarket<Python::BID>(Python::SimpleMarketMaker& mm, hf::price_t price) {
    BOOST_ASSERT_MSG(price != hf::NULL_PRICE, "Really shouldn't ever see a null quote price here.  I think?");
    return price >= mm._books[Python::BID]._price
        ? static_cast<hf::qty_t>(mm._books[Python::BID]._otherQuantity * mm._pctTobMaxQuote)
        : std::numeric_limits<hf::qty_t>::max();
}

template<>
inline hf::qty_t getMaxQuoteSizeBasedOnMarket<Python::ASK>(Python::SimpleMarketMaker& mm, hf::price_t price) {
    BOOST_ASSERT_MSG(price != hf::NULL_PRICE, "Really shouldn't ever see a null quote price here.  I think?");
    return price <= mm._books[Python::ASK]._price
        ? static_cast<hf::qty_t>(mm._books[Python::ASK]._otherQuantity * mm._pctTobMaxQuote)
        : std::numeric_limits<hf::qty_t>::max();
}

template<>
inline hf::qty_t getExternalQuoteSize<Python::BID>(Python::SimpleMarketMaker& mm, hf::qty_t maxQuoteSize) {
    if (mm._externallyInitiatedPosition < 0) {
        return std::min(mm._quoteSize, maxQuoteSize);
    }
    if (mm._maxExternallyInitiatedPosition <= mm._externallyInitiatedPosition) {
        return 0;
    }
    return std::min(mm._quoteSize, std::min(mm._maxExternallyInitiatedPosition - mm._externallyInitiatedPosition, maxQuoteSize));
}

template<>
inline hf::qty_t getExternalQuoteSize<Python::ASK>(Python::SimpleMarketMaker& mm, hf::qty_t maxQuoteSize) {
    if (mm._externallyInitiatedPosition > 0) {
        return std::min(mm._quoteSize, maxQuoteSize);
    }
    if (mm._maxExternallyInitiatedPosition <= -mm._externallyInitiatedPosition) {
        return 0;
    }
    return std::min(mm._quoteSize, std::min(mm._maxExternallyInitiatedPosition + mm._externallyInitiatedPosition, maxQuoteSize));
}

template<>
inline hf::price_t roundToTickSize<Python::BID>(Python::SimpleMarketMaker& mm, hf::price_t price) {
    return Python::RoundToTickSize(price - mm._commission, mm._tickSize, Python::BID);
}

template<>
inline hf::price_t roundToTickSize<Python::ASK>(Python::SimpleMarketMaker& mm, hf::price_t price) {
    return Python::RoundToTickSize(price + mm._commission, mm._tickSize, Python::ASK);
}

template<>
inline bool isThereEnoughSizeToJoin<Python::BID>(Python::SimpleMarketMaker& mm, hf::price_t quotePrice) {
    auto& bidBook = mm._books[Python::BID];
    return ((quotePrice >= bidBook._price) & (bidBook._otherQuantity >= mm._joinSize)) | (quotePrice < bidBook._price);
}

template<>
inline bool isThereEnoughSizeToJoin<Python::ASK>(Python::SimpleMarketMaker& mm, hf::price_t quotePrice) {
    auto& askBook = mm._books[Python::ASK];
    return ((quotePrice <= askBook._price) & (askBook._otherQuantity >= mm._joinSize)) | (quotePrice > askBook._price);
}

template<>
inline bool isThereEnoughSizeToStay<Python::BID>(Python::SimpleMarketMaker& mm, hf::price_t quotePrice) {
    auto& bidBook = mm._books[Python::BID];
    return ((quotePrice >= bidBook._price) & (bidBook._otherQuantity >= mm._leaveSize)) | (quotePrice < bidBook._price);
}

template<>
inline bool isThereEnoughSizeToStay<Python::ASK>(Python::SimpleMarketMaker& mm, hf::price_t quotePrice) {
    auto& askBook = mm._books[Python::ASK];
    return ((quotePrice <= askBook._price) & (askBook._otherQuantity >= mm._leaveSize)) | (quotePrice > askBook._price);
}

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
