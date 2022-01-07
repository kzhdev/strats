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
inline hf::price_t getHedgeRefPrice(hf::price_t ourInternalPrice, hf::price_t bookPrice);

template<>
inline hf::price_t getHedgeRefPrice<Python::BID>(hf::price_t ourInternalPrice, hf::price_t bookPrice) {
    return (ourInternalPrice != hf::NULL_PRICE) ? std::min<hf::price_t>(ourInternalPrice, bookPrice) : bookPrice;
}

template<>
inline hf::price_t getHedgeRefPrice<Python::ASK>(hf::price_t ourInternalPrice, hf::price_t bookPrice) {
    return (ourInternalPrice != hf::NULL_PRICE) ? std::max<hf::price_t>(ourInternalPrice, bookPrice) : bookPrice;
}

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
inline hf::qty_t getMaxQuoteSizeBasedOnMarket(const md::top_t::level_t& book, double pctTobMaxQuote, hf::price_t price);

template<>
inline hf::qty_t getMaxQuoteSizeBasedOnMarket<Python::BID>(const md::top_t::level_t& book, double pctTobMaxQuote, hf::price_t price) {
    BOOST_ASSERT_MSG(price != hf::NULL_PRICE, "Really shouldn't ever see a null quote price here.  I think?");
    return price >= book.pr
           ? static_cast<hf::qty_t>(book.q * pctTobMaxQuote)
           : std::numeric_limits<hf::qty_t>::max();
}

template<>
inline hf::qty_t getMaxQuoteSizeBasedOnMarket<Python::ASK>(const md::top_t::level_t& book, double pctTobMaxQuote, hf::price_t price) {
    BOOST_ASSERT_MSG(price != hf::NULL_PRICE, "Really shouldn't ever see a null quote price here.  I think?");
    return price <= book.pr
           ? static_cast<hf::qty_t>(book.q * pctTobMaxQuote)
           : std::numeric_limits<hf::qty_t>::max();
}

template<uint32_t Side>
inline hf::qty_t getExternalQuoteSize(const Python::SimpleMarketMaker& mm, hf::qty_t quoteSize, hf::qty_t maxQuoteSize);

template<uint32_t Side>
inline hf::price_t roundToTickSize(const Python::SimpleMarketMaker& mm, hf::price_t price);

template<uint32_t Side>
inline bool isThereEnoughSize(const md::top_t::level_t& book, hf::qty_t sizeThreshold, hf::price_t quotePrice);

template<>
inline bool isThereEnoughSize<Python::BID>(const md::top_t::level_t& book, hf::qty_t sizeThreshold, hf::price_t quotePrice) {
    return ((quotePrice >= book.pr) & (book.q >= sizeThreshold)) | (quotePrice < book.pr);
}

template<>
inline bool isThereEnoughSize<Python::ASK>(const md::top_t::level_t& book, hf::qty_t sizeThreshold, hf::price_t quotePrice) {
    return ((quotePrice <= book.pr) & (book.q >= sizeThreshold)) | (quotePrice > book.pr);
}

}   // end of anonymous namespace

namespace Python {

class SimpleMarketMaker {
    friend class MarketMaker;
    OrderRequestManager& _orderRequestManager = OrderRequestManager::get();
    MarketMaker* _marketMaker;
    ReadyOrderRequests _lastOrderRequests;
    bool _maxTicksWideStopExtQuotingBreached = false;
    bool _maxTicksWideDisableBreached = false;

    static constexpr uint8_t QUOTE_ORDER_ID = orderId(StrategyId::MarketMaker, Order::Type::Quoting);
    static constexpr uint8_t HEDGE_ORDER_ID = orderId(StrategyId::MarketMaker, Order::Type::Hedging);

public:

    struct Quote {
        OrderId_t _id = QUOTE_ORDER_ID;
        hf::price_t _internalPrice = hf::NULL_PRICE;
        hf::qty_t _internalSize = 0;
        hf::price_t _externalPrice = hf::NULL_PRICE;
        hf::qty_t _externalSize = 0;
        hf::price_t _lastInternalPriceSent = hf::NULL_PRICE;
        hf::price_t _lastExternalSizeSent = 0;

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
        hf::price_t _lastInternalPriceSent = hf::NULL_PRICE;
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
        _quoteSize = contract.toValidQty(config._quoteSize);
        if (!_quoteSize) {
            logging::GetLogger().logErrorAndThrow(boost::str(boost::format("MM %1% quoteSize is 0") % contract._name));
        }
        _quoteGap = config._quoteGap;
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
        _maxTicksWideStopExtQuoting = config._maxTicksWideStopExtQuoting < 0 ? std::numeric_limits<int64_t>::max() : config._maxTicksWideStopExtQuoting * _tickSize;
        _maxTicksWideDisable = config._maxTicksWideDisable < 0 ? std::numeric_limits<int64_t>::max() : config._maxTicksWideDisable * _tickSize;
        _internallyInitiatedPosition = config._internalPosition;
        _externallyInitiatedPosition = config._externalPosition;
        _worsenPayupConditionAmount = config._worsenPayupCondPercent * _tickSize;
        _queueImportance = config._queueImportance;

        auto netPos = _internallyInitiatedPosition + _externallyInitiatedPosition;
        if (netPos) {
            LOG(0, "MM {C[i1]}. Loaded Positions. InternalPosition={i2} ExternalPosition={i3}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
            _marketMaker->sendFillToFront(contract._internalId, (netPos > 0 ? BID : ASK),
                                          _marketMaker->_viperConfig._mmLastFillPrices[contract._marketMakeIndex],
                                          std::abs(netPos), FillType::Phantom, FillSource::ViperStart,
                                          _internallyInitiatedPosition, _externallyInitiatedPosition);
        }
    }

    void takeOverFill(const FillMessage& fill) {
        LOG(0, "MM {C[i1]}. Take overfill. Size={i2}", getContractName(), fill._qty)
        LOG(0, "MM {C[i1]}. Before take overfill. InternalInitPosition={i2} ExtInitPosition={i3} MaxExtInitPos={i4}", getContractName(),
            _internallyInitiatedPosition, _externallyInitiatedPosition, _maxExternallyInitiatedPosition)

        hf::qty_t allocatedInternally = 0;
        hf::qty_t allocatedExternally = 0;
        hf::qty_t remainingSize = offsetPosition(fill._side, fill._qty, allocatedInternally, allocatedExternally);
        auto sizeTaken = allocatedInternally + allocatedExternally;
        if (remainingSize > 0) {
            const hf::qty_t multiplier = fill._side == BID ? 1 : -1;
            hf::qty_t absExternalPosition = std::abs(_externallyInitiatedPosition);
            if (_maxExternallyInitiatedPosition <= absExternalPosition) {
                LOG(0, "MM {C[i1]}. External position reached or exceeded max, could not take anymore.", getContractName())
            } else {
                const hf::qty_t newPosition = remainingSize * multiplier + _externallyInitiatedPosition;
                if (std::abs(newPosition) > _maxExternallyInitiatedPosition) {
                    hf::qty_t taken = _maxExternallyInitiatedPosition - absExternalPosition;
                    _externallyInitiatedPosition += taken * multiplier;
                    allocatedExternally += taken;
                    sizeTaken += taken;
                    remainingSize -= taken;
                    LOG(0, "MM {C[i1]}. Could not take the whole overfill. Took={i2}", getContractName(), sizeTaken)
                }
                else {
                    _externallyInitiatedPosition = newPosition;
                    allocatedExternally += remainingSize;
                    sizeTaken += remainingSize;
                    remainingSize = 0;
                    LOG(0, "MM {C[i1]}. Took the whole overfill. Took={i2}", getContractName(), sizeTaken)
                }
            }
        }

        if (sizeTaken) {
            _marketMaker->sendFillToFront(fill._contractId, fill._side, fill._price, sizeTaken, fill._type, fill._source, allocatedInternally, allocatedExternally);
        }
        LOG(0, "MM {C[i1]}. After take overfill. InternalInitPosition={i2} ExtInitPosition={i3} MaxExtInitPos={i4}", getContractName(),
            _internallyInitiatedPosition, _externallyInitiatedPosition, _maxExternallyInitiatedPosition)

        if (remainingSize > 0) {
            bool overFillSendToOtherStrategy = false;
            for (uint32_t i = toUInt(StrategyId::RelativeValue); i < toUInt(StrategyId::MAX); ++i) {
                if (_marketMaker->_strategies[i]->strategyId() != StrategyId::ArbieSpreader &&
                    _marketMaker->_strategies[i]->isEnabled()) {
                    LOG(0, "MM {C[i1]}. Send untaken overfill to Strategy {i2} Side={i3} Price={d1} Size={i4}",
                        getContractName(), strategyName((StrategyId)i), fill._side, ToDollars(fill._price), remainingSize)
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

    void forceTakeOverFill(uint32_t side, hf::price_t price, hf::qty_t overFillSize, FillSource source, bool checkLimit = false) {
        LOG(0, "MM {C[i1]}. Forced to take overfill. Size={i2} Source={i3}", getContractName(), overFillSize, toUInt(source))
        LOG(0, "MM {C[i1]}. Before forced to take fill. InternalInitPosition={i2} ExtInitPosition={i3} MaxExtInitPos={i4}", getContractName(),
            _internallyInitiatedPosition, _externallyInitiatedPosition, _maxExternallyInitiatedPosition)

        hf::qty_t allocatedInternally = 0;
        hf::qty_t allocatedExternally = 0;
        hf::qty_t remainingSize = offsetPosition(side, overFillSize, allocatedInternally, allocatedExternally);
        if (remainingSize > 0) {
            const hf::qty_t multiplier = side == BID ? 1 : -1;
            _externallyInitiatedPosition += remainingSize * multiplier;
            allocatedExternally += remainingSize;
        }

        LOG(0, "MM {C[i1]}. After forced to take fill. InternalInitPosition={i2} ExtInitPosition={i3} MaxExtInitPos={i4}", getContractName(),
            _internallyInitiatedPosition, _externallyInitiatedPosition, _maxExternallyInitiatedPosition)

        _marketMaker->sendFillToFront(_contract._internalId, side, price, overFillSize, FillType::External, source, allocatedInternally, allocatedExternally);
        if (checkLimit && !isWithinPositionLimits()) {
            disableExternalQuoting();
        }
    }

    template<uint32_t Side>
    bool isHedging() const noexcept {
        return _hedges[Side]._sizeToQuote != 0;
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

    bool isInPayUp(hf::qty_t ourSize, const md::top_t::level_t& book) const noexcept {
        const hf::qty_t payUpBasedOnSize = static_cast<hf::qty_t>(ourSize * _payUpMultiple);
        const hf::qty_t payupQuantity = std::max(_payUpQuantity, payUpBasedOnSize);
        const bool shouldPayup = (book.q <= payupQuantity) | (book.o <= _payUpOrders);
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

    bool hasMarketData(const md::top_t::level_t& bidBook, const md::top_t::level_t& askBook) {
        const bool missingMarketData = ((bidBook.pr == hf::NULL_PRICE) | (askBook.pr == hf::NULL_PRICE));
        if (BOOST_UNLIKELY(missingMarketData)) {
            if (_contract._toMarketMake & _marketMaker->canExchangeQuoteExternally(_contract._venue)) {
                if (_enabled) {
                    _marketMaker->sendErrorMessage(boost::str(boost::format("MM - Not having market data so turning market maker off. Contract=%1%") % _contract._name), ErrorType::Critical);
                    LOG(0, "MM {C[i1]}. Lost market data. Disabling, turn off external quoting and useGTC. ", getContractName())
                    disable(true, false, true);
                    setQuoteExternalFlag(false);
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

    template<uint32_t Side>
    const md::top_t::level_t& topBook() noexcept {
        return _books[Side].l[0];
    }

    template<uint32_t Side>
    int32_t getQuoteLevelIndex() const noexcept {
        auto& quote = _quotes[Side];
        return getPriceLevelIndex<Side>(quote._externalPrice);
    }

    template<uint32_t Side>
    int32_t getPriceLevelIndex(hf::price_t price) const noexcept {
        if (price == hf::NULL_PRICE ||
            IsBetterPrice<Side>(price, _books[Side].l[0].pr)) {
            return -1;
        }
        for (size_t i = 0; i < md::top_t::LEVEL_NUM; ++i) {
            if (_books[Side].l[i].pr == hf::NULL_PRICE) {
                return -1;
            }
            if (price == _books[Side].l[i].pr) {
                return i;
            }
        }
        return -1;
    }

    void onMarketDataChange() {
        const auto& bidBook = topBook<BID>();
        const auto& askBook = topBook<ASK>();

        LOG(0, "MM {C[i1]}. Recalculate. Bid: {i2}({i3})@{d1} Ask={i4}({i5})@{d2} lastMDbid={i6} priceChangeIndex={i7}/{i8} qtyChangeIndex={i9}/{i10}", getContractName(),
            bidBook.q, bidBook.o, ToDollars(bidBook.pr), askBook.q, askBook.o, ToDollars(askBook.pr), _lastMDbid,
            toUInt(_mdPriceChangeIndex[BID]), toUInt(_mdPriceChangeIndex[ASK]), toUInt(_mdQtyChangeIndex[BID]), toUInt(_mdQtyChangeIndex[ASK]))

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }

        bool topBookChanged = _mdQtyChangeIndex[BID] == 0 || _mdQtyChangeIndex[ASK] == 0;
        if (topBookChanged) {
            if ((bidBook.q > 0) && (askBook.q > 0)) {
                calculateEstablishedLevel((double)(bidBook.q + askBook.q) / 2);
            }
            _midPoint = calculateMidPoint(bidBook, askBook);
            LOG(0, "MM {C[i1]}. Midpoint. Midpoint={d1}", getContractName(), ToDollars(_midPoint))

            const double marketSpread = askBook.pr - bidBook.pr;
            _width = static_cast<hf::price_t>(marketSpread / 2.0 * _widthPercentage);
        }

        if (BOOST_UNLIKELY(!isEnabled())) {
            return;
        }

        if (BOOST_UNLIKELY((askBook.pr - bidBook.pr) > _maxTicksWideDisable)) {
            _maxTicksWideDisableBreached = true;
            LOG(0, "MM {C[i1]}. MaxTicksWidthDisable breached, disable contract; disable external quoting; disable GTC. ticksWide={i2} _maxTicksWideDisable={i3}", getContractName(), (askBook.pr - bidBook.pr), _maxTicksWideDisable)
            disable(true, false, true);
            disableExternalQuoting(true);
            _marketMaker->sendErrorMessage(boost::str(boost::format(
                    "MM - MaxTicksWidthDisable breached, disable contract, disable external quoting, disable GTC. Contract=%1%")
                    % _contract._name), ErrorType::Critical);
            return;
        }

        bool maxTickWideAlreadyBreached = _maxTicksWideStopExtQuotingBreached;
        if (BOOST_UNLIKELY((askBook.pr - bidBook.pr) > _maxTicksWideStopExtQuoting)) {
            if (!_maxTicksWideStopExtQuotingBreached) {
                _marketMaker->sendGenericMessageToExecutor(ViperMessageType::ChangeUseGTC, _contract._internalId, false);
                std::string msg = boost::str(boost::format("MM - MaxTicksWidth breached, disable external quoting, disable GTC. Contract=%1%") % _contract._name);
                _quoteExternalBeforeMaxTicksWideStopQuotingBreached = disableExternalQuotingAndUseGTC(msg, true, false);
                _maxTicksWideStopExtQuotingBreached = true;
            }
        } else if (_maxTicksWideStopExtQuotingBreached) {
            _maxTicksWideStopExtQuotingBreached = false;
            if (_quoteExternalBeforeMaxTicksWideStopQuotingBreached) {
                LOG(0, "MM {C[i1]}. MaxTicksWidthStopExtQuote Unbreached. Re-enable external quoting", getContractName())
                _marketMaker->sendErrorMessage(boost::str(boost::format(
                        "MM - MaxTicksWidthStopExtQuote Unbreached, re-enable external quoting. Contract=%1%")
                                                          % _contract._name), ErrorType::Normal);
                enableExternalQuoting(false);
            }
        }

        bool hasChange = false;
        if (BOOST_UNLIKELY(_maxTicksWideStopExtQuotingBreached & !maxTickWideAlreadyBreached)) {
            _quotes[BID].reset();
            _quotes[ASK].reset();
            modifyQuoteOrder<BID, 0>(_quotes[BID], false);
            modifyQuoteOrder<ASK, 0>(_quotes[ASK], false);
            hasChange = true;
        }

        if (_mdQtyChangeIndex[BID] != -1 || _mdQtyChangeIndex[ASK] != -1) {
            bool quoteExternally = canQuoteExternally();
            bool bidChanged = updateQuotesBySide<BID>(bidBook, askBook, topBookChanged, quoteExternally, false);
            bool askChanged = updateQuotesBySide<ASK>(askBook, bidBook, topBookChanged, quoteExternally, false);
            checkQuoteCross<5>(quoteExternally, bidChanged, askChanged);
            hasChange |= bidChanged | askChanged;

            if (topBookChanged || askChanged) {
                if (isHedging<BID>()) {
                    bool hedgeChanged = updateHedgePrice<BID>(bidBook, askBook);
                    hasChange |= hedgeChanged;
                    hasChange |= checkHedgeCross<BID, 6>(quoteExternally, hedgeChanged, askChanged);
                }
            }
            if (topBookChanged || bidChanged) {
                if (isHedging<ASK>()) {
                    bool hedgeChanged = updateHedgePrice<ASK>(askBook, bidBook);
                    hasChange |= hedgeChanged;
                    hasChange |= checkHedgeCross<ASK, 6>(quoteExternally, hedgeChanged, bidChanged);
                }
            }
        }

        if (hasChange) {
            notifyOrderRequest();
        }
    }

    template<uint32_t Source>
    void checkQuoteCross(bool quoteExternally, bool& bidChanged, bool& askChanged) {
        if (!(bidChanged & askChanged) && (bidChanged | askChanged)) {  // one side did not change
            if (!bidChanged) {
                DLOG(0,"MM {C[i1]}. check cross orgBID={d1} ASK={d2}", getContractName(), ToDollars(_quotes[BID]._lastInternalPriceSent),
                     ToDollars(_quotes[ASK]._internalPrice))
                if (_quotes[BID]._lastInternalPriceSent != hf::NULL_PRICE &&
                    _quotes[ASK]._internalPrice != hf::NULL_PRICE &&
                    _quotes[BID]._lastInternalPriceSent >= _quotes[ASK]._internalPrice) {
                    // force update BID quote, otherwise new ASK will cross old BID
                    modifyQuoteOrder<BID, Source>(_quotes[BID], quoteExternally);
                    bidChanged = true;
                }
            } else {
                DLOG(0,"MM {C[i1]}. check cross BID={d1} orgASK={d2}", getContractName(),
                     ToDollars(_quotes[BID]._internalPrice), ToDollars(_quotes[ASK]._lastInternalPriceSent))
                if (_quotes[ASK]._lastInternalPriceSent != hf::NULL_PRICE &&
                    _quotes[BID]._internalPrice != hf::NULL_PRICE &&
                    _quotes[ASK]._lastInternalPriceSent <= _quotes[BID]._internalPrice) {
                    // force update ASK quote, otherwise new BID will cross old ASK
                    modifyQuoteOrder<ASK, Source>(_quotes[ASK], quoteExternally);
                    askChanged = true;
                }
            }
        }
    }

    template<uint32_t Side, uint32_t Source>
    bool checkHedgeCross(bool quoteExternally, bool hedgeChanged, bool otherSideQuoteChanged) {
        auto& hedge = _hedges[Side];
        if (hedge._inPayUp) {
            return false;
        }
        auto& quote = _quotes[1 - Side];
        if (hedgeChanged && !otherSideQuoteChanged) {
            DLOG(0,"MM {C[i1]}. check hedge cross old quote. Hedge: side={i2} price={d1} Quote: priceSent={d2}", getContractName(),
                 Side, ToDollars(hedge._internalPrice), ToDollars(quote._lastInternalPriceSent))
            if (hedge._internalPrice != hf::NULL_PRICE && quote._lastInternalPriceSent != hf::NULL_PRICE &&
                IsPriceBetterOrEqual<Side>(hedge._internalPrice, quote._lastInternalPriceSent)) {
                // force update quote in the other side, otherwise the hedge will cross the quote in other side
                if (Side == BID) {
                    modifyQuoteOrder<ASK, Source>(quote, quoteExternally);
                } else {
                    modifyQuoteOrder<BID, Source>(quote, quoteExternally);
                }
                return true;
            }
        } else if (!hedgeChanged && otherSideQuoteChanged) {
            DLOG(0,"MM {C[i1]}. check quote cross old hedge. Quote: side={i2} price={d1} Hedge: priceSent={d2}", getContractName(),
                 (1 - Side), ToDollars(quote._lastInternalPriceSent), ToDollars(hedge._internalPrice))
            if (hedge._internalPrice != hf::NULL_PRICE && quote._lastInternalPriceSent != hf::NULL_PRICE &&
                IsPriceBetterOrEqual<Side>(hedge._internalPrice, quote._lastInternalPriceSent)) {
                // force update hedge, otherwise the hedge will cross the quote in other side
                modifyHedgeOrder<Side>(hedge, false);
                return true;
            }
        }
        return false;
    }

    void resetChangeIndices() noexcept {
        _mdQtyChangeIndex[BID] = -1;
        _mdPriceChangeIndex[BID] = -1;
        _mdQtyChangeIndex[ASK] = -1;
        _mdPriceChangeIndex[ASK] = -1;
    }

    bool forceUpdateQuotesUpdateHedgePrice() {
        const auto& bidBook = topBook<BID>();
        const auto& askBook = topBook<ASK>();

        bool quoteExternally = canQuoteExternally();
        bool hasChange = false;
        bool bidChanged = updateQuotesBySide<BID>(bidBook, askBook, false, quoteExternally, true);
        bool askChanged = updateQuotesBySide<ASK>(askBook, bidBook, false, quoteExternally, true);
        checkQuoteCross<7>(quoteExternally, bidChanged, askChanged);
        hasChange |= bidChanged | askChanged;
        if (isHedging<BID>()) {
            bool hedgeChanged = updateHedgePrice<BID>(bidBook, askBook);
            hasChange |= hedgeChanged;
            hasChange |= checkHedgeCross<BID, 8>(quoteExternally, hedgeChanged, askChanged);
        }
        if (isHedging<ASK>()) {
            bool hedgeChanged = updateHedgePrice<ASK>(askBook, bidBook);
            hasChange |= hedgeChanged;
            hasChange |= checkHedgeCross<ASK, 8>(quoteExternally, hedgeChanged, bidChanged);
        }
        return hasChange;
    }

    template<uint32_t Side>
    bool updateQuotesBySide(const md::top_t::level_t& book, const md::top_t::level_t& otherSideBook, bool tobChanged,
                            bool quoteExternally, bool forceUpdate) {
        bool hasChange = false;
        int32_t quoteLevelIndex = -1;
        if (BOOST_LIKELY(!_maxTicksWideStopExtQuotingBreached)) {
            if (tobChanged || forceUpdate) {
                // Top book changed.
                // NOTE: either side of tob changed we need to recalculate because the weighted mid point might changed
                hasChange |= updateQuote<Side, 1>(book, quoteExternally, forceUpdate);
            } else if (quoteExternally) {
                // Change not on the top of book
                quoteLevelIndex = getQuoteLevelIndex<Side>();
                if (_mdPriceChangeIndex[Side] != -1) {
                    if (_mdQtyChangeIndex[Side] <= quoteLevelIndex) {
                        // price & qty has changed on a level better than
                        // quote level. Quote level might have changed as well
                        hasChange |= updateQuote<Side, 3>(book, quoteExternally);
                    }
                } else if (_mdQtyChangeIndex[Side] == quoteLevelIndex) {
                    // only market qty changed on the quote level
                    hasChange |= updateQuote<Side, 4>(book, quoteExternally);
                }
            }
        }
        return hasChange;
    }

    template<uint32_t Side>
    bool updateHedgePrice(const md::top_t::level_t& book, const md::top_t::level_t& otherSideBook) {
        bool hasChange = false;
        LOG(0, "MM {C[i1]}. {i2} updating hedge price", getContractName(), Side)
        Hedge& hedge = _hedges[Side];
        bool prePayUp = hedge._inPayUp;
        hf::price_t internalPrice;
        hf::price_t externalPrice;
        hedge._inPayUp = calculateHedgePrices<Side>(hedge._sizeToQuote, internalPrice, externalPrice, book, otherSideBook);
        if (hedge._inPayUp) {
            hedge._internalPrice = internalPrice;
            hedge._externalPrice = externalPrice;
            modifyHedgeOrder<Side>(hedge, true);
            LOG(0, "MM {C[i1]}. {i2} Paying up. IntPrice={d1} ExtPrice={d2} Size={i3} IntPosition={i4} ExtPosition={i5}", getContractName(),
                Side, ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote,
                _internallyInitiatedPosition, _externallyInitiatedPosition)
            hasChange = true;
        } else {
            const bool priceChanged = std::abs(hedge._lastInternalPriceSent - internalPrice) > _minInternalPriceChange;
            if (priceChanged || prePayUp != hedge._inPayUp) {
                hedge._internalPrice = internalPrice;
                hedge._externalPrice = externalPrice;
                LOG(0, "MM {C[i1]}. {i2} Repriced hedge. IntPrice={d1} ExtPrice={d2} Size={i3}", getContractName(),
                    Side, ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), hedge._sizeToQuote)
                modifyHedgeOrder<Side>(hedge, false);
                hasChange = true;
            }
        }
        return hasChange;
    }

    template<uint32_t Side, uint32_t source>
    bool updateQuote(const md::top_t::level_t& book, bool quoteExternally, bool forceUpdate = false) {
        hf::price_t price = calculatePriceFromMidPointAndWidth<Side>(_midPoint, _width);
        auto [internalPrice, externalPrice, quoteSize] = calculateQuotePriceAndSizeByOrders<Side>(book, price, quoteExternally);
        auto& quote = _quotes[Side];
        hf::qty_t maxQuoteSizeBasedOnMarket = getMaxQuoteSizeBasedOnMarket<Side>(book, _pctTobMaxQuote, externalPrice);
        const bool isBookInPayUpMode = isInPayUp(getExternalQuoteSize<Side>(*this, quoteSize, maxQuoteSizeBasedOnMarket), book);
        bool changeInPrice = false;
        bool hasChange = false;

        LOG(0, "MM {C[i1]}. {i2} Quote based on orders: price={d1} extPrice={d2} qty={i3} inPayUp={i4}", getContractName(),
            Side, ToDollars(internalPrice), ToDollars(externalPrice), quoteSize, isBookInPayUpMode)

        if (!isBookInPayUpMode) {
            const hf::price_t internalPriceChange = internalPrice - _quotes[Side]._lastInternalPriceSent;
            changeInPrice = std::abs(internalPriceChange) > _minInternalPriceChange;
            quote._internalPrice = internalPrice;
            if (quoteExternally) {
                quote._externalPrice = externalPrice;
                assert(IsPriceBetterOrEqual<Side>(book.pr, quote._externalPrice));
            }
        }
        else {
            const hf::price_t worsenPrice = calculateWorsenPrice<Side>(book.pr, _worsenPayupConditionAmount);
            const hf::price_t ordinaryPrice = calculatePriceFromMidPointAndWidth<Side>(_midPoint, _width);
            internalPrice = calculatePayUpInternalPrice<Side>(ordinaryPrice, worsenPrice);
            auto extPrice = roundToTickSize<Side>(*this, internalPrice);
            if (externalPrice != extPrice) {
                // price worsened, evaluate orders on the new level
                LOG(0, "MM {C[i1]}. Re-evaluate orders on worsened price level", getContractName())
                std::tie(internalPrice, externalPrice, quoteSize) = calculateQuotePriceAndSizeByOrders<Side>(book, internalPrice, quoteExternally);
                LOG(0, "MM {C[i1]}. {i2} Quote based on orders: price={d1} extPrice={d2} qty={i3}", getContractName(), Side, ToDollars(internalPrice), ToDollars(externalPrice), quoteSize)
            }
            changeInPrice = internalPrice != quote._internalPrice;
            quote._internalPrice = internalPrice;
            if (quoteExternally) {
                quote._externalPrice = extPrice;
            }
        }

        bool changeInSize = false;
        maxQuoteSizeBasedOnMarket = getMaxQuoteSizeBasedOnMarket<Side>(book, _pctTobMaxQuote, quote._externalPrice);
        if (quoteExternally) {
            if (quote._externalSize == 0) {
                if (isThereEnoughSize<Side>(book, _joinSize, quote._externalPrice)) {
                    quote._externalSize = calculateCandidateExternalQuoteSize<Side>(quoteSize, maxQuoteSizeBasedOnMarket);
                    changeInSize |= quote._externalSize != 0;
                }
            } else {
                if (!isThereEnoughSize<Side>(book, _leaveSize, quote._externalPrice)) {
                    // leave the book
                    quote._externalSize = 0;
                    changeInSize = true;
                }
                else {
                    hf::qty_t candidateSize = calculateCandidateExternalQuoteSize<Side>(quoteSize, maxQuoteSizeBasedOnMarket);
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

        hf::qty_t candidateInternalSize = _contract.toValidQty(getInternalQuoteSize<Side>(*this, maxQuoteSizeBasedOnMarket));
        if(candidateInternalSize != quote._internalSize) {
            quote._internalSize = candidateInternalSize;
            changeInSize = true;
        }

        if (changeInPrice | changeInSize | forceUpdate) {
            modifyQuoteOrder<Side, source>(quote, quoteExternally);
            hasChange = true;
        }
        return hasChange;
    }

    template<uint32_t Side>
    hf::qty_t calculateCandidateExternalQuoteSize(hf::qty_t quoteSize, hf::qty_t maxQuoteSizeBasedOnMarket) const {
        auto candidateSize = _contract.toValidQty(getExternalQuoteSize<Side>(*this, quoteSize, maxQuoteSizeBasedOnMarket));
        auto& quote = _quotes[Side];
        // Always allow size decrement. If size increased, the size increment has to be grater than quote size
        if (candidateSize > quote._lastExternalSizeSent) {
            if (((candidateSize - quote._lastExternalSizeSent) < _quoteSize) &&
                (candidateSize > std::min<hf::qty_t>(_quoteSize, maxQuoteSizeBasedOnMarket))) {
                candidateSize = quote._lastExternalSizeSent;
            }
        }
        return candidateSize;
    }

    template<uint32_t Side>
    std::tuple<hf::price_t, hf::price_t, hf::qty_t> calculateQuotePriceAndSizeByOrders(const md::top_t::level_t& book, hf::price_t price, bool quoteExternally) {
        if (!quoteExternally) {
            DLOG(0, "MM {C[i1]}. quoteExternally disabled. skip order qp based calculation", getContractName())
            return std::make_tuple(price, roundToTickSize<Side>(*this, price), _quoteSize);
        }

        auto targetPrice = roundToTickSize<Side>(*this, price);
        if (IsBetterPrice<Side>(targetPrice, book.pr)) {
            DLOG(0, "MM {C[i1]}. targetPrice {d1} better than TOB. Set targetPrice to TOB", getContractName(), ToDollars(targetPrice))
            targetPrice = book.pr;
            price = applyPriceOffset<Side>(targetPrice, _commission);
        }

        DLOG(0, "MM {C[i1]}. calculateQuotePriceAndSizeByOrders Side={i2} TOB={i3}({i4})@{d1} price={d2} roundedPrice={d3}", getContractName(),
             Side, book.q, book.o, ToDollars(book.pr), ToDollars(price), ToDollars(targetPrice));
        if (BOOST_UNLIKELY(_establishedLevel == 0)) {
            DLOG(0, "MM {C[i1]}. establishedLevel==0 bypass queue position evaluation", getContractName())
            return std::make_tuple(price, targetPrice, _quoteSize);
        }

        if (_queueImportance == 0) {
            DLOG(0, "MM {C[i1]}. queueImportance=0. bypass queue position evaluation", getContractName())
            return std::make_tuple(price, targetPrice, _quoteSize);
        }

        auto calculateInternalPrice = [this, price](hf::qty_t qp) -> std::tuple<hf::price_t, hf::price_t> {
            hf::price_t internalPrice;
            hf::price_t externalPrice;
            double pctThroughQueue = std::max<double>(std::min<double>(1.0 - (double)qp / _establishedLevel, 1.0), 0.0);
            double offsetScale = (2 / (1 + std::exp(-(double)_queueImportance * (pctThroughQueue - 0.5))) - 1);
            hf::price_t offset = offsetScale * _width;
            internalPrice = applyPriceOffset<Side>(price, offset);
            externalPrice = roundToTickSize<Side>(*this, internalPrice);
            DLOG(0, "MM {C[i1]}. calculateInternalPrice. qp={i2} establishedLevel={i3} queueImportance={i4} pctThroughQueue={d1} offsetScale={d2} offset={d3} internalPrice={d4} roundedPrice={d5}", getContractName(),
                 qp, _establishedLevel, _queueImportance, pctThroughQueue, offsetScale, ToDollars(offset), ToDollars(internalPrice), ToDollars(externalPrice))
            return std::make_tuple(internalPrice, externalPrice);
        };

        auto targetLevel = getPriceLevelIndex<Side>(targetPrice);
        auto targetSize = targetLevel == -1 ? 0 : _books[Side].l[targetLevel].q - _books[Side].l[targetLevel].ext.implied.impliedQtyNotTrackedByQueuePosition;
        auto iter = _priceOrders[Side].find(targetPrice);
        if (iter == _priceOrders[Side].end() || iter->second.empty()) {
            DLOG(0, "MM {C[i1]}. No orders placed at {d1} yet. assume qp at the back = {i2}", getContractName(), ToDollars(targetPrice), targetSize);
            auto [internalPrice, externalPrice] = calculateInternalPrice(targetSize);
            if (IsBetterPrice<Side>(targetPrice, externalPrice)) {
                // If price worse than target, should stay on target with size 0
                return std::make_tuple(price, targetPrice, 0);
            } else if (externalPrice != targetPrice) {
                // evaluated price better than target, should stay on the target
                return std::make_tuple(price, targetPrice, _quoteSize);
            }
            return std::make_tuple(internalPrice, externalPrice, _quoteSize);
        }

        hf::price_t cost = 0;
        hf::qty_t qty = 0;
        auto& orders = iter->second;
        bool orderUnwanted = false;
        for (auto* order : orders) {
            auto qp = order->qp;
            if (order->qp == om::strategy_order_t::queue_pos_t::NOT_SET) {
                DLOG(0, "MM {C[i1]}. Order {i2} queue position not updated yet. assume qp at the back = {i3}", getContractName(), order->orderId, targetSize);
                qp = targetSize;
            }
            DLOG(0, "MM {C[i1]}. calculateQuotePriceAndSizeByOrders. Order {i2} ord_price={d1} qty={i3} qp={i4}", getContractName(),
                 order->orderId, ToDollars(order->price), order->qty, order->qp)
            auto [internalPrice, externalPrice] = calculateInternalPrice(qp);

            if (IsPriceBetterOrEqual<Side>(externalPrice, targetPrice)) {
                cost += internalPrice * order->qty;
                qty += order->qty;
            } else {
                // order is unwanted, stop evaluating, all order on the back should be removed
                orderUnwanted = true;
                break;
            }
        }

        // If there is enough gap at the end, spray a new order
        if (!orderUnwanted && orders.back()->qp >= 0) {
            auto qtyBehind = targetSize - orders.back()->qp;
            if (qtyBehind >= _quoteGap) {
                // try adding sprayer order at the back
                LOG(0, "MM {C[i1]}. Try spray quote at {d1}. qp={i2}", getContractName(), ToDollars(targetPrice), targetSize)
                auto [internalPrice, externalPrice] = calculateInternalPrice(targetSize);

                if (IsPriceBetterOrEqual<Side>(externalPrice, targetPrice)) {
                    auto sprayQty = getSprayQty<Side>(qty, book, externalPrice);
                    if (sprayQty) {
                        LOG(0, "MM {C[i1]}. Add new spray quote {i2}@{d1}.", getContractName(), sprayQty, ToDollars(targetPrice))
                        cost += internalPrice * sprayQty;
                        qty += sprayQty;
                    }
                }
            }
        }
        if (qty) {
            auto internalPrice = cost/qty;
            auto externalPrice = roundToTickSize<Side>(*this, internalPrice);
            if (IsBetterPrice<Side>(targetPrice, externalPrice)) {
                // If price worse than target, should stay on target with size 0
                return std::make_tuple(price, targetPrice, 0);
            } else if (externalPrice != targetPrice) {
                return std::make_tuple(price, targetPrice, qty);
            }
            return std::make_tuple(internalPrice, externalPrice, qty);
        }
        return std::make_tuple(price, targetPrice, 0);
    }

    template<uint32_t Side>
    hf::qty_t getSprayQty(hf::qty_t currentCumOrderQty, const md::top_t::level_t& book, hf::price_t externalPrice) const noexcept {
        auto size = currentCumOrderQty + _quoteSize;
        auto maxQuoteSizeBasedOnMarket = getMaxQuoteSizeBasedOnMarket<Side>(book, _pctTobMaxQuote, externalPrice);
        size = calculateCandidateExternalQuoteSize<Side>(size, maxQuoteSizeBasedOnMarket);
        return std::max<hf::qty_t>(size - currentCumOrderQty, 0);
    }

    void notifyOrderRequest() {
        _orderRequestManager.notifyOrderRequest(_contract._internalId, StrategyId::MarketMaker, _lastOrderRequests);
    }

    template<uint32_t Side, uint32_t source>
    void modifyQuoteOrder(Quote& quote, bool persist) {
        _lastOrderRequests._quote[Side] = _orderRequestManager.sendQuoteRequest(_contract._internalId,
                                                                                QUOTE_ORDER_ID,
                                                                                Side, quote._internalPrice,
                                                                                quote._internalSize,
                                                                                quote._externalPrice,
                                                                                quote._externalSize, persist, false,
                                                                                _orderExecutionToken);
        LOG(0, "MM {C[i1]}. Update Quote. Side={i2} Internal: {i3}@{d1} External: {i4}@{d2} persist={i5} reqId={i6} source={i7} execToken={i8}", getContractName(),
            Side, quote._internalSize, ToDollars(quote._internalPrice), quote._externalSize,
            ToDollars(quote._externalPrice), persist, _lastOrderRequests._quote[Side], source, _orderExecutionToken)
        quote._lastInternalPriceSent = quote._internalPrice;
        quote._lastExternalSizeSent = quote._externalSize;
    }

    bool disableExternalQuotingAndUseGTC(const std::string& message, bool placeHedgeBack = true, bool notifyExtQuoteDisableToGUI = true) {
        bool msgSend = false;
        auto oldQuoteExternal = setQuoteExternalFlag(false);
        if (oldQuoteExternal) {
            if (notifyExtQuoteDisableToGUI) {
                _marketMaker->notifyFrontMMExternalQuoteDisabled(_contract._internalId);
            }
            _marketMaker->sendErrorMessage(message, ErrorType::Critical);
            msgSend = true;
        }

        auto oldUseGTC = *_contract._useGTC;
        if (oldUseGTC) {
            _marketMaker->sendGenericMessageToExecutor(ViperMessageType::ChangeUseGTC, _contract._internalId, false);
            if (!msgSend) {
                _marketMaker->sendErrorMessage(message, ErrorType::Critical);
            }
        }
        return oldQuoteExternal;
    }

    hf::price_t calculateMidPoint(const md::top_t::level_t& bid, const md::top_t::level_t& ask) const {
        //Weighted average in ungapped Market
        if(bid.pr + _tickSize == ask.pr) {
            const hf::qty_t askSize = std::min(ask.q, _establishedLevel);
            const hf::qty_t bidSize = std::min(bid.q, _establishedLevel);
            return (ask.pr * bidSize + bid.pr * askSize) / (bidSize + askSize);
        }
        return (bid.pr + ask.pr) / 2;
    }

    void calculateEstablishedLevel(double qty) {
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
                _establishedLevel = std::max<hf::qty_t>(_emaEstLevel * _estLevelPctEma, 1);
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

    hf::qty_t offsetPosition(uint32_t side, hf::qty_t remainingSize, hf::qty_t& allocatedInternally, hf::qty_t& allocatedExternally) {
        if (side == BID) {
            remainingSize = tryOffsetPosition<BID>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
            remainingSize = tryOffsetPosition<BID>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
        }
        else {
            remainingSize = tryOffsetPosition<ASK>(_internallyInitiatedPosition, remainingSize, allocatedInternally);
            remainingSize = tryOffsetPosition<ASK>(_externallyInitiatedPosition, remainingSize, allocatedExternally);
        }
        return remainingSize;
    }

    void onFill(const FillMessage& fill) {
        if (fill._source != FillSource::Quote && fill._source != FillSource::Hedge) {
            if (fill._strategy != StrategyId::MarketMaker) {
                LOG(0, "MM {C[i1]}. Handling fill transferred from strategy {i2}. Side={i3} Price={d1} Size={i4}",
                    getContractName(), strategyName(fill._strategy), fill._side, ToDollars(fill._price), fill._qty)
                forceTakeOverFill(fill._side, fill._price, fill._qty, fill._source, true);
                onEndOfFills(_orderExecutionToken);
            } else if (fill._forceOverFill) {
                forceTakeOverFill(fill._side, fill._price, fill._qty, fill._source, fill._checkLimit);
                // onEndOfFills event will follow
            } else {
                takeOverFill(fill);
                // onEndOfFills event will follow
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
                remainingSize = offsetPosition(fill._side, remainingSize, allocatedInternally, allocatedExternally);
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
                remainingSize = offsetPosition(fill._side, remainingSize, allocatedInternally, allocatedExternally);
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

    bool modifyHedgeSize() {
        bool hasHedgeChange = false;
        hf::qty_t quoteSize = calculateHedgeQuoteSize();
        assert(_contract._venue != hf::VENUE::B3 || (_contract.toValidQty(quoteSize) == quoteSize));
        if (quoteSize > 0) {
            Hedge& hedge = _hedges[BID];
            hasHedgeChange = hedge._sizeToQuote != quoteSize;
            hedge._sizeToQuote = quoteSize;
            Hedge& otherHedge = _hedges[ASK];
            if (hasHedgeChange) {
                LOG(0, "MM {C[i1]}. 0 Modifying hedge. InternalPosition={i2} ExternalPosition={i3} HedgeSize={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, quoteSize)
                modifyHedgeOrder<BID>(hedge, false);
            }
            if (otherHedge._sizeToQuote > 0) {
                otherHedge.reset();
                LOG(0, "MM {C[i1]}. 1 Modifying hedge. InternalPosition={i2} ExternalPosition={i3} HedgeSize=0", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
                modifyHedgeOrder<ASK>(otherHedge, false);
                hasHedgeChange = true;
            }
        }
        else if (quoteSize < 0) {
            quoteSize = -quoteSize;
            Hedge& hedge = _hedges[ASK];
            hasHedgeChange = hedge._sizeToQuote != quoteSize;
            hedge._sizeToQuote = quoteSize;
            Hedge& otherHedge = _hedges[BID];
            if (hasHedgeChange) {
                LOG(0, "MM {C[i1]}. 1 Modifying hedge. InternalPosition={i2} ExternalPosition={i3} HedgeSize={i4}", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition, quoteSize)
                modifyHedgeOrder<ASK>(hedge, false);
            }
            if (otherHedge._sizeToQuote > 0) {
                otherHedge.reset();
                LOG(0, "MM {C[i1]}. 0 Modifying hedge. InternalPosition={i2} ExternalPosition={i3} HedgeSize=0", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
                modifyHedgeOrder<BID>(otherHedge, false);
                hasHedgeChange = true;
            }
        }
        else {
            Hedge& bidHedge = _hedges[BID];
            if (bidHedge._sizeToQuote > 0) {
                bidHedge.reset();
                LOG(0, "MM {C[i1]}. 0 Modifying hedge. InternalPosition={i2} ExternalPosition={i3} HedgeSize=0", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
                modifyHedgeOrder<BID>(bidHedge, false);
                hasHedgeChange = true;
            }
            Hedge& askHedge = _hedges[ASK];
            if (askHedge._sizeToQuote > 0) {
                askHedge.reset();
                LOG(0, "MM {C[i1]}. 1 Modifying hedge. InternalPosition={i2} ExternalPosition={i3} HedgeSize=0", getContractName(), _internallyInitiatedPosition, _externallyInitiatedPosition)
                modifyHedgeOrder<ASK>(askHedge, false);
                hasHedgeChange = true;
            }
        }
        return hasHedgeChange;
    }

    template<uint32_t Side>
    void modifyHedgeOrder(Hedge& hedge, bool inPayUp) {
        _lastOrderRequests._hedge[Side] = _orderRequestManager.sendHedgeRequest(_contract._internalId, HEDGE_ORDER_ID,
                                                                                Side, hedge._internalPrice, hedge._externalPrice,
                                                                                hedge._sizeToQuote, true, inPayUp, _orderExecutionToken);
        DLOG(0, "MM {C[i1]}. Send hedge. Side={i2} Size={i3} InternalPrice={d1} ExternalPrice={d2} PayUp={i4} reqId={i5} execToken={i6}", getContractName(),
             Side, hedge._sizeToQuote, ToDollars(hedge._internalPrice), ToDollars(hedge._externalPrice), inPayUp,
             _lastOrderRequests._hedge[Side], _orderExecutionToken)
        hedge._lastInternalPriceSent = hedge._internalPrice;
    }

    template<uint32_t Side>
    bool calculateHedgePrices(hf::qty_t hedgeSize, hf::price_t& internalPrice, hf::price_t& externalPrice, const md::top_t::level_t& book, const md::top_t::level_t& otherSideBook) const noexcept {
        const bool inPayUp = isInPayUp(hedgeSize, otherSideBook);
        if (inPayUp) {
#ifdef IS_DEV
            LOG(0, "MM {C[i1]}. {i2} payUp hedge. hedgeSize={i3} otherBook={i4}({i5})@{d1}", getContractName(), Side,
                 hedgeSize, otherSideBook.q, otherSideBook.o, ToDollars(otherSideBook.pr))
#endif
            externalPrice = otherSideBook.pr;
            internalPrice = applyPriceOffset<Side>(externalPrice, _commission);
        } else {
            auto otherSide = 1 - Side;
            hf::price_t ref_price = getHedgeRefPrice<Side>(_quotes[otherSide]._internalPrice, otherSideBook.pr);
            internalPrice = BetterPrice<Side>(applyPriceOffset<Side>(ref_price, -_hedgeOffsetPrice), _midPoint);
            externalPrice = roundToTickSize<Side>(*this, internalPrice);
        }
        return inPayUp;
    }

    void onEndOfFills(uint32_t execToken, bool isManualFill = false) {
        LOG(0, "MM {C[i1]}. EndOfFill. internalPosition={i2} externalPosition={i3} execToken={i4}", getContractName(),
            _internallyInitiatedPosition, _externallyInitiatedPosition, execToken)
        assert(!_orderExecutionToken || execToken);
        _orderExecutionToken = execToken;

        //Likely offline GTC fill
        if(BOOST_UNLIKELY(!isManualFill && !_marketMaker->anyStrategyEnabled())) {
            [this]() NEVER_INLINE COLD {
                LOG(0, "MM {C[i1]}. Fill received while all strategies are disabled (maybe a gtc?). Check fills window!", getContractName());
                _marketMaker->sendErrorMessage("Market maker " + _contract._name + ". Fill received while all stregies are disabled. Check fills window!", ErrorType::Critical);
            }();
            return;
        }

        applyUpdates();
    }

    void onEndOfExternalFills(uint32_t side, uint32_t execToken) {
        LOG(0, "MM {C[i1]}. Side={i2} End of external fills. fillId={i3}", getContractName(), side, execToken)
        onEndOfFills(execToken);
    }

    void onEndOfInternalFills(uint32_t execToken) {
        LOG(0, "MM {C[i1]}. End of internal fills. FillId={i2}", getContractName(), execToken)
        onEndOfFills(execToken);
    }

    void applyUpdates() {
        if (!isEnabled() || !_contract._toMarketMake) {
            return;
        }

        const auto& bidBook = topBook<BID>();
        const auto& askBook = topBook<ASK>();

        if (BOOST_UNLIKELY(!hasMarketData(bidBook, askBook))) {
            return;
        }
        bool hedgeSizeChanged = modifyHedgeSize();
        if (forceUpdateQuotesUpdateHedgePrice() | hedgeSizeChanged) {
            notifyOrderRequest();
        }
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
        bool hasMD = ((topBook<BID>().pr != hf::NULL_PRICE) & (topBook<ASK>().pr != hf::NULL_PRICE));
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

            if (BOOST_UNLIKELY((topBook<ASK>().pr - topBook<BID>().pr) > _maxTicksWideDisable)) {
                _maxTicksWideDisableBreached = true;
                disable();
                LOG(0, "MM {C[i1]}. Not enabling. MaxTicksWidthDisable breached. ticksWide={i2} maxTicksWideDisable={i3}", getContractName(), (topBook<ASK>().pr - topBook<BID>().pr), _maxTicksWideDisable)
                _marketMaker->sendErrorMessage(boost::str(boost::format("Not enabling MM %1% - MaxTicksWideDisable breached")
                    % _contract._name), ErrorType::Critical);
                return false;
            }
            _maxTicksWideDisableBreached = false;
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
                setQuoteExternalFlag(false);
                _marketMaker->sendErrorMessage("MM - isWithinPositionLimits() check failed on start, disabling external quoting", ErrorType::Critical);
                _marketMaker->notifyFrontMMExternalQuoteDisabled(_contract._internalId);
            }

            Quote& bidQuote = _quotes[BID];
            bidQuote._externalSize = 0;
            Quote& askQuote = _quotes[ASK];
            askQuote._externalSize = 0;

            bool hasHedgeSizeChange = modifyHedgeSize();
            if (forceUpdateQuotesUpdateHedgePrice() | hasHedgeSizeChange) {
                notifyOrderRequest();
            }
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

    bool setQuoteExternalFlag(bool value, bool skip_logging = false) {
        bool cachedValue = _quoteExternal;
        _quoteExternal = value;
        if (!skip_logging & (cachedValue != value)) {
            LOG(0, "MM {C[i1]}. Quote external changed. Value={i2}", getContractName(), value);
        }
        return cachedValue;
    }

    bool disableExternalQuoting(bool skip_logging = false) {
        bool oldValue = setQuoteExternalFlag(false);
        if (oldValue) {
            applyUpdates();
        }
        return oldValue;
    }

    void enableExternalQuoting(bool notifyChange = true) {
        bool oldValue = setQuoteExternalFlag(true);
        if (!oldValue) {
            if (notifyChange) {
                _marketMaker->notifyFrontMMExternalQuoteEnabled(_contract._internalId);
            }
            applyUpdates();
        }
    }

    bool handleParametersUpdate(proto::GridCell& gridCell) {
        MarketMakerParametersColumns columnType = static_cast<MarketMakerParametersColumns>(gridCell.col());
        bool needRecalculate = true;
        switch (columnType) {
            case MarketMakerParametersColumns::QuoteExternal: {
                bool quoteExternal = 1 == atoi(gridCell.value().c_str());
                _quoteExternal = quoteExternal;
                LOG(0, "MM {C[i1]}. Quote external changed. Value={i2}", getContractName(), quoteExternal)
                break;
            }
            case MarketMakerParametersColumns::QuoteSize: {
                auto value = atoi(gridCell.value().c_str());
                if (BOOST_UNLIKELY(value <= 0)) {
                    LOG(0, "MM {C[i1]}. Quote size must be > 0. Change back to {i2}", getContractName(), _quoteSize)
                    _marketMaker->sendErrorMessage(boost::str(boost::format("MM quote size must be > 0, change back to %1%. Contract = %2%.") % _contract._name % _quoteSize), ErrorType::Critical);
                    gridCell.set_value(std::to_string(_quoteSize));
                } else {
                    if(_contract._venue == hf::VENUE::B3) {
                        hf::qty_t quoteSize = _contract.toValidQty(value);
                        if (quoteSize == 0) {
                            LOG(0, "MM {C[i1]}. Quote size must >= {i2}. Change back to {i3}", getContractName(), _contract._orderIncrementQty, _quoteSize)
                            _marketMaker->sendErrorMessage(boost::str(boost::format("MM quote size must be >= %1%, change back to %2%. Contract = %3%.") % _contract._orderIncrementQty % _quoteSize % _contract._name), ErrorType::Critical);
                            gridCell.set_value(std::to_string(_quoteSize));
                            break;
                        }

                        if (quoteSize != value) {
                            value = quoteSize;
                            gridCell.set_value(std::to_string(value));
                        }
                    }
                    _quoteSize = value;
                    LOG(0, "MM {C[i1]}. Quote size changed. Value={i2}", getContractName(), _quoteSize)
                }
                break;
            }
            case MarketMakerParametersColumns::QuoteGap: {
                _quoteGap = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. Quote gap changed. Value={i2}", getContractName(), _quoteGap)
                break;
            }
            case MarketMakerParametersColumns::QuotePctTob: {
                double candidateValue = boost::lexical_cast<double>(gridCell.value().c_str());

                if(candidateValue < 0) {
                    LOG(0, "MM {C[i1]}. PctTobMaxQuoteChange rejected, value<0. oldValue={d1} candidateValue={d2}", getContractName(), _pctTobMaxQuote, candidateValue);
                    _marketMaker->sendErrorMessage("MM %TobMaxQuoteChange must be >0", ErrorType::Critical);
                    gridCell.set_value(std::to_string(_pctTobMaxQuote));
                    needRecalculate = false;
                }
                else {
                    _pctTobMaxQuote = candidateValue;
                    LOG(0, "MM {C[i1]}. PctTobMaxQuote changed.  Value={i2}", getContractName(), candidateValue);
                }
                break;
            }
            case MarketMakerParametersColumns::HedgeOffset: {
                double value = boost::lexical_cast<double>(gridCell.value());
                if (value <= 0.0) {
                    LOG(0, "MM {C[i1]}. HedgeOffset must be > 0. Set Value back to {d1}", getContractName(), _hedgeOffset)
                    _marketMaker->sendErrorMessage("MM HedgeOffset Parameter must > 0.", ErrorType::Normal);
                    gridCell.set_value(std::to_string(_hedgeOffset));
                    needRecalculate = false;
                } else {
                    _hedgeOffset =  value;
                    _hedgeOffsetPrice = value * _tickSize;
                    LOG(0, "MM {C[i1]}. HedgeOffset changed. Value={d1}", getContractName(), _hedgeOffset)
                }
                break;
            }
            case MarketMakerParametersColumns::JoinSize: {
                _joinSize = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. Join size changed. Value={i2}", getContractName(), _joinSize)
                break;
            }
            case MarketMakerParametersColumns::LeaveSize: {
                _leaveSize = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. Leave size changed. Value={i2}", getContractName(), _leaveSize)
                break;
            }
            case MarketMakerParametersColumns::MaxPosInterrnal: {
                _maxInternallyInitiatedPosition = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. MaxPosInternal changed. Value={i2}", getContractName(), _maxInternallyInitiatedPosition)
                break;
            }
            case MarketMakerParametersColumns::MaxPosExternal: {
                _maxExternallyInitiatedPosition = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. MaxPosExternal changed. Value={i2}", getContractName(), _maxExternallyInitiatedPosition)
                break;
            }
            case MarketMakerParametersColumns::PayUpOrders: {
                _payUpOrders = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. PayUpOrders changed. Value={i2}", getContractName(), _payUpOrders)
                break;
            }
            case MarketMakerParametersColumns::PayUpQty: {
                _payUpQuantity = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. PayUpQty changed. Value={i2}", getContractName(), _payUpQuantity)
                break;
            }
            case MarketMakerParametersColumns::PayUpMultiple: {
                _payUpMultiple = atof(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. PayUpMultiple changed. Value={d1}", getContractName(), _payUpMultiple)
                break;
            }
            case MarketMakerParametersColumns::AsiaWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. AsiaWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::Asia) {
                    updateWidthPercent(widthPercentage);
                } else {
                    needRecalculate = false;
                }
                break;
            }
            case MarketMakerParametersColumns::EuroWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. EuroWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::Europe) {
                    updateWidthPercent(widthPercentage);
                } else {
                    needRecalculate = false;
                }
                break;
            }
            case MarketMakerParametersColumns::USWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. USWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::US) {
                    updateWidthPercent(widthPercentage);
                } else {
                    needRecalculate = false;
                }
                break;
            }
            case MarketMakerParametersColumns::SafeModeWidth: {
                auto widthPercentage = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. SafeModeWidth percent changed. Value={d1}", getContractName(), widthPercentage)
                if (_marketMaker->_viperConfig._currentShift == Shifts::SafeMode) {
                    updateWidthPercent(widthPercentage);
                } else {
                    needRecalculate = false;
                }
                break;
            }
            case MarketMakerParametersColumns::MinInternalPriceChange: {
                _minInternalPriceChange = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                LOG(0, "MM {C[i1]}. Min internal price change changed. Value={d1}", getContractName(), ToDollars(_minInternalPriceChange))
                break;
            }
            case MarketMakerParametersColumns::EstLevelEMASeconds: {
                auto value = boost::lexical_cast<uint32_t>(gridCell.value());
                _estLevelEmaPeriod = GetCyclesFromSeconds(value, _marketMaker->_frequency);
                LOG(0, "MM {C[i1]}. Established level EMA seconds changed. Value={i2}", getContractName(), value)
                break;
            }
            case MarketMakerParametersColumns::EstLevelPctEMA: {
                _estLevelPctEma = boost::lexical_cast<double>(gridCell.value());
                LOG(0, "MM {C[i1]}. Established level PctEMA changed. Value={d1}", getContractName(), _estLevelPctEma)
                break;
            }
            case MarketMakerParametersColumns::Commission: {
                _commission = ToRhonePrice(boost::lexical_cast<double>(gridCell.value()));
                LOG(0, "MM {C[i1]}. Commission changed. Value={d1}", getContractName(), _commission)
                break;
            }
            case MarketMakerParametersColumns::Enabled: {
                break;
            }
            case MarketMakerParametersColumns::MaxTicksWideStopExtQuoting: {
                int32_t value = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. MaxTicksWideStopExtQuoting changed. Value={i2}", getContractName(), value)
                if (value  < 0) {
                    _maxTicksWideStopExtQuoting = std::numeric_limits<int64_t>::max();
                    LOG(0, "MM {C[i1]}. Ignore MaxTicksWideStopExtQuoting checking.", getContractName())
                } else {
                    _maxTicksWideStopExtQuoting = value * _tickSize;
                }
                break;
            }
            case MarketMakerParametersColumns::MaxTicksWideDisable: {
                int32_t value = atoi(gridCell.value().c_str());
                LOG(0, "MM {C[i1]}. MaxTicksWideDisable changed. Value={i2}", getContractName(), value)
                if (value  < 0) {
                    _maxTicksWideDisable = std::numeric_limits<int64_t>::max();
                    LOG(0, "MM {C[i1]}. Ignore MaxTicksWideDisable checking.", getContractName())
                } else {
                    _maxTicksWideDisable = value * _tickSize;
                }
                break;
            }
            case MarketMakerParametersColumns::WorsenPayupCondPercent: {
                double worsenPayupConditionValue = boost::lexical_cast<double>(gridCell.value());
                if(worsenPayupConditionValue < 0 || IsZero(worsenPayupConditionValue)) {
                    LOG(0, "MM {C[i1]}. WorsenPayupCond% <= 0, refusing to change.  Value={d1} Amount={i2}", getContractName(), (_worsenPayupConditionAmount / static_cast<double>(_tickSize)), _worsenPayupConditionAmount);
                    _marketMaker->sendErrorMessage("MM WorsenPayupCond% must be > 0", ErrorType::Critical);
                    gridCell.set_value(std::to_string(_worsenPayupConditionAmount / static_cast<double>(_tickSize)));
                    needRecalculate = false;
                }
                else {
                    _worsenPayupConditionAmount = worsenPayupConditionValue * _tickSize;
                    LOG(0, "MM {C[i1]}. WorsenPayupCond% changed. Value={d1} Amount={i2}", getContractName(), worsenPayupConditionValue, _worsenPayupConditionAmount);
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
                break;
            }
            default:
                assert(false && "unhandled parameter update");
                needRecalculate = false;
                break;
        }
        return needRecalculate;
    }

    void handleEnableChange(bool enabled) {
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
    }

    void updateWidthPercent(double widthPercentage) {
        _widthPercentage = widthPercentage;
        LOG(0, "MM {C[i1]}. Update MMWidthPercentage to {d1}", getContractName(), _widthPercentage)
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
                                   std::forward_as_tuple(msg.price, msg.qty, msg.qp, msg.orderId, msg.side)).first;
            _priceOrders[msg.side][msg.price].emplace_back(&(iter->second));
        } else {
            auto& order = iter->second;
            if (order.price != msg.price) {
                // price changed, remove order from previous price level
                removeOrderFromPriceLevel(msg.side, order);
                if (msg.qty) {
                    order.price = msg.price;
                    order.qty = msg.qty;
                    order.qp = msg.qp;
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
                order.qp = msg.qp;
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
        if (BOOST_UNLIKELY(order.qp < om::strategy_order_t::queue_pos_t::NOT_SET)) {
            LOG(0, "MM {C[i1]}. Order {i2} has negative qp {i3}. set qp to 0", getContractName(), order.orderId, order.qp)
            order.qp = 0;
        }

        if (!isEnabled() || !canQuoteExternally()) {
            return;
        }

        bool hasChange = false;
        const auto& quote = _quotes[order.side];
        if (order.price == quote._externalPrice && std::next(orderIter) == _orders.end()) {
            // this is the last order on top level
            if (order.side == BID) {
                hasChange |= updateQuote<BID, 8>(topBook<BID>(), true);
            } else {
                hasChange |= updateQuote<ASK, 8>(topBook<ASK>(), true);
            }
        }

        if (hasChange) {
            notifyOrderRequest();
        }
    }

    void onEconomicNumber(EconomicNumber::Color color) {
        _quoteExternalBeforeEconomicNumber = _quoteExternal;
        if (_quoteExternal) {
            LOG(0, "MM {C[i1}. Disable quote external on {i2} economic number", getContractName(), toUInt(color));
            _quoteExternal = false;
            _marketMaker->notifyFrontMMExternalQuoteDisabled(_contract._internalId);
            applyUpdates();
        }
    }

    void onEconomicNumberEnded(EconomicNumber::Color color) {
        if (_maxTicksWideDisableBreached) {
            LOG(0, "MM {C[i1}. Not Enable quote external on {i2} economic number ended due to maxTickWideDisable breached.", getContractName(), toUInt(color));
            _marketMaker->sendErrorMessage(boost::str(boost::format(
                    "Not Enable %1% MM quote external on %2% economic number ended due to maxTickWideDisable breached.") %
                    _contract._name % to_string(color)), ErrorType::Critical);
        } else {
            _marketMaker->sendGenericMessageToExecutor(ViperMessageType::CanEnableUseGTC, _contract._internalId);
            if (_quoteExternalBeforeEconomicNumber & !_maxTicksWideStopExtQuoting) {
                LOG(0, "MM {C[i1}. Enable quote external on {i2} economic number ended", getContractName(), toUInt(color));
                enableExternalQuoting();
            } else {
                LOG(0, "MM {C[i1}. Not Enable quote external on {i2} economic number ended. maxTicksWideStopExtQuoting breached", getContractName(), toUInt(color));
                _marketMaker->sendErrorMessage(boost::str(boost::format(
                        "Not Enable %1% MM quote external on %2% economic number ended. maxTickWideStopQuoting breached.") %
                                                          _contract._name % to_string(color)), ErrorType::Normal);
            }
        }

    }

#ifdef IS_DEV
    std::string getContractName() const noexcept {
        return _contract._name;
    }
#else
    uint32_t getContractName() const noexcept {
            return _contract._internalId;
        }
#endif

    bool _contractCanTrade = false;
    bool _contractCanCancel = false;
    bool _gatewayActive = true;
    bool _enabled = false;
    bool _quoteExternal;
    bool _quoteExternalBeforeEconomicNumber = false;
    bool _quoteExternalBeforeMaxTicksWideStopQuotingBreached = false;
    hf::qty_t _quoteSize;
    hf::qty_t _quoteGap;
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
    hf::qty_t _establishedLevel = 1;
    uint64_t _estLevelEmaPeriod;
    uint64_t _lastEmaTime = 0;
    double _estLevelPctEma;
    hf::price_t _tickSize;
    hf::price_t _hedgeOffsetPrice;
    hf::price_t _maxTicksWideStopExtQuoting;
    hf::price_t _maxTicksWideDisable;
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
    uint32_t _orderExecutionToken = 0;
    uint32_t _queueImportance = 0;
    hf::batch_id_t _lastMDbid = 0;
    md::top_t _books[2];
    int8_t _mdPriceChangeIndex[2] = {-1, -1};
    int8_t _mdQtyChangeIndex[2] = {-1, -1};

    struct OrderInfo {
        hf::price_t price;
        hf::qty_t qty;
        hf::qty_t qp;
        uint32_t orderId;
        uint32_t side;

        OrderInfo(hf::price_t p, hf::qty_t q, hf::qty_t qp, uint32_t id, uint32_t s)
            : price(p), qty(q), qp(qp), orderId(id), side(s)
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
inline hf::qty_t getExternalQuoteSize<Python::BID>(const Python::SimpleMarketMaker& mm, hf::qty_t quoteSize, hf::qty_t maxQuoteSize) {
    if (mm._externallyInitiatedPosition < 0) {
        return std::min(quoteSize, std::min(mm._maxExternallyInitiatedPosition, maxQuoteSize));
    }
    if (mm._maxExternallyInitiatedPosition <= mm._externallyInitiatedPosition) {
        return 0;
    }
    return std::min(quoteSize, std::min(mm._maxExternallyInitiatedPosition - mm._externallyInitiatedPosition, maxQuoteSize));
}

template<>
inline hf::qty_t getExternalQuoteSize<Python::ASK>(const Python::SimpleMarketMaker& mm, hf::qty_t quoteSize, hf::qty_t maxQuoteSize) {
    if (mm._externallyInitiatedPosition > 0) {
        return std::min(quoteSize, std::min(mm._maxExternallyInitiatedPosition, maxQuoteSize));
    }
    if (mm._maxExternallyInitiatedPosition <= -mm._externallyInitiatedPosition) {
        return 0;
    }
    return std::min(quoteSize, std::min(mm._maxExternallyInitiatedPosition + mm._externallyInitiatedPosition, maxQuoteSize));
}

template<>
inline hf::price_t roundToTickSize<Python::BID>(const Python::SimpleMarketMaker& mm, hf::price_t price) {
    return Python::RoundToTickSize(price - mm._commission, mm._tickSize, Python::BID);
}

template<>
inline hf::price_t roundToTickSize<Python::ASK>(const Python::SimpleMarketMaker& mm, hf::price_t price) {
    return Python::RoundToTickSize(price + mm._commission, mm._tickSize, Python::ASK);
}

}

#endif //RHONEOMS_SIMPLE_MARKET_MAKER_H
