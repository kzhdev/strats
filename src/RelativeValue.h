#ifndef VIPER_RELATIVE_VALUE_H
#define VIPER_RELATIVE_VALUE_H

#include <boost/format.hpp>
#include <md/md.h>
#include <functional>
#include <limits>
#include <set>
#include <util/ConfigManager.h>
#include "Common.h"
#include "ViperConfig.h"
#include "MathStructures.h"
#include "ClockUtil.h"
#include "UpdateSelector.h"
#include "logging/Coolog.h"
#include "ViperStrategyBase.h"
#include "OrderRequestManager.h"
#include "MarketDataConsumer.h"
#include "PerfMonitor.h"

namespace Python {

struct Lean {
    Market* _market;
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
    hf::price_t _price;
    double _size;
    bool _isExternal = false;
};


template<uint32_t Side>
struct LeanSorter {
    static constexpr double priceFactor = (Side == BID) ? 1.0 : -1.0;
    bool isBetterPrice(hf::price_t p1, hf::price_t p2);

    bool operator()(Lean& a, Lean& b) const {
        auto aValid = a._size >= 1.0;
        auto bValid = b._size >= 1.0;
        // 1. if one lean is valid and other is not, consider the valid better.
        // 2. if both same validity
        //    a. If side is BID, bigger price is best
        //    b. If side is ASK, lower price is best (hence priceFactor == -1)
        auto aPair = std::make_pair(aValid, a._price * priceFactor);
        auto bPair = std::make_pair(bValid, b._price * priceFactor);
        return aPair > bPair;  // pair ordering is lexicographical
    }
};


struct Leans {

    void add(Market& market, uint32_t leanIndex, uint32_t marketIndex, hf::price_t price, double size, bool isExternal) {
        Lean& lean = _leans[_leanIndex];
        lean._market = &market;
        lean._instrumentIndex = leanIndex;
        lean._marketIndex = marketIndex;
        lean._price = price;
        lean._size = size;
        lean._isExternal = isExternal;
        ++_leanIndex;
    }

    void sortBids() {
        auto end = _leans.begin() + _leanIndex;
        std::sort(_leans.begin(), end, LeanSorter<BID>());
    }

    void sortAks() {
        auto end = _leans.begin() + _leanIndex;
        std::sort(_leans.begin(), end, LeanSorter<ASK>());
    }

    uint32_t getNumberOfLeans() const noexcept {
        return _leanIndex;
    }

    Lean* getLean_FOR_TESTING(uint32_t instrumentIndex) noexcept {
        for (uint32_t index = 0; index < _leanIndex; ++index) {
            Lean& lean = _leans[index];
            if (lean._instrumentIndex == instrumentIndex) {
                return &lean;
            }
        }
        return nullptr;
    }

    void clear() noexcept {
        _leanIndex = 0;
    }

    uint32_t _leanIndex = 0;
    std::array<Lean, 1000> _leans;
};

template<uint32_t Side>
inline hf::price_t calculateHedgeCost(const Market& leanMarket, hf::price_t leanFair) noexcept;

template<>
inline hf::price_t calculateHedgeCost<BID>(const Market& leanMarket, hf::price_t leanFair) noexcept {
    return leanFair - leanMarket.getPrice();
}

template<>
inline hf::price_t calculateHedgeCost<ASK>(const Market& leanMarket, hf::price_t leanFair) noexcept {
    return leanMarket.getPrice() - leanFair;
}

template<uint32_t Side>
inline hf::price_t calculateQuotePrice(hf::price_t fair, hf::price_t width, hf::price_t hedgingCost, hf::price_t multipliedCommission) noexcept;

template<>
inline hf::price_t calculateQuotePrice<BID>(hf::price_t fair, hf::price_t width, hf::price_t hedgingCost, hf::price_t multipliedCommission) noexcept {
    return fair - width - hedgingCost - multipliedCommission;
}

template<>
inline hf::price_t calculateQuotePrice<ASK>(hf::price_t fair, hf::price_t width, hf::price_t hedgingCost, hf::price_t multipliedCommission) noexcept {
    return fair + width + hedgingCost + multipliedCommission;
}

struct Quote {
    Leans _leans;

    hf::price_t _bestPrice = hf::NULL_PRICE;
    hf::qty_t _bestSize = 0;
    uint32_t _bestLeanIndex = 0;

    hf::price_t _externalPrice = hf::NULL_PRICE;
    hf::qty_t _externalSize = 0;

    uint32_t _previousBestLeanIndex = 0;
    bool _leanMarketGone = false;
    bool _hasChange = false;

    void resetBest() {
        _bestPrice = hf::NULL_PRICE;
        _bestSize = 0;
        _bestLeanIndex = 0;
        _previousBestLeanIndex = 0;
    }

    template <typename ContractsT, typename ValidateMaxTicksThroughMarketFunc, typename LogErrorFunc>
    bool createQuote(hf::price_t tickSize,
                     uint32_t side,
                     bool quoteExternal,
                     ContractsT& contracts,
                     uint32_t contractId,
                     bool isEnabled,
                     bool staticQuote,
                     uint32_t quoteSize,
                     ValidateMaxTicksThroughMarketFunc&& validateMaxTicksThroughMarketFunc,
                     LogErrorFunc&& logErrorFunc) {
        if (BOOST_LIKELY(_leans.getNumberOfLeans() > 0)) {
            _leanMarketGone = false;

            if (side == BID) {
                _leans.sortBids();
            } else {
                _leans.sortAks();
            }

            Lean& bestLean = _leans._leans[0];

            auto oldBestPrice = _bestPrice;
            auto oldBestSize = _bestSize;
            auto oldExtPrice = _externalPrice;
            auto oldExtSize = _externalSize;

            _bestPrice = bestLean._price;
            _bestSize = contracts[contractId]._definition.toValidQty(static_cast<hf::qty_t>(bestLean._size + 0.0001));
            _bestLeanIndex = bestLean._instrumentIndex;

            bool hasChange = false;

            auto& contract = contracts[contractId];
            bool internalPriceChanged = std::abs(oldBestPrice - _bestPrice) > contract._priceChangeThreshold;
            hasChange |= (internalPriceChanged | (oldBestSize != _bestSize));

            auto checkExternalChange = [this, &contract, oldExtPrice, oldExtSize]() -> bool {
                bool externalPriceChanged = std::abs(oldExtPrice - _externalPrice) > contract._priceChangeThreshold;
                return (externalPriceChanged | (oldExtSize != _externalSize));
            };

            hf::price_t roundedPrice = hf::NULL_PRICE;
            hf::qty_t clubSize = 0;
            uint32_t clubCount = 0;

            if (quoteExternal) {
                for (uint32_t leanIndex = 0; leanIndex < _leans.getNumberOfLeans(); ++leanIndex) {
                    Lean& lean = _leans._leans[leanIndex];
                    hf::price_t price = RoundToTickSize(lean._price, tickSize, side);
                    if (roundedPrice == hf::NULL_PRICE) {
                        roundedPrice = price;
                        clubSize = static_cast<hf::qty_t>(lean._size);
                        ++clubCount;
                        // LOG(0, "Quote {C[i1]}. Added lean {C[i2]} Size={d1} Price={d2}", getContract(contractId, contracts),  getContract(lean._instrumentIndex, contracts), lean._size, ToDollars(lean._price))
                    } else if (roundedPrice == price) {
                        clubSize += static_cast<hf::qty_t>(lean._size);
                        ++clubCount;
                       // LOG(0, "Quote {C[i1]}. Added lean {C[i2]} Size={d1} Price={d2}", getContract(contractId, contracts),  getContract(lean._instrumentIndex, contracts), lean._size, ToDollars(lean._price))
                    } else {
                        break;
                    }

                    if(BOOST_LIKELY(isEnabled)) {
                        const auto &validationResult = validateMaxTicksThroughMarketFunc(lean._price, lean._instrumentIndex);
                        if (BOOST_UNLIKELY(!validationResult.isValid)) {
                            logValidateMaxTicksThroughMarketError(side, contractId, validationResult, lean, contracts,
                                                                  logErrorFunc);
                            _externalPrice = roundedPrice;
                            _externalSize = 0;
                            return false;
                        }
                    }
                }
                _externalPrice = roundedPrice;
                _externalSize = contracts[contractId]._definition.toValidQty(staticQuote ? quoteSize : std::max(_bestSize, static_cast<hf::qty_t>(clubSize / clubCount + 0.0001)));
                hasChange |= checkExternalChange();
            }
            else {
                _externalPrice = hf::NULL_PRICE;
                _externalSize = 0;
                hasChange |= checkExternalChange();
                if(BOOST_LIKELY(isEnabled)) {
                    const auto &validationResult = validateMaxTicksThroughMarketFunc(bestLean._price, bestLean._instrumentIndex);
                    if (BOOST_UNLIKELY(!validationResult.isValid)) {
                        logValidateMaxTicksThroughMarketError(side, contractId, validationResult, bestLean, contracts, logErrorFunc);
                        return false;
                    }
                }
            }

            _hasChange |= hasChange;
            auto& leanContract = contracts[bestLean._instrumentIndex];
            auto& market = bestLean._isExternal ? leanContract.externalMarket(side) : leanContract.internalMarket(side);


            if (hasChange || (_previousBestLeanIndex != bestLean._instrumentIndex)) {
                LOG(0, "RV {C[i1]}. Quote. Side={i2} Internal: {i3}@{d1} External: {i4}@{d2} Lean={C[i5]} {i6}@{d3} MarketId={i7}",
                    getContract(contractId, contracts), side, _bestSize, ToDollars(_bestPrice), _externalSize, ToDollars(_externalPrice), getContract(bestLean._instrumentIndex, contracts),
                    market.getAdjustedSize(), ToDollars(market.getPrice()), market.getId())
            }

            _previousBestLeanIndex = bestLean._instrumentIndex;
        } else if (!_leanMarketGone) {
            _bestPrice = hf::NULL_PRICE;
            _bestSize = 0;
            _externalPrice = hf::NULL_PRICE;
            _externalSize = 0;
            _leanMarketGone = true;
            _hasChange = true;
        }
        return true;
    }

#ifdef IS_DEV
        template <typename ContractsT>
        std::string getContract(uint32_t id, ContractsT& contracts) const {
            return contracts[id]._definition._name;
        }
#else
        template <typename ContractsT>
        uint32_t getContract(uint32_t id, ContractsT& contracts) const {
            return id;
        }
#endif

private:
        template<typename ContractsT, typename LogErrorFunc, typename ValidationResultT>
        NEVER_INLINE COLD void logValidateMaxTicksThroughMarketError(uint32_t side, uint32_t contractId, const ValidationResultT& validationResult, const Lean& lean, ContractsT&& contracts, LogErrorFunc&& logErrorFunc) const {
            auto& leanContract = contracts[lean._instrumentIndex];
            auto& quoteContract = contracts[contractId];
            auto otherSide = [side](){return side==BID ? ASK : BID;};

            LOG(0, "Quote Violated MaxTicksThroughMarket.  Contract={C[i1]} Side={i2} Lean={C[i3]} InternalQuote={i4}@{d1} ExternalQuote={i5}@{d2} HedgeMarket(MMHedge:0;MM:1,AS:3,EXT:4)={i6} quoteWithoutCap={d3} leanMarket(ss)=({d4};{d5})",
                getContract(contractId, contracts), side, getContract(lean._instrumentIndex, contracts), _bestSize, ToDollars(_bestPrice), _externalSize, ToDollars(_externalPrice),
                lean._marketIndex, ToDollars(validationResult.quoteWithoutCap), ToDollars(leanContract.externalMarket(side).getPrice()), ToDollars(leanContract.internalMarket(side).getPrice()))
            LOG(0, "  ...QuoteViolatedMaxTicksThroughMarket (continues) quoteMarket(os)=({d1};{d2})",
                ToDollars(quoteContract.externalMarket(otherSide()).getPrice()), ToDollars(quoteContract.internalMarket(otherSide()).getPrice()))

            //We really need a "log both" method . . .
            boost::format firstFormatString("Quote Violated MaxTicksThroughMarket. Contract=%1% Side=%2% Lean=%3% rvExternalQuote=%4%@%5% rvInternalQuote=%6%@%7% HedgeMarket(mmhedge:0;mm:1,as:3,ext:4)=%8%");
            logErrorFunc(boost::str(firstFormatString
                                    % contracts[contractId]._definition._name % side % contracts[lean._instrumentIndex]._definition._name
                                    %  _externalSize % ToDollars(_externalPrice) % _bestSize % ToDollars(_bestPrice)
                                    % lean._marketIndex));

            boost::format secondFormatString("    rvQuoteWithoutCap=%1% quoteMarket=(%2%,%3%) leanMarket=(%4%,%5%)");
            logErrorFunc(boost::str(secondFormatString % ToDollars(validationResult.quoteWithoutCap)
                                    % ToDollars(quoteContract.externalMarket(otherSide()).getPrice())
                                    % ToDollars(quoteContract.internalMarket(otherSide()).getPrice())
                                    % ToDollars(leanContract.externalMarket(side).getPrice())
                                    % ToDollars(leanContract.internalMarket(side).getPrice())));
        }
};

struct RvContract {
    ConfigContract _definition;
    Market _markets[2][2];  // first: BID, ASK; Second: 0 - internal; 1 - external
    Quote _quotes[2];

    double _pricingMultiplier;
    double _quantityMultiplier;
    double _priceChangeThreshold;
    hf::qty_t _establishedLevel;
    uint32_t _internalId;
    hf::batch_id_t _lastMDbid = 0;
    bool _canTradeStatus = false;
    bool _traded = false;
    bool _usedInPricing = false;
    bool _logNoPrice = false;

    void clearLeans() {
        _quotes[BID]._leans.clear();
        _quotes[ASK]._leans.clear();
    }

    template<uint32_t Side>
    Market& externalMarket() noexcept {
        return _markets[Side][Market::External];
    }

    template<uint32_t Side>
    const Market& externalMarket() const noexcept {
        return _markets[Side][Market::External];
    }

    Market& externalMarket(uint32_t side) noexcept {
        return _markets[side][Market::External];
    }

    const Market& externalMarket(uint32_t side) const noexcept {
        return _markets[side][Market::External];
    }

    template<uint32_t Side>
    Market& internalMarket() noexcept {
        return _markets[Side][Market::Internal];
    }

    template<uint32_t Side>
    const Market& internalMarket() const noexcept {
        return _markets[Side][Market::Internal];
    }

    Market& internalMarket(uint32_t side) noexcept {
        return _markets[side][Market::Internal];
    }

    const Market& internalMarket(uint32_t side) const noexcept {
        return _markets[side][Market::Internal];
    }

    template<uint32_t Side>
    Market& bestMarket() noexcept {
        return BetterMarket<Side>(_markets[Side][Market::Internal], _markets[Side][Market::External]);
    }

    void initMarkets(uint32_t contractId) {
        _markets[BID][Market::Type::Internal].init(StrategyId::RelativeValue, BID, MarketIds::__end__, contractId);
        _markets[BID][Market::Type::External].init(StrategyId::RelativeValue, BID, MarketIds::MARKET_ID_EXTERNAL, contractId);
        _markets[ASK][Market::Type::Internal].init(StrategyId::RelativeValue, ASK, MarketIds::__end__, contractId);
        _markets[ASK][Market::Type::External].init(StrategyId::RelativeValue, ASK, MarketIds::MARKET_ID_EXTERNAL, contractId);
    }
};

class RelativeValue final : public ViperStrategyBaseImpl<RelativeValue> {
public:
    RelativeValue()
        : _updateSelector(*this)
        , _orderRequestMgr(OrderRequestManager::get())
    {
#ifdef VIPER_UNIT_TEST
        _msgQueue.init(DEFAULT_QUEUE_SIZE);
#endif
    }

    ResourceId id() const noexcept override { return ResourceId::RelativeValue; }
    StrategyId strategyId() const noexcept override { return StrategyId::RelativeValue; }

    void registerOrderClient() {
        for (uint32_t contractIndex = _indexOfFirstTradedContract; contractIndex < _numberOfAllContracts; ++contractIndex) {
            RvContract& contract = _contracts[contractIndex];
            sendRegisterOrderClientRequest(contract._internalId, {StrategyId::MarketMaker, StrategyId::ArbieSpreader});
        }
    }

    void runCycle(uint64_t now) {
#ifdef ENABLE_PERF
        auto begin = clockUtil::getCpuTicksStart();
#endif
        if (_numberOfFills > 0) {
            LOG(0, "RV drained number of fills. {i1}", _numberOfFills)
        }

        const bool calculateDueToHedges = processHedgeRequests();
#ifdef ENABLE_PERF
        bool addPerf = calculateDueToHedges;
#endif
        if (_hadExternalMarketData) {
#ifdef ENABLE_PERF
            addPerf = true;
#endif
            recalculateAfterExternalMarketDataChange();
        }

        _canSendQuotes = _hedgeRequests.empty() & !_withinInternalFillsOfCycle;
        const bool recalculateQuotes = (canActOnMarketData() & (_calculateDueToFront | _hadInternalMarketData  | _hadExternalMarketData))
                | calculateDueToHedges | checkEMAOnOffTime(now);
        if (recalculateQuotes) {
#ifdef ENABLE_PERF
            addPerf = true;
#endif
            _hasPendingQuoteCalculation = _withinInternalFillsOfCycle;
            calculateQuotes();
        } else if (_endOfQuotes & _hasPendingQuoteCalculation){
#ifdef ENABLE_PERF
            addPerf = true;
#endif
            _hasPendingQuoteCalculation = false;
            calculateQuotes();
        }

        // reset flags
        _calculateDueToFront = false;
        _hadInternalMarketData = false;
        _hadExternalMarketData = false;
        _numberOfFills = 0;
#ifdef ENABLE_PERF
        if (addPerf) {
            auto end = clockUtil::getCpuTicksEnd();
            PerfMonitor::get()->addPerfPoint(PerfId::RV_RunCycle, PerfType::Begin, begin);
            PerfMonitor::get()->addPerfPoint(PerfId::RV_RunCycle, PerfType::End, end);
        }
#endif
    }

#ifdef IS_DEV
    std::string getVectorName(uint32_t id) {
        return _vectorNames[id];
    }
#else
    uint32_t getVectorName(uint32_t id) {
        return id;
    }
#endif

    bool checkEMAOnOffTime(uint64_t now, bool recalculateFairs = true) {
        bool shouldRecalculate = false;
        for (uint32_t i = 0; i < _numberOfVectors; ++i) {
            shouldRecalculate |= checkEMAOnOffTime(now, i);
        }

        if (shouldRecalculate & recalculateFairs) {
            calculateLongTermFairsInVector();
            calculateShortTermFairs();
        }
        return shouldRecalculate;
    }

    bool checkEMAOnOffTime(uint64_t now, uint32_t vectorId) {
        bool shouldRecalculate = false;
        if (_useEMAInVector.getValue(vectorId)) {
            // EMA already on, check EMA Off time
            auto offTime = _emaOffTime.getValue(vectorId);
            if (offTime != (uint64_t)-1 && now >= offTime) {
                LOG(0, "Turn off EMA for factor {V[i1]} due to EMAOff timer", getVectorName(vectorId));
                setUseEMA(vectorId, false, boost::str(boost::format("Turn off EMA for factor %1%, due to EMAOff timer") % _vectorNames[vectorId]));
                // change ema off time to next day
                _emaOffTime.setValue(vectorId, changeTimeToNextDay(offTime));
                shouldRecalculate = true;
            }

            // make sure the on time changed to next day
            auto onTime = _emaOnTime.getValue(vectorId);
            if (onTime != (uint64_t)-1 && now >= onTime) {
                _emaOnTime.setValue(vectorId, changeTimeToNextDay(onTime));
            }
        } else {
            auto onTime = _emaOnTime.getValue(vectorId);
            if (onTime != (uint64_t)-1 && now >= onTime) {
                LOG(0, "Turn on EMA for factor {i1} due to EMAOn timer", getVectorName(vectorId));
                setUseEMA(vectorId, true, boost::str(boost::format("Turn on EMA for factor %1%, due to EMAOn timer") % _vectorNames[vectorId]));
                _emaOnTime.setValue(vectorId, changeTimeToNextDay(onTime));
                shouldRecalculate = true;
            }

            // make sure the off time changed to next day
            auto offTime = _emaOffTime.getValue(vectorId);
            if (offTime != (uint64_t)-1 && now >= offTime) {
                _emaOffTime.setValue(vectorId, changeTimeToNextDay(offTime));
            }
        }

        return shouldRecalculate;
    }

    void setUseEMA(uint32_t vectorId, bool isOn, const std::string& msg) {
        _useEMAInVector.setValue(vectorId, isOn);
        sendErrorMessage(msg, ErrorType::Normal);
        sendGenericMessageToFront(ViperMessageType::EmaOnOff, vectorId, isOn);
    }

    void publishLatencyTrackingStatsAndTracking() {
        if(_latencyTracking.isset()) {
            sendMessageToFront<RVLatencyTrackingMessage>( ViperMessageType::RVLatencyTracking,
                    [&](RVLatencyTrackingMessage &message) {
                        message._fields = _latencyTracking;
                        message._hedgeId = getLastHedgeId();
                    });
        }
        _latencyTracking.unset();
    }

    bool processHedgeRequests() {
        const bool hasHedgeRequests = !_hedgeRequests.empty();
        if (hasHedgeRequests) {
            SCOPED_PERF_POINT(PerfId::RV_HedgeProcess);

            LOG(0, "RV. Processing hedge requests. {i1}", _hedgeRequests.size())
            for (auto it = _hedgeRequests.begin(); it != _hedgeRequests.end(); ) {
                const uint32_t contractId = it->first;
                double requestSize = it->second;
                const uint32_t requestSide = requestSize > 0 ? BID : ASK;
                const double residuals = getResiduals(contractId, requestSide);
                const bool sameSide = ((requestSize >= 0) & (residuals >= 0)) | ((requestSize  <= 0) & (residuals <= 0));
                double size = requestSize + residuals;
                double absSize = std::abs(size);
                const uint32_t hedgeSide = size > 0 ? BID : ASK;
                RvContract& contract = _contracts[contractId];
                LOG(0, "RV {C[i1]}. Hedge request. AbsSize={d1} Side={i2} residuals={d2} reqSize={d3}", getContract(contractId), absSize, hedgeSide, residuals, requestSize)
                bool removeRequest = false;
                bool abort = false;
                double leftToHedge = absSize;
                if(BOOST_LIKELY(absSize <= _rhoneMaxClipSize.getValue(contractId))) {
                    leftToHedge = sendHedges(contract, absSize, hedgeSide, removeRequest, std::abs(residuals));
                }
                else {
                    LOG(0, "RV {C[i1]}. Absolute size greater than rhone limit.  Aborting RV Hedge Loop. LeftToHedge={d1} residual={d2}", getContract(contract._internalId), leftToHedge, residuals);
                    disableRelativeValue("RV Absolute size greater than rhone limit.  Aborting RV Hedge Loop and disabling.  Contract=" +
                        _viperConfig._allContracts[contract._internalId]._name + " LeftToHedge=" + std::to_string(leftToHedge) + " Residual=" + std::to_string(residuals));
                    abort = removeRequest = true;
                }
                if (removeRequest) {
                    addPosition(contract, requestSize, true, false);
                    it = _hedgeRequests.erase(it);
                    LOG(0, "RV {C[i1]}. Ending hedging loop. LeftToHedge={d1}", getContract(contract._internalId), leftToHedge);
                } else {
                    double left;
                    double hedgedSize;
                    if (sameSide) {
                        left = std::max<double>(0, leftToHedge - std::abs(residuals));
                        hedgedSize = std::abs(requestSize) - left;
                    } else {
                        left = leftToHedge;
                        hedgedSize = std::abs(requestSize) - left;
                    }
                    addPosition(contract, requestSide == BID ? hedgedSize : -hedgedSize, true, false);
                    if (IsZero(left)) {
                        it = _hedgeRequests.erase(it);
                    } else {
                        it->second = requestSide == BID ? left : -left;
                    }
                    LOG(0, "RV {C[i1]}. Not fully hedged. LeftToHedge={d1} Left={d2} hedged={d3}", getContract(contract._internalId), leftToHedge, left, hedgedSize)
                }

                if(BOOST_UNLIKELY(abort)) {
                    return true;
                }

                break; // do hedge only one contract at a time - temporary fix
            }
            calculateShortTermFairs();
            publishLatencyTrackingStatsAndTracking();
        }
        return hasHedgeRequests;
    }

    double getResiduals(uint32_t contractId, uint32_t side) {
        RvContract& contract = _contracts[contractId];
        double position = 0;
        Quote& quote = contract._quotes[side];
        Lean& lean = quote._leans._leans[0];
        int vectorIndexSigned = _factorHedgedIndexes.getValue(contractId, lean._instrumentIndex);
        if (vectorIndexSigned != -1) {
            auto vectorIndex = (uint32_t) vectorIndexSigned;
            if (_factorHedged.getValue(vectorIndex)) {
                double vectorRisk = _vectorRiskMatrix.getValue(vectorIndex, contractId);
                if (!IsZero(vectorRisk)) {
                    double vectorPos = _positionsInVector.getValue(vectorIndex);
                    position += vectorPos / _vectorRisk_Div_qtyMultiplier_Matrix.getValue(vectorIndex, contractId);
                }
                LOG(0, "{C[i1]} getResiduals vectorIndex={i2} vectorRisk={d1} position={d2}", getContract(contract._internalId), vectorIndex, vectorRisk, position)
            }
        }
        return position;
    }

    // Handle system status

    void onQuoteExternalChanged(SystemState quoteExt) {
        calculateQuotes();
    }

    void onExchangeQuoteExternalChanged(const SharedSystemState::QuoteExtBitset& quoteExt) {
        calculateQuotes();
    }

    void cancelGtcAndDisable() {
        handleDisabled();
    }
    void handleHardShutdown() {}

    void handleDisabled() {
        for (uint32_t contractIndex = 0; contractIndex < _numberOfPricingContracts; ++contractIndex) {
            auto& stf = _shortTermFairsForContracts.getRef(contractIndex);
            if (stf._subscribed) {
                _shortTermFairUpdate.emplace(contractIndex, &stf);
            }
        }

        for (uint32_t contractIndex = _indexOfFirstTradedContract; contractIndex < _numberOfAllContracts; ++contractIndex) {
            RvContract &contract = _contracts[contractIndex];
            if (contract._traded) {
                contract._quotes[BID].resetBest();
                contract._quotes[ASK].resetBest();
            }

            auto& stf = _shortTermFairsForContracts.getRef(contractIndex);
            if (stf._subscribed) {
                _shortTermFairUpdate.emplace(contractIndex, &stf);
            }
        }
        sendSTFUpdates(true);
        _orderRequestMgr.sendCancelRequest(StrategyId::RelativeValue);
    }

    void handleLogin() {}
    void handleLogout() {}

    void handleCheckQuotes() {
        LOG(0, "RV check quotes {i1}", 0);
        doInitialCalculations();
    }

    bool handleEnabled() {
        if (canEnable()) {
            doInitialCalculations();
            return true;
        }
        return false;
    }

    bool canEnable() {
        bool canEnable = true;
        if (!hasMarketDataAndWithinMaxTicksWide()) {
            canEnable = false;
        }
        if (!isPnlWithinLimits()) {
            canEnable = false;
        }
        if (!isWithinPositionLimits()) {
            canEnable = false;
        }
        if (!isWidthMultiplierWithinLimits(_widthMultiplier)) {
            sendErrorMessage(boost::str(boost::format("RV width multiplier not within limits. Value must be > 1 and < 10 Value=%1%") % _widthMultiplier), ErrorType::Critical);
            canEnable = false;
        }

        for(unsigned i = 0; i < _pctStfMoves._size; ++i) {
            if(!isPctStfMoveWithinLimits(_pctStfMoves.getValue(i))) {
                sendErrorMessage(boost::str(boost::format("%%stfMove not within limits.  Value must be within [0,2].  Contract=%1% Value=%2%") % getContract(i) % _pctStfMoves.getValue(i)), ErrorType::Critical);
                canEnable = false;
            }
        }

        return canEnable;
    }

    bool hasMarketDataAndWithinMaxTicksWide() {
        bool hasMarketData = true;
        for (uint32_t contractIndex = 0; contractIndex < _numberOfAllContracts; ++contractIndex) {
            RvContract& contract = _contracts[contractIndex];
            if (contract._usedInPricing) {
                auto& externalBidMarket = contract.externalMarket<BID>();
                auto& externalAskMarket = contract.externalMarket<ASK>();
                const bool missingBook = (externalBidMarket.getPrice() == hf::NULL_PRICE) | (externalAskMarket.getPrice() == hf::NULL_PRICE);
                if (missingBook) {
                    hasMarketData = false;
                    sendErrorMessage(boost::str(boost::format("RV - Does not have market data for. Contract=%1%") % contract._definition._name), ErrorType::Critical);
                    LOG(0, "RV - Does not have market data for {C[i1]}", getContract(contractIndex))
                    break;
                }

                if((externalBidMarket.getAdjustedSize() < 0) || (externalBidMarket.getUnAdjustedSize() < 0)
                   || (externalAskMarket.getAdjustedSize() < 0) || (externalAskMarket.getUnAdjustedSize() < 0)) {
                    hasMarketData = false;
                    std::string errorMessageForGUI = boost::str(boost::format("Contract %1% has bad size.  BidUA=%2% AskUA=%3% BidAdj=%4% AskAdj=%5%")
                            % contract._definition._name % externalBidMarket.getUnAdjustedSize() % externalAskMarket.getUnAdjustedSize()
                            % externalBidMarket.getAdjustedSize() % externalAskMarket.getAdjustedSize());
                    sendErrorMessage(errorMessageForGUI, ErrorType::Critical);
                    LOG(0, "RV - Invalid quantities for {C[i1]} - bUA={i2} aUA={i3} bA={i4} aA={i5}", getContract(contractIndex), externalBidMarket.getUnAdjustedSize(),
                        externalAskMarket.getUnAdjustedSize(), externalBidMarket.getAdjustedSize(), externalAskMarket.getAdjustedSize());
                    logging::GetLogger().logError(errorMessageForGUI);
                    break;
                }
            }

            if (BOOST_UNLIKELY(!isWithinMaxTicksWide(contract))) {
                sendErrorMessage(boost::str(boost::format("RV - Breach MaxTicksWide. Contract=%1%") % contract._definition._name), ErrorType::Critical);
                hasMarketData = false;
                break;
            }
        }
        return hasMarketData;
    }

    bool isWithinMaxTicksWide(const RvContract& contract) const noexcept {
        if (_maxTicksWide.getValue(contract._internalId) != std::numeric_limits<int64_t>::max() &&
            (contract.externalMarket<ASK>().getPrice() == hf::NULL_PRICE ||
            contract.externalMarket<BID>().getPrice() == hf::NULL_PRICE)) {
            return false;
        }
        return (contract.externalMarket<ASK>().getPrice() - contract.externalMarket<BID>().getPrice()) <= _maxTicksWide.getValue(contract._internalId);
    }

    bool isWithinPositionLimits() {
        bool withinLimits = true;
        for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
            const double vectorPosition = _positionsInVector.getValue(vectorIndex);
            const double limit = _vectorsMaxPosition.getValue(vectorIndex);
            if (std::abs(vectorPosition) > limit) {
                sendErrorMessage(boost::str(boost::format(
                        "RV - not within vector position limits. Vector=%1%, Position=%2%, Limit=%3%")
                        % _vectorNames[vectorIndex] % vectorPosition % limit), ErrorType::Critical);
                LOG(0, "Vector position not within limits. Vector={V[i1]} Position={d1} Limit={d2}", getVectorName(vectorIndex), vectorPosition, limit)
                withinLimits = false;
            }
        }
        return withinLimits;
    }

    bool doInitialCalculations() {
        logVectorPositions(false);
        logContractPositions(false);
        calculateAllMidPoints();
        calculateOverdeterminedPrices();
        calculateVectorMarketPrices();
        calculateLongTermFairsInVector();
        calculateShortTermFairs();
        validateRhoneRiskLimitsExist();
        return calculateQuotes();
    }

    void calculateAllMidPoints() {
        for (uint32_t i = 0; i < _numberOfAllContracts; ++i) {
            double weight = _weights.getValue(i);
            if (!IsZero(weight)) {
                RvContract& contract = _contracts[i];
                Market& bid = contract.externalMarket<BID>();
                Market& ask = contract.externalMarket<ASK>();
                if (bid.getPrice() != hf::NULL_PRICE && ask.getPrice() != hf::NULL_PRICE) {
                    updateMidPoint(bid, ask, i, contract._establishedLevel);
                } else {
                    LOG(0, "RV {C[i1]}. Missing market on initial calculation of mid points.", getContract(contract._internalId))
                }
            }
        }
    }

    void handleInternalHedgeDone(const InternalHedgeDone& message) {
        DLOG(0, "RV {C[i1]}. InternalHedgeDone. Side={i2} HedgeId={i3} Market={i4}",
             getContract(message._instrumentIndex), message._side, message._hedgeId, message._marketIndex)
        RvContract& contract = _contracts[message._instrumentIndex];
        Market& market = contract._markets[message._side][Market::Internal];
        market.handleInternalHedgeDone(message._marketIndex, message._hedgeId);
    }

    void handleHedgeMarketDataAdjustmentAck(const HedgeMarketDataAdjustmentAck& hedgeAck) {
        RvContract& contract = _contracts[hedgeAck._instrumentIndex];
        LOG(0, "RV {C[i1]}. Received hedge md ack. HedgeId={i2} Adj={i3} OrderId={i4}", getContract(contract._internalId), hedgeAck._hedgeId, hedgeAck._size, (uint32_t) hedgeAck._orderId)
        Market& market = contract._markets[hedgeAck._side][Market::External];
        market.ackFAKAdjustment(hedgeAck._hedgeId, hedgeAck._size, hedgeAck._orderId);
    }

    void handleFill(const FillMessage& fill, bool checkLimit = true) {
        RvContract& contract = _contracts[fill._contractId];
        if (fill._source == FillSource::AsToRv) {
            const_cast<FillMessage&>(fill)._rvStf = _shortTermFairsForContracts.getRef(fill._contractId)._fair;
            sendFillToFront(fill);
            LOG(0, "RV {C[i1]}. got AS filled. Side={i2} Price={d1} Qty={i3}", getContract(contract._internalId), fill._side, ToDollars(fill._price), fill._qty)
            addPosition(contract, (fill._side == BID ? fill._qty : -fill._qty), checkLimit, false);
            return;
        }

        if (isEnabled()) {
            const hf::qty_t fillSize = fill._side == BID ? fill._qty : -fill._qty;
            const_cast<FillMessage&>(fill)._rvStf = _shortTermFairsForContracts.getRef(fill._contractId)._fair;
            sendFillToFront(fill);
            LOG(0, "RV {C[i1]}. got filled. Side={i2} Price={d1} Qty={i3}", getContract(contract._internalId), fill._side, ToDollars(fill._price), fill._qty)
            auto hedgeIt = _hedgeRequests.find(fill._contractId);
            if (hedgeIt != _hedgeRequests.end()) {
                hedgeIt->second += fillSize;
            } else {
                _hedgeRequests.insert({fill._contractId, fillSize});
            }
            if(!_latencyTracking.isset()) {
                _latencyTracking = fill._latencyTracking;
            }
        } else {
            LOG(0, "RV {C[i1]}. got filled. But disabled. Sending fill to MarketMaker. Side={i2} Price={d1} Qty={i3}", getContract(fill._contractId), fill._side, ToDollars(fill._price), fill._qty);
            sendFillToMarketMaker(fill);
        }
    }

    void handleEndOfInternalFills(int64_t tranNumber) {
        LOG(0, "RV End of internal fills transaction {i1}", tranNumber);
        _withinInternalFillsOfCycle = false;
    }

    void handleSetRhoneLimits(const SetRhoneLimits& limits) {
        LOG(0, "RV Handling SetRhoneLimits request.  numLimits={i1}", limits._numLimits);
        for(unsigned i = 0; i < limits._numLimits; ++i) {
            const RhoneLimit& limit = limits._limits[i];
            int previousLimit;
            if((previousLimit = _rhoneMaxClipSize.getValue(limit._internalId)) != 0) {
                if(previousLimit != limit._max_order_qty) {
                    LOG(0, "RV Set rhone limit for Contract={C[i1]} receiving updated limit.  This should not be possible!  old={i2} new={i3}", getContract(limit._internalId), previousLimit, limit._max_order_qty);
                    sendErrorMessage(boost::str(boost::format("RV Set rhone limit for Contract=%1% receiving updated limit.  This should not be possible!  old=%2% new=%3%") % _viperConfig._allContracts[limit._internalId]._name % previousLimit % limit._max_order_qty), ErrorType::Critical);
                }
            }
            _rhoneMaxClipSize.setValue(limit._internalId, limit._max_order_qty);
            LOG(0, "RV Set rhone limit for Contract={C[i1]} to {i2}", getContract(limit._internalId), _rhoneMaxClipSize.getValue(limit._internalId));
        }
    }

    void sendFillToMarketMaker(const FillMessage& fill) {
        getStrategy(StrategyId::MarketMaker)->sendMessage<FillMessage>(
                ViperMessageType::Fill,
                [&](FillMessage& message) {
                    memcpy(&message, &fill, sizeof(FillMessage));
                    message._source = FillSource::RvDisable;
                    message._forceOverFill = true;
                });
    }

    void sendFillToFront(const FillMessage& fill) {
        sendMessageToFront<FillMessage>(
                ViperMessageType::Fill,
                [&](FillMessage& message) {
                    memcpy(&message, &fill, sizeof(FillMessage));
                });
    }

    bool processGridChange(GridUpdate& update) {
        return _updateSelector.processUpdate(update);
    }

    bool validateRvVectorParametersUpdate(GridUpdate& update) {
        return true;
    }

    bool isChangeWithinStfLimits(double originalRange, double range, double maxPosition, double ltf, uint32_t vectorIndex) {
        if (isEnabled()) {
            const bool setToMarket = _setToMarket.getValue(vectorIndex);
            if (!setToMarket) {
                double fareMove = range / maxPosition;
                double currentStf = _shortTermFairsInVector.getValue(vectorIndex);
                double newStf = ltf - _positionsInVector.getValue(vectorIndex) * fareMove;
                double change = std::abs(newStf - currentStf);
                double limit = std::abs(0.05 * originalRange);
                if (change > limit) {
                    return false;
                }
            }
        }
        return true;
    }

    bool onRvVectorParametersUpdate(GridUpdate& update) {
        bool valid = validateRvVectorParametersUpdate(update);

        if (!valid) {
            return false;
        }

        if (update._update.updatetype() == proto::GridUpdateType::GridButtonType) {
            if (update._update.gridbutton() == STF_TO_MARKET) {
                if (!isEnabled()) {
                    setShortTermFairToMarket();
                    calculateQuotes();
                } else {
                    sendErrorMessage("Can set STF to market only when RV not enabled", ErrorType::Critical);
                }
                return true;
            }
        }

        if (update._update.updatetype() == proto::GridUpdateType::CellButtonType) {
            auto cellButton = update._update.cellbutton();
            auto vectorId = cellButton.row();
            switch(cellButton.col()) {
                case toUInt(VectorParametersColumns::STF_To_Market):
                    if (!isEnabled()) {
                        setShortTermFairToMarket(vectorId);
                        calculateQuotes();
                    } else {
                        sendErrorMessage("Can set STF to market only when RV not enabled", ErrorType::Critical);
                    }
                    break;
            }
            return true;
        }

        bool foundation = false;
        bool longTermFairs = false;
        bool shortTermFairs = false;
        std::vector<uint32_t> checkEMA;

        std::vector<std::tuple<VectorParametersColumns, uint32_t, double>> changedRangeMaxPos;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            auto columnType = static_cast<VectorParametersColumns>(gridCell.col());
            uint32_t vectorId = gridCell.row();
            switch (columnType) {
                case VectorParametersColumns::Range: {
                    auto value = boost::lexical_cast<double>(gridCell.value());
                    if (isEnabled()) {
                        auto range = _vectorsRange.getValue(vectorId);
                        if (isgreater(abs((value / range) - 1), 0.1)) {
                            LOG(0, "RV could not change Range more than 10 percent while RV enabled. Vector={V[i1]} Value={d1} OrgValue={d2}", getVectorName(vectorId), value, range)
                            sendErrorMessage(boost::str(boost::format("RV could not change Range more than 10 percent while RV enabled. Range=%1% Factor=%2%") % value % _vectorNames[vectorId]), ErrorType::Critical);
                            gridCell.set_value(std::to_string(range));
                            break;
                        }
                    }
                    if (isChangeWithinStfLimits(_vectorsRange.getValue(vectorId), value, _vectorsMaxPosition.getValue(vectorId), _longTermFairsInVector.getValue(vectorId), vectorId)) {
                        _vectorsRange.setValue(vectorId, value);
                        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
                        foundation = true;
                        shortTermFairs = true;
                        LOG(0, "RV range changed. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                        changedRangeMaxPos.emplace_back(std::make_tuple(VectorParametersColumns::Range, vectorId, value));
                    } else {
                        LOG(0, "RV could not change range. Change would be over 5 percent of range. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                        sendErrorMessage(boost::str(boost::format("RV could not change range. Change would be over 5 percent of range. Range=%1% Factor=%2%") % value % _vectorNames[vectorId]), ErrorType::Critical);
                        gridCell.set_value(std::to_string(_vectorsRange.getValue(vectorId)));
                    }
                    break;
                }
                case VectorParametersColumns::MaxPos: {
                    auto value = boost::lexical_cast<double>(gridCell.value());
                    if (isEnabled()) {
                        auto maxPos = _vectorsMaxPosition.getValue(vectorId);
                        if (isgreater(abs((value / maxPos) - 1), 0.1)) {
                            LOG(0, "RV could not change MaxPos more than 10 percent while RV enabled. Vector={V[i1]} Value={d1} OrgValue={d2}", getVectorName(vectorId), value, maxPos)
                            sendErrorMessage(boost::str(boost::format("RV could not change MaxPos more than 10 percent while RV enabled. MaxPos=%1% Factor=%2%") % value % _vectorNames[vectorId]), ErrorType::Critical);
                            gridCell.set_value(std::to_string(static_cast<int32_t>(maxPos)));
                            break;
                        }
                    }
                    if (isChangeWithinStfLimits(_vectorsRange.getValue(vectorId), _vectorsRange.getValue(vectorId), value, _longTermFairsInVector.getValue(vectorId), vectorId)) {
                        _vectorsMaxPosition.setValue(vectorId, value);
                        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
                        foundation = true;
                        shortTermFairs = true;
                        LOG(0, "RV max_pos changed. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                        changedRangeMaxPos.emplace_back(std::make_tuple(VectorParametersColumns::MaxPos, vectorId, value));
                    } else {
                        LOG(0, "RV could not change max_pos. Change would be over 5 percent of range. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                        sendErrorMessage(boost::str(boost::format("RV could not change max pos. Change would be over 5 percent of range. MaxPos=%1% Factor=%2%") % value % _vectorNames[vectorId]), ErrorType::Critical);
                        gridCell.set_value(std::to_string(static_cast<int32_t>(_vectorsMaxPosition.getValue(vectorId))));
                    }
                    break;
                }
                case VectorParametersColumns::EMA_ON: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _useEMAInVector.setValue(vectorId, value);

                    checkEMA.push_back(vectorId);
                    LOG(0, "RV ema_on changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), value)
                    if (!value) {
                        sendGenericMessageToFront(ViperMessageType::StoreLtf, vectorId);
                    }
                    break;
                }
                case VectorParametersColumns::EMA_Time: {
                    uint32_t value = boost::lexical_cast<uint32_t>(gridCell.value());
                    uint64_t periodInCycles = GetCyclesFromSeconds(value, _frequency);
                    _emaPeriodInVector.setValue(vectorId, periodInCycles);
                    longTermFairs = true;
                    shortTermFairs = true;
                    LOG(0, "RV ema_time changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), value)
                    break;
                }
                case VectorParametersColumns::EMA_On_Time: {
                    uint32_t time = boost::lexical_cast<uint32_t>(gridCell.value());
                    LOG(0, "RV ema_on_time changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), time);
                    if (time == (uint32_t)-1) {
                        if (_emaOnTime.getValue(vectorId) != (uint64_t)-1) {
                            _emaOnTime.setValue(vectorId, -1);
                            checkEMA.push_back(vectorId);
                        }
                    } else {
                        auto onTime = convertTimeToCycles(time / 100, time % 100, 0, clockUtil::GetCycles(), _frequency);
                        if (onTime != _emaOnTime.getValue(vectorId)) {
                            _emaOnTime.setValue(vectorId, onTime);
                            checkEMA.push_back(vectorId);
                        }
                    }
                    break;
                }
                case VectorParametersColumns::EMA_Off_Time: {
                    uint32_t time = boost::lexical_cast<uint32_t>(gridCell.value());
                    LOG(0, "RV ema_off_time changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), time)
                    if (time == (uint32_t)-1) {
                        if (_emaOffTime.getValue(vectorId) != (uint64_t)-1) {
                            _emaOffTime.setValue(vectorId, -1);
                            checkEMA.push_back(vectorId);
                        }
                    } else {
                        auto offTime = convertTimeToCycles(time / 100, time % 100, 0, clockUtil::GetCycles(), _frequency);
                        if (offTime != _emaOffTime.getValue(vectorId)) {
                            _emaOffTime.setValue(vectorId, offTime);
                            checkEMA.push_back(vectorId);
                        }
                    }
                    break;
                }
                case VectorParametersColumns::LTF: {
                    auto value = boost::lexical_cast<double>(gridCell.value());
                    switch (updateLTF(vectorId, value)) {
                        case LTFUpdateResult::Success:
                            shortTermFairs = true;
                            break;
                        case LTFUpdateResult::ViolateSTFLimits:
                            LOG(0, "RV could not change ltf. Change would be over 5% of range. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                            sendErrorMessage(boost::str(boost::format("RV could not change LTF. Change would be over 5 percent of range. LTF=%1% Factor=%2%") % value % _vectorNames[vectorId]), ErrorType::Critical);
                            gridCell.set_value(std::to_string(_longTermFairsInVector.getValue(vectorId)));
                            break;
                        case LTFUpdateResult::CannotUpdate:
                            LOG(0, "RV could not change ltf. EMA on. Vector={V[i1]}", getVectorName(vectorId))
                            sendErrorMessage(boost::str(boost::format("RV could not change LTF. EMA on. Factor=%1%") % _vectorNames[vectorId]), ErrorType::Critical);
                            gridCell.set_value(std::to_string(_longTermFairsInVector.getValue(vectorId)));
                            break;
                    }
                    break;
                }
                case VectorParametersColumns::SetToMarket: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _setToMarket.setValue(vectorId, value);
                    shortTermFairs = true;
                    LOG(0, "RV set_to_market changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), value)
                    break;
                }
                case VectorParametersColumns::Hedged: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _factorHedged.setValue(vectorId, value);
                    LOG(0, "RV hedged changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), value)
                    break;
                }
                case VectorParametersColumns::LTF_Delta: {
                    auto value = boost::lexical_cast<double>(gridCell.value());
                    auto range = _vectorsRange.getValue(vectorId);
                    if (isgreater(value, range * 0.05)) {
                        LOG(0, "RV LTFDelta cannot greater than 5% of factor's range. Vector={V[i1]} LTFDelta={d1}", getVectorName(vectorId), value)
                        sendErrorMessage(boost::str(boost::format("RV LTFDelta cannot greater than 5 percent of factor's range. Factor=%1% LTFDelta=%2%") % _vectorNames[vectorId] % value), ErrorType::Critical);
                        gridCell.set_value(std::to_string(_ltfDelta.getValue(vectorId)));
                    } else {
                        _ltfDelta.setValue(vectorId, value);
                        LOG(0, "RV LTFDelta changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), value)
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (!changedRangeMaxPos.empty()) {
            getStrategy(StrategyId::ArbieSpreader)->sendMessage<VectorParameterUpdate>(
                    ViperMessageType::VectorParameterUpdate,
                    [&changedRangeMaxPos] (VectorParameterUpdate& msg) {
                        msg._numUpdate = changedRangeMaxPos.size();
                        VectorParameterUpdate::Update* data = msg._data;
                        for (auto& change : changedRangeMaxPos) {
                            data->_type = std::get<0>(change);
                            data->_vectorIndex = std::get<1>(change);
                            data->_value = std::get<2>(change);
                            ++data;
                        }
                    },
                    changedRangeMaxPos.size() * sizeof(VectorParameterUpdate::Update));
        }

        if (foundation) {
            calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
            sendQuoteSizesToGUI();
        }

        bool checkFairs = false;
        if (!checkEMA.empty()) {
            auto now = clockUtil::GetCycles();
            for (const auto& vectorId : checkEMA) {
                checkFairs |= checkEMAOnOffTime(now, vectorId);
            }
        }

        if (checkFairs | longTermFairs) {
            calculateLongTermFairsInVector();
        }

        if (checkFairs | shortTermFairs) {
            calculateShortTermFairs();
        }

        calculateQuotes();

        return true;
    }

    enum class LTFUpdateResult {
        ViolateSTFLimits,
        Success,
        CannotUpdate,
    };

    LTFUpdateResult updateLTF(uint32_t vectorId, double value, bool checkLimits = true) {
        if (_useEMAInVector.getValue(vectorId)) {
            return LTFUpdateResult::CannotUpdate;
        }

        if (checkLimits) {
            if (isChangeWithinStfLimits(_vectorsRange.getValue(vectorId), _vectorsRange.getValue(vectorId), _vectorsMaxPosition.getValue(vectorId), value, vectorId)) {
                _longTermFairsInVector.setValue(vectorId, value);
                LOG(0, "RV ltf changed. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                return LTFUpdateResult::Success;
            }
            return LTFUpdateResult::ViolateSTFLimits;
        }
        _longTermFairsInVector.setValue(vectorId, value);
        LOG(0, "RV ltf changed. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
        return LTFUpdateResult::Success;
    }

    uint64_t changeTimeToNextDay(uint64_t time) const noexcept {
        return time + GetCyclesFromSeconds(86400, _frequency);
    }

    bool onRvContractParametersUpdate(GridUpdate& update) {
        bool valid = validateRvContractParametersUpdate(update);

        if (!valid) {
            return false;
        }

        bool foundation = false;
        bool widthChanged = false;
        bool rvQuoteMaxPercentWidthChange = false;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(gridCell.col());
            RvContract& contract = _contracts[gridCell.row()];
            switch (columnType) {
                case ContractParameterRVColumns::QuoteExternal: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _contractQuoteExternal.setValue(gridCell.row(), value);
                    LOG(0, "RV {C[i1]}. quote external changed. Value={i2}", getContract(contract._internalId),value)
                    break;
                }
                case ContractParameterRVColumns::Commission: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _commissionsInput.setValue(gridCell.row(), value);
                    foundation = true;
                    LOG(0, "RV {C[i1]}. commission changed. Value={d1}", getContract(contract._internalId), value)
                    break;
                }
                case ContractParameterRVColumns::Width: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _contractWidths.setValue(gridCell.row(), ToRhonePrice(value));
                    calculateOneExtraContractWidth(gridCell.row());
                    foundation = true;
                    widthChanged = true;
                    LOG(0, "RV {C[i1]}. Width changed. Value={d1}", getContract(contract._internalId), value)
                    break;
                }
                case ContractParameterRVColumns::PCTStfMove : {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _pctStfMoves.setValue(gridCell.row(), value);
                    foundation = true;
                    rvQuoteMaxPercentWidthChange = true;
                    LOG(0, "RV {C[i1]}. %%stfMove changed. Value={d1}", getContract(contract._internalId), value)
                    break;
                }
                case ContractParameterRVColumns::MaxTicksThroughMarket : {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _maxTicksThroughMarket.setValue(gridCell.row(), contract._definition._tickSize * value);
                    LOG(0, "RV {C[i1]}. MaxTicksThroughMarket changes. Value={d1}", getContract(contract._internalId), value)
                    break;
                }
                case ContractParameterRVColumns::StaticQuote: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _staticQuote.setValue(gridCell.row(), value);
                    LOG(0, "RV {C[i1]}. static quote changed. Value={i2}", getContract(contract._internalId),value)
                    break;
                }
                case ContractParameterRVColumns::QuoteSize: {
                    uint32_t value = boost::lexical_cast<uint32_t>(gridCell.value());
                    _staticQuoteSize.setValue(gridCell.row(), value);
                    LOG(0, "RV {C[i1]}. quote size changed. Value={i2}", getContract(contract._internalId),value)
                    break;
                }
                default:
                    break;
            }
        }

        if (foundation) {
            calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
        }

        calculateQuotes();

        if (widthChanged || rvQuoteMaxPercentWidthChange) {
            sendQuoteSizesToGUI();
        }

        return true;
    }

    bool validateRvContractParametersUpdate(GridUpdate& update) {

        bool isValid = true;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell* gridCell = update._update.mutable_cells(i);
            ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(gridCell->col());
            RvContract& contract = _contracts[gridCell->row()];

            if(columnType == ContractParameterRVColumns::PCTStfMove) {
                bool errorOccurredOnThisIter = false;
                try {
                    double value = boost::lexical_cast<double>(gridCell->value());
                    if(!isPctStfMoveWithinLimits(value)) {
                        sendErrorMessage(boost::str(boost::format("%%stfMove not within limits.  Value must be within [0,2].  Contract=%1% Value=%2%") % contract._definition._name % value), ErrorType::Critical);
                        isValid = false;
                        errorOccurredOnThisIter = true;
                    }
                }
                catch(boost::bad_lexical_cast& blc) {
                    sendErrorMessage(boost::str(boost::format("%%stfMove not parsable.  Contract=%1% Value=%2%") % contract._definition._name % gridCell->value()), ErrorType::Critical);
                    isValid = false;
                    errorOccurredOnThisIter = true;
                }

                if(errorOccurredOnThisIter) {
                    gridCell->set_value(std::to_string(_pctStfMoves.getValue(gridCell->row())));
                }
            }

            if(columnType == ContractParameterRVColumns::MaxTicksThroughMarket) {
                bool errorOccurredOnThisIter = false;
                try {
                    double value = boost::lexical_cast<double>(gridCell->value());
                    if(value < 0) {
                        sendErrorMessage(boost::str(boost::format("MaxTicksThroughMarket must be >= 0.  Contract=%1% Value=%2%") % contract._definition._name % gridCell->value()), ErrorType::Critical);
                        isValid = false;
                        errorOccurredOnThisIter = true;
                    }
                }
                catch(boost::bad_lexical_cast& blc) {
                    sendErrorMessage(boost::str(boost::format("maxTicksThroughmarket not parsable (too big?).  Contract=%1% Value=%2%") % contract._definition._name % gridCell->value()), ErrorType::Critical);
                    isValid = false;
                    errorOccurredOnThisIter = true;
                }

                if(errorOccurredOnThisIter) {
                    gridCell->set_value(std::to_string(((double)_maxTicksThroughMarket.getValue(gridCell->row())) / contract._definition._tickSize));
                }
            }
        }

        return isValid;
    }

    bool onRvLeanMatrixUpdate(GridUpdate& update) {
        bool valid = validateRvLeanMatrixUpdate(update);

        if (!valid) {
            return false;
        }

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
            RvContract& contractOne = _contracts[gridCell.row()];
            RvContract& contractTwo = _contracts[gridCell.col()];
            _leanMatrix.setValue(gridCell.row(), gridCell.col(), value);
            LOG(0, "RV lean_matrix changed. Contract={C[i1]} Contract={C[i2]} Value={i3}", getContract(contractOne._internalId), getContract(contractTwo._internalId), value)
        }

        calculateQuotes();

        return true;
    }

    void validateRhoneRiskLimitsExist() {
        for(const auto& contract : _contracts) {
            if(contract._traded) {
                if(!_rhoneMaxClipSize.getValue(contract._internalId)) {
                    sendErrorMessage("RV No Rhone Limit for Contract= " + _viperConfig._allContracts[contract._internalId]._name + " RV will disable when attempting to send hedge for contract.", ErrorType::Critical);
                }
            }
        }
    }

    bool validateRvLeanMatrixUpdate(GridUpdate& update) {
        return true;
    }

    bool onRvPricingConfigUpdate(GridUpdate& update) {
        bool valid = validateRvPricingConfigUpdate(update);

        if (!valid) {
            return false;
        }

        bool needToCalculateOverDeterminedMatrix = false;
        std::set<int> needToUpdateMidPoint;
        std::unordered_map<int, double> originalWeights;
        bool sendUpdate = false;
        bool qtyMultiplierChanged = false;
        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            PricingConfigsColumns columnType = static_cast<PricingConfigsColumns>(gridCell.col());
            RvContract& contract = _contracts[gridCell.row()];
            switch (columnType) {
                case PricingConfigsColumns::UseWeighted: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _useWeightedMid.setValue(gridCell.row(), value);
                    needToUpdateMidPoint.emplace(gridCell.row());
                    LOG(0, "RV use_weighted changed. Contract={C[i1]} Value={i2}", getContract(contract._internalId), value)
                    break;
                }
                case PricingConfigsColumns::Weight: {
                    if (isEnabled()) {
                        gridCell.set_value(std::to_string(_weights.getValue(gridCell.row())));
                        LOG(0, "RV {C[i1]}. Weight change not allowed while RV enabled.", getContract(contract._internalId))
                        sendErrorMessage(boost::str(boost::format("Weight change is not allowed while RV enabled. Contract=%1%") %  contract._definition._name), ErrorType::Critical);
                        break;
                    }
                    const double weight = boost::lexical_cast<double>(gridCell.value());
                    LOG(0, "RV {C[i1]}. Weight changed. Value={d1}", getContract(contract._internalId), weight)
                    originalWeights.emplace(i, _weights.getValue(gridCell.row()));
                    _weights.setValue(gridCell.row(), weight);
                    auto oldUsedInPricing = contract._usedInPricing;
                    contract._usedInPricing = !IsZero(weight);
                    if (contract._usedInPricing || oldUsedInPricing != contract._usedInPricing) {
                        needToCalculateOverDeterminedMatrix = true;
                        needToUpdateMidPoint.emplace(gridCell.row());
                    }
                    break;
                }
                case PricingConfigsColumns::EstablishedLevel: {
                    hf::qty_t value = boost::lexical_cast<hf::qty_t>(gridCell.value());
                    contract._establishedLevel = value;
                    needToUpdateMidPoint.emplace(gridCell.row());
                    LOG(0, "RV established_level changed. Contract={C[i1]} Value={i2}", getContract(contract._internalId), value)
                    break;
                }
                case PricingConfigsColumns::MaxTicksWide: {
                    int32_t value = boost::lexical_cast<int32_t>(gridCell.value());
                    LOG(0, "RV {C[i1]}. MaxTicksWidth changes. Value={i2}", getContract(contract._internalId), value)
                    if (value < 0) {
                        _maxTicksWide.setValue(gridCell.row(), std::numeric_limits<int64_t>::max());
                        LOG(0, "RV {C[i1]}. Ignore MaxTicksWidth checking", getContract(contract._internalId))
                    } else {
                        _maxTicksWide.setValue(gridCell.row(), value * contract._definition._tickSize);
                    }

                    if (BOOST_UNLIKELY(!isWithinMaxTicksWide(contract))) {
                        LOG(0, "RV {C[i1]}. MaxTicksWide breached. Disable RV.", getContract(contract._internalId))
                        disableRelativeValue(boost::str(boost::format("MaxTicksWide breached. Disable RV. Contract=%1%") % contract._definition._name));
                    }
                    break;
                }
                case PricingConfigsColumns::QuantityMultiplier: {
#ifndef IS_DEV
                    if (contract._definition._venue != hf::VENUE::B3) {
                        gridCell.set_value(std::to_string(contract._quantityMultiplier));
                        sendErrorMessage(boost::str(boost::format("QuantityMultiplier is read-only for %1%") % contract._definition._name), ErrorType::Normal);
                        break;
                    }
#endif
                    if (isEnabled()) {
                        gridCell.set_value(std::to_string(contract._quantityMultiplier));
                        sendErrorMessage(boost::str(boost::format("QuantityMultiplier can't be changed while RV is enabled. Contract=%1%") % contract._definition._name), ErrorType::Normal);
                        break;
                    }

                    contract._quantityMultiplier = boost::lexical_cast<double>(gridCell.value());
                    _viperConfig._pricingConfigs[contract._internalId]._quantityMultiplier = contract._quantityMultiplier;
                    LOG(0, "RV {C[i1]}. QuantityMultiplier changed. Value={i2}", getContract(contract._internalId), contract._quantityMultiplier)
                    handleQtyMultiplierUpdate(contract);
                    qtyMultiplierChanged = true;
                    sendUpdate = true;
                    break;
                }
                case PricingConfigsColumns::SweepTicks:
                case PricingConfigsColumns::SweepQty:
                case PricingConfigsColumns::PricingMultiplier:  // read only
                case PricingConfigsColumns::ASExtCommission:
                    break;
            }
        }

        if (qtyMultiplierChanged) {
            calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
            updatePositionInVector();
        }

        if (needToCalculateOverDeterminedMatrix) {
            try {
                calculateOverdeterminedMatrix();
            } catch (const std::runtime_error& e) {
                LOG(0, "RV PrinceParameter weight change caused exception in matrix calculation. {i1}", 0)
                std::string msg = "Weight change in PricingParameter caused exception in matrix calculation.";
                if (!isEnabled()) {
                    sendErrorMessage(msg, ErrorType::Critical);
                } else {
                    disableRelativeValue(msg);
                }

                // revert weights back
                for (auto& kvp : originalWeights) {
                    proto::GridCell& gridCell = *update._update.mutable_cells(kvp.first);
                    gridCell.set_value(std::to_string(kvp.second));
                    _weights.setValue(gridCell.row(), kvp.second);
                    RvContract& contract = _contracts[gridCell.row()];
                    contract._usedInPricing = !IsZero(kvp.second);
                }
            }
        }

        for (int rowIndex : needToUpdateMidPoint) {
            RvContract& contract = _contracts[rowIndex];
            updateMidPoint(contract.externalMarket<BID>(), contract.externalMarket<ASK>(), rowIndex, contract._establishedLevel);
        }

        calculateOverdeterminedPrices();
        calculateVectorMarketPrices();
        calculateLongTermFairsInVector();
        calculateShortTermFairs();
        calculateQuotes();

        if (sendUpdate) {
            sendQuoteSizesToGUI();
        }
        return true;
    }

    void handleQtyMultiplierUpdate(RvContract& contract) {
        for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfRows; ++vectorIndex) {
            if (contract._definition._legs.size() == 1) {
                assert(contract._internalId < _risk_X_qtyMultiplier_Matrix._numberOfColumns);
                _risk_X_qtyMultiplier_Matrix.setValue(vectorIndex, contract._internalId, _riskMatrix.getValue(vectorIndex, contract._internalId) * contract._quantityMultiplier);
            }
            _vectorRisk_Div_qtyMultiplier_Matrix.setValue(vectorIndex, contract._internalId, (_vectorRiskMatrix.getValue(vectorIndex, contract._internalId) / contract._quantityMultiplier));
        }

        for (auto& otherContract : _contracts) {
            _costMultiplierMatrix.setValue(contract._internalId, otherContract._internalId,
                                      contract._pricingMultiplier / otherContract._pricingMultiplier * contract._quantityMultiplier / otherContract._quantityMultiplier);
            _costMultiplierMatrix.setValue(otherContract._internalId, contract._internalId,
                                      otherContract._pricingMultiplier / contract._pricingMultiplier * otherContract._quantityMultiplier / contract._quantityMultiplier);
        }
    }

    void updatePositionInVector() {
        _positionsInVector.clear();

        auto& rvPositions = _viperConfig._positions[toUInt(StrategyId::RelativeValue)];
        for(unsigned i = 0; i < _viperConfig._numberOfPricingContracts; ++i) {
            const auto& contract = _viperConfig._pricingConfigs[i];
            auto& rvContract = *getContract(contract._contract);
            auto& pos = rvPositions[i];
            addPositionToVector(rvContract, pos, true, false);
        }
    }

    //Must be thread safe if verbose == false
    bool validateRvPricingConfigUpdate(GridUpdate& update, bool verbose = true) {
        return true;
    }

    COLD
    void onManualFill(ManualFill &fill) {
        RvContract& contract = _contracts[fill._contract];
        uint32_t side = fill._size > 0 ? BID : ASK;
        hf::price_t price;

        auto [bidPrice, bidSize, askPrice, askSize] = std::make_tuple(contract.externalMarket<BID>().getPrice(), contract.externalMarket<BID>().getUnAdjustedSize(),
                                                                      contract.externalMarket<ASK>().getPrice(), contract.externalMarket<ASK>().getUnAdjustedSize());

        if(contract._usedInPricing && bidPrice != hf::NULL_PRICE && bidSize > 0 && askPrice != hf::NULL_PRICE && askSize > 0) {
            price = ToRhonePrice(calculateMidpoint(contract.externalMarket<BID>(), contract.externalMarket<ASK>(), _useWeightedMid.getValue(fill._contract), contract._establishedLevel));
        }
        else {
            price = GetPriceForManualFill(side, bidPrice, bidSize, askPrice, askSize);
        }

        hf::price_t stf = hf::NULL_PRICE;
        if(contract._usedInPricing && canActOnMarketData()) {
            stf = _shortTermFairsForContracts.getRef(fill._contract)._fair;
        }

        addPosition(contract, fill._size, false, false);
        sendAcceptedManualFillToFront(contract._internalId, fill._size, price, stf);
    }

    COLD
    void onEndOfManualFills() {
        checkVectorPositionLimits(false);
        calculateShortTermFairs();
        calculateQuotes();
    }

    bool onMarketMakerParametersUpdate(GridUpdate& update) { return true; }
    bool onStackerParametersUpdate(GridUpdate& update) { return true; }
    bool onPnlParametersUpdate(GridUpdate& update) { return true; }
    bool onArbieSpreaderParametersUpdate(GridUpdate& update) { return true; }
    bool onArbieSpreaderPathsUpdate(GridUpdate& update) { return true; }

    bool onGlobalConfigsUpdate(GridUpdate& update) {
        bool quotes = false;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            auto& config = _viperConfig._globalConfigs[gridCell.row()];
            if (config._key == RV_WIDTH_MULTIPLIER) {
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                try {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    if (!isWidthMultiplierWithinLimits(value)) {
                        LOG(0, "RV could not change width multiplier. Value must be > 1 and < 10 Value={d1}", value)
                        sendErrorMessage(boost::str(boost::format(
                                "RV could not change width multiplier. Value must be > 1 and < 10 Value=%1%") %
                                                    value), ErrorType::Critical);
                        gridCell.set_value(config._shifts[toUInt(shift)]);
                    } else {
                        if (_viperConfig._currentShift == shift) {
                            _widthMultiplier = value;
                            calculateExtraContractWidths();
                            LOG(0, "RV width_multiplier changed. Value={d1}", _widthMultiplier)
                            quotes = true;
                        }
                    }
                }
                catch (const boost::bad_lexical_cast&) {
                    LOG(0, "Invalid RV width multiplier value {i1}", 0)
                    gridCell.set_value(config._shifts[toUInt(shift)]);
                    sendErrorMessage(boost::str(boost::format("Invalid RV width multiplier value %1%") % gridCell.value()), ErrorType::Critical);
                }
                break;
            }
        }

        if (quotes) {
            calculateQuotes();
            sendQuoteSizesToGUI();
        }

        return true;
    }

    bool isWidthMultiplierWithinLimits(double value) {
        return (Compare(value, 1.0) >= 0) & (Compare(value, 10.0) <= 0);
    }

    bool isPctStfMoveWithinLimits(double value) {
        return (value >= 0) && (value <= 2);
    }

    void calculateExtraContractWidths() {
         for (uint32_t i = 0; i < _contractWidths._size; ++i) {
             calculateOneExtraContractWidth(i);
         }
    }

    void calculateOneExtraContractWidth(uint32_t id) {
        hf::price_t width = static_cast<hf::price_t>(_contractWidths.getValue(id) * (_widthMultiplier - 1.0));
        _extraContractWidths.setValue(id, width);
    }

    void onPoolThresholdExceeded(const std::string& queueName, uint64_t usage) {
        logging::GetLogger().logError("RV Pool_threshold_exceeded");
    }

    void onQueueThresholdExceeded(const std::string& queueName, uint64_t usage) {
        logging::GetLogger().logError("RV Queue_threshold_exceeded");
    }

    void handleUncommonMDEvent(const md::MDEvent &mde, const md::MDBatch& batch, uint32_t internalId) {
        switch (mde.type) {
            case md::MDEvent::TYPE::FAK_DONE: {
                const auto &event = static_cast<const md::MDFakDonenEvent &> (mde);
                handleFakDone(ToViperSide(event._side), internalId, event._oid.value());
                break;
            }
            default:
                break;
        }
    }

    void onEndMDBatch(hf::batch_id_t bid) {
        for (uint32_t contractId : _mdUpdated) {
            auto& rvContract = _contracts[contractId];
            rvContract._lastMDbid = bid;
        }
    }

    void onMDTrade(const md::MDBatch& batch, const md::MDTradeEvent &event, uint32_t internalId) {
        RvContract& contract = _contracts[internalId];

        if (BOOST_UNLIKELY(contract._lastMDbid >= batch.bid)) {
            logging::GetLogger().logWarning(boost::str(boost::format("RV %1%. Skip stale MD. Trade. bid=%2% lastMDbid=%3%") % contract._definition._name % batch.bid % contract._lastMDbid));
            return;
        }

        if (batch.venue == hf::VENUE::CME) {
            for (uint32_t i = 0; i < event.num; ++i) {
                auto& trade = event.trades[i];
                if (trade.side == hf::SIDE::BUY) {
                    //LOG(0, "RV: Contract {C[i1]} Bid book adjustment due to trade. {i2}@{d1}", getContract(internalId), trade.q, ToDollars(trade.pr))
                    onMarketDataUpdate(contract, internalId, true, trade.pr, trade.q, false, 0, 0);
                } else if (trade.side == hf::SIDE::SELL) {
                    //LOG(0, "RV: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}", getContract(internalId), trade.q, ToDollars(trade.pr))
                    onMarketDataUpdate(contract, internalId, false, 0, 0, true, trade.pr, trade.q);
                }
            }
        }
    }

    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent& event, uint32_t internalId, hf::batch_id_t bid) {
        RvContract& contract = _contracts[internalId];
        switch (event.statusType) {
            case md::MDStatusEvent::TYPE::RECOVERING:
            case md::MDStatusEvent::TYPE::TIMEOUT:
            case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
                contract._canTradeStatus = false;
                if (contract._usedInPricing) {
                    LOG(0, "RV {C[i1]}. market data not working. Reason={i2}", getContract(contract._internalId), toUInt(event.statusType))
                    disableRelativeValue(boost::str(boost::format("Contract %1% market data not working. Reason=%2%.") % contract._definition._name % toUInt(event.statusType)));
                } else {
                    LOG(0, "RV {C[i1]}. market data not working. Reason={i2}.", getContract(contract._internalId), toUInt(event.statusType))
                }
                break;
            }
            case md::MDStatusEvent::TYPE::RECOVERED: {
                LOG(0, "RV {C[i1]}. market data recovered.", getContract(contract._internalId))
                contract._canTradeStatus = true;
                break;
            }
            case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                switch (venue) {
                    case hf::VENUE::B3:
                    case hf::VENUE::CME:
                        handle_CME_B3_MDStatus(event, contract);
                        break;
                    case hf::VENUE::ICE:
                        handleICEMDStatus(event, contract);
                        break;
                    case hf::VENUE::EUREX:
                        handleEUREXMDStatus(event, contract);
                        break;
                    default:
                        assert(false && "unhandled venue");
                        break;
                }
                break;
            }
            case md::MDStatusEvent::TYPE::UNKNOWN: {
                std::string message = boost::str(boost::format("Contract %1% market data - received unknown md status. RV") % contract._definition._name);
                sendErrorMessage( boost::str(boost::format("Contract %1% market data - received unknown md status. RV") % contract._definition._name), ErrorType::Critical);
                LOG(0, "Contract {C[i1]} market data - received unknown md status. RV", getContract(contract._internalId))
                break;
            }
            case md::MDStatusEvent::TYPE::UNRECOVERABLE:
                break;
        }
    }

    void handleICEMDStatus(const md::MDStatusEvent& event, RvContract& contract) {
        switch (event.extStatus) {
            case 'O': {
                contract._canTradeStatus = true;
                LOG(0, "RV {C[i1]}. market data - open.", getContract(contract._internalId))
                break;
            }
            case 'C': {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - closed. Used in pricing={i2}", getContract(contract._internalId), contract._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed. Used in pricing=%2%") % contract._definition._name % contract._usedInPricing));
                break;
            }
            case 'E': {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - expired. Used in pricing={i2}", getContract(contract._internalId), contract._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - expired. Used in pricing=%2%") % contract._definition._name % contract._usedInPricing));
                break;
            }
            case '1': {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - entered pre-open. Used in pricing={i2}", getContract(contract._internalId), contract._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - entered pre-open.  Used in pricing=%2%") % contract._definition._name % contract._usedInPricing));
                break;
            }
            default:
                break;
        }
    }

    void handle_CME_B3_MDStatus(const md::MDStatusEvent& event, RvContract& contract) {
        switch (event.extStatus) {
            case 17: {
                contract._canTradeStatus = true;
                LOG(0, "RV {C[i1]}. market data - opened.", getContract(contract._internalId))
                break;
            }
            case 2: {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - halted. Used in pricing={i2}", getContract(contract._internalId), contract._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - halted. Used in pricing=%2%.") % contract._definition._name % contract._usedInPricing));
                break;
            }
            case 101:   // B3 Final Closing Call
            case 18:
            case 4: {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]} market data - closed({i2}). Used in pricing={i3}", getContract(contract._internalId), (uint32_t)(event.extStatus), contract._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._usedInPricing));
                break;
            }
            case 21: {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]} market data - entered pre-open. Used in pricing={i2}", getContract(contract._internalId), contract._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - entered pre-open. Used in pricing=%2%") % contract._definition._name % contract._usedInPricing));
                break;
            }
            default: {
                break;
            }
        }
    }
    void onContractStatusChanged(RvContract& contract, const md::MDStatusEvent& event, const std::string& msg) {
        if (!contract._canTradeStatus) {
            if (contract._usedInPricing) {
                LOG(0, "RV {C[i1]}. market data not working. Reason={i2}", getContract(contract._internalId), toUInt(event.statusType))
                disableRelativeValue(boost::str(boost::format("Contract %1% market data not working. Reason=%2%.") % contract._definition._name % toUInt(event.statusType)));
            } else {
                LOG(0, "RV {C[i1]}. market data not working. Reason={i2}.", getContract(contract._internalId), toUInt(event.statusType))
            }
        }
    }

    void handleEUREXMDStatus(const md::MDStatusEvent& event, RvContract& contract) {
        switch(event.extStatus) {
            case 203: //Continuous
                contract._canTradeStatus = true;
                LOG(0, "RV Contract {C[i1]} market data - opened.", getContract(contract._internalId));
                break;
            case 200: //Closed
                contract._canTradeStatus = false;
                LOG(0, "RV Contract {C[i1]} market data - closed.", getContract(contract._internalId))
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._usedInPricing));
                break;
            case 2:   /* Halt */
            case 201: /* Restricted */
            case 216: /* Freeze */
            case 213: /* IPO Auction Freeze */
            case 214: /* Pre Call */
            case 215: /* Call */
                // These are states that as described we will handle as a freeze, but have never been seen in market data
                LOG(0, "RV Contract {C[i1]}. market data - seen rare MD state we're treating as a halt  extStatus={i2}", getContract(contract._internalId), event.extStatus);
                FALLTHROUGH;
            case 204: /* Opening Auction */
            case 206: /* Intraday Auction */
            case 208: /* Circuit Breaker Auction */
            case 210: /* Closing Auction */
            case 212: /* IPO Auction */
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - auction.  extStatus={i2}", getContract(contract._internalId), event.extStatus);
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - auction(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._usedInPricing));
                break;
            case 205: /* Opening Auction Freeze */
            case 207: /* Intraday Auction Freeze */
            case 209: /* Circuit Breaker Auction Freeze */
            case 211: /* Closing Auction Freeze */
                //These states are similar to CME no-cancel, except instead of a cancel reject, the exchange will "hold" the cancel until the market re-opens and
                //cancel the order if it's not matched.  Note that this state is not guaranteed to occur before the market re-opens!
                contract._canTradeStatus  = false;
                LOG(0, "RV {C[i1]}. market data - Auction Freeze.  extStatus={i2}", getContract(contract._internalId), event.extStatus);
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - auction freeze(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._usedInPricing));
                break;
            case 202: //Book
                LOG(0, "RV {C[i1]}. market data - Book.", getContract(contract._internalId));
                break; //Explicitly doing nothing
            default: //Should never get here
                LOG(0, "RV Contract {C[i1] market data - unknown state received.  state={i2}", getContract(contract._internalId), static_cast<int>(event.extStatus));
                sendErrorMessage(boost::str(boost::format("Contract %1% market data - unknown state received.  Tell a dev! extStatus=%2%") % getContractName(contract._internalId) %
                                            static_cast<int>(event.extStatus)), ErrorType::Critical);
                break;
        }
    }

    void handleFakDone(uint32_t side, uint32_t internalId, om::strategy_order_id_t::value_type oid) {
        LOG(0, "RV {C[i1]}. Received FAK done.", internalId)
        RvContract& contract = _contracts[internalId];
        Market& book = contract.externalMarket(side);
        book.handleFakDone(oid);
    }

    void onMDUpdate(const md::MDUpdateEvent &event, uint32_t internalId, hf::batch_id_t batchBid) {
        const md::top_t::level_t& bid = event.buy.top.l[0];
        const md::top_t::level_t& ask = event.sell.top.l[0];

        RvContract& contract = _contracts[internalId];

        if (BOOST_UNLIKELY(contract._lastMDbid >= batchBid)) {
            logging::GetLogger().logWarning(boost::str(boost::format("RV %1%. Skip stale MD. Update. bid=%2% lastMDbid=%3%") % contract._definition._name % batchBid % contract._lastMDbid));
            return;
        }

        const Market& rvBid = contract.externalMarket<BID>();
        const Market& rvAsk = contract.externalMarket<ASK>();

        const bool bidChanged = (event.buy.updatedQtyNum == 1) && ((rvBid.getPrice() != bid.pr) || (rvBid.getUnAdjustedSize() != bid.q));
        const bool askChanged = (event.sell.updatedQtyNum == 1) && ((rvAsk.getPrice() != ask.pr) || (rvAsk.getUnAdjustedSize() != ask.q));

        onMarketDataUpdate(contract, internalId, bidChanged, bid.pr, bid.q, askChanged, ask.pr, ask.q);
    }

    void onMarketDataUpdate(RvContract& contract, uint32_t internalId, bool bidChanged, hf::price_t bidPrice, hf::qty_t bidSize, bool askChanged,  hf::price_t askPrice, hf::qty_t askSize) {
        BOOST_ASSERT(internalId == contract._internalId);
        if (bidChanged) {
            Market& bid = contract.externalMarket<BID>();
            bid.update(bidPrice, bidSize);
        }
        if (askChanged) {
            Market& ask = contract.externalMarket<ASK>();
            ask.update(askPrice, askSize);
        }
        if (bidChanged | askChanged) {
            _hadExternalMarketData = true;

            _updatedContracts.emplace(internalId);
            if (contract._usedInPricing) {
                const bool missingBook = (contract.externalMarket<BID>().getPrice() == hf::NULL_PRICE) | (contract.externalMarket<ASK>().getPrice() == hf::NULL_PRICE);
                if (BOOST_UNLIKELY(missingBook)) {
                    LOG(0, "RV {C[i1]}. Lost book for contract.", getContract(contract._internalId))
                    disableRelativeValue(boost::str(boost::format("Do not have market data for pricing. Contract=%1%") % contract._definition._name));
                }
            }

            if (BOOST_UNLIKELY(!isWithinMaxTicksWide(contract))) {
                LOG(0, "RV {C[i1]}. MaxTicksWide breached. Disable RV.", getContract(contract._internalId))
                disableRelativeValue(boost::str(boost::format("MaxTicksWide breached. Disable RV. Contract=%1%") % contract._definition._name));
            }
        }
    }

    bool canActOnMarketData() const {
        return isEnabled() | (_state.load(std::memory_order_relaxed) == SystemState::CheckQuotes);
    }

    void recalculateAfterExternalMarketDataChange() {
        if (canActOnMarketData()) {
            SCOPED_PERF_POINT(PerfId::RV_CalcAfterMDChange);
            bool needToRecalculate = false;
            for (uint32_t id : _updatedContracts) {
                RvContract& contract = _contracts[id];
                if (contract._usedInPricing) {
                    needToRecalculate = true;
                    updateMidPoint(contract.externalMarket<BID>(), contract.externalMarket<ASK>(), id, contract._establishedLevel);
                }
            }
            if (needToRecalculate) {
                calculateOverdeterminedPrices();
            }
            calculateVectorMarketPrices();
            calculateLongTermFairsInVector();
            calculateShortTermFairs();
        }
        _updatedContracts.clear();
    }

    void disableRelativeValue(const std::string& reason, bool wasEnabled) {
        if (wasEnabled) {
            LOG(0, "Disabling Relative Value. {i1}", 0)
            _state.store(SystemState::Disabled, std::memory_order_release);
            handleDisabled();
            _oldState = SystemState::Disabled;  // prevent calling handleDisabled() again through checkSystemState;
            sendErrorMessage("Disabling Relative Value. Reason=" + reason, ErrorType::Critical);
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled),
                                      proto::SystemState::SystemStateDisabled, toUInt(StrategyId::RelativeValue));
            _skipClubCount = 0;
        }
    }

    void disableRelativeValue(const std::string& reason) {
        disableRelativeValue(reason, isEnabled());
    }

    void processInternalMarketUpdate(const MarketUpdate& update) {
        RvContract& contract = _contracts[update._instrumentIndex];
        Market& bid = contract._markets[BID][0];
        bid.setId(update._bidMarketId);
        const hf::price_t bidPrice = update._bidSize == 0 ? hf::NULL_PRICE : update._bidPrice;
        bid.updateInternalMarket(bidPrice, update._bidSize);
        Market& ask = contract._markets[ASK][0];
        ask.setId(update._askMarketId);
        const hf::price_t askPrice = update._askSize == 0 ? hf::NULL_PRICE : update._askPrice;
        ask.updateInternalMarket(askPrice, update._askSize);
    }

    void updateMidPoint(Market& bid, Market& offer, uint32_t instumentIndex, hf::qty_t establishedLevel) {
        const bool useWeightedMid = _useWeightedMid.getValue(instumentIndex);
        double midpoint = calculateMidpoint(bid, offer, useWeightedMid, establishedLevel);
        auto oldMidPoint = _midPoints.getValue(0, instumentIndex);
        _midPoints.setValue(0, instumentIndex, midpoint);
        if (std::abs(oldMidPoint - midpoint) > _contracts[instumentIndex]._priceChangeThreshold) {
            LOG(0, "Updating midpoint for {C[i1]}. midpoint={d1}", getContract(instumentIndex), midpoint);
        }
    }

    void calculateOverdeterminedPrices() {
        DLOG(0, "CalculateOverdeterminedPrices. {i1}", 0)
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
            double sum = 0;
            for (uint32_t allIndex = 0; allIndex < _numberOfAllContracts; ++allIndex) {
                sum += _ODM.getValue(outrightIndex, allIndex) * _midPoints.getValue(0, allIndex);
            }
            _overdeterminedPrices.setValue(0, outrightIndex, sum);
        }
    }

    void calculateVectorMarketPrices() {
        // LOG(0, "calculateVectorMarketPrices {i1}", 0)
        auto priorPrices = _priorVectorMarketPrices;
        _priorVectorMarketPrices = _currentVectorMarketPrices;
        _currentVectorMarketPrices = priorPrices;
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = 0;
            for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
                RvContract& contract = _contracts[outrightIndex];
                sum += _pricingMatrix.getValue(vectorIndex, outrightIndex) * _overdeterminedPrices.getValue(0, outrightIndex) * contract._pricingMultiplier;
            }
            _currentVectorMarketPrices->setValue(vectorIndex, sum);
        }
        if (_calculatedVectorPrices) {
            _canCalculateEma = true;
        } else {
            _calculatedVectorPrices = true;
        }
    }

    void calculateLongTermFairsInVector() {
        // LOG(0, "calculateLongTermFairsInVector {i1}", 0)
        const uint64_t now = clockUtil::GetCycles();
        const uint64_t timeInCycles = now - _lastEmaTime;
        if (_canCalculateEma) {
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                const bool useEma = _useEMAInVector.getValue(vectorIndex);
                if (useEma) {
                    BOOST_ASSERT(std::isfinite(_currentVectorMarketPrices->getValue(vectorIndex)));
                    const uint64_t period = _emaPeriodInVector.getValue(vectorIndex);
                    if(BOOST_LIKELY(period > 0)) {
                        BOOST_ASSERT(std::isfinite(_priorVectorMarketPrices->getValue(vectorIndex)));
                        const double alpha = static_cast<double>(timeInCycles) / period;
                        const double mu = std::exp(-alpha);
                        const double nu = (1 - mu) / alpha;
                        const double lastEma = _longTermFairsInVector.getValue(vectorIndex);
                        const double newEma =
                                mu * lastEma + (nu - mu) * _priorVectorMarketPrices->getValue(vectorIndex) +
                                (1 - nu) * _currentVectorMarketPrices->getValue(vectorIndex);
                        if(BOOST_LIKELY(std::isfinite(newEma))) {
                            _longTermFairsInVector.setValue(vectorIndex, newEma);
                        }
                        // LOG(0, "EMA calculation. Vector={V[i1]} ElapsedTime={i2} Period={i3} alpha={d1} mu={d2} nu={d3} lastEma={d4} newEma={d5}",
                        //    getVectorName(vectorIndex), GetMicrosFromCycles(timeInCycles, _frequency), GetMicrosFromCycles(period, _frequency), alpha, mu, nu, lastEma, newEma)
                    }
                    else {
                        if(BOOST_LIKELY(std::isfinite(_currentVectorMarketPrices->getValue(vectorIndex)))) {
                            _longTermFairsInVector.setValue(vectorIndex, _currentVectorMarketPrices->getValue(vectorIndex));
                        }
                    }
                }
            }
        }
        _lastEmaTime = now;
    }

    double calculateMidpoint(Market& bid, Market& offer, bool useWightedMid, hf::qty_t establishedLevel) const {
        if (useWightedMid) {
            return calculateWeightedMidpoint(bid, offer, establishedLevel);
        }
        return calculateSimpleAverage(bid, offer);
    }

    double calculateWeightedMidpoint(Market& bid, Market& offer, hf::qty_t establishedLevel) const {
        const hf::qty_t offerSize = std::min(offer.getUnAdjustedSize(), establishedLevel);
        const hf::qty_t bidSize = std::min(bid.getUnAdjustedSize(), establishedLevel);
        return (ToDollars(offer.getPrice()) * bidSize + ToDollars(bid.getPrice()) * offerSize) / (bidSize + offerSize);
    }

    double calculateSimpleAverage(Market& bid, Market& offer) const {
        return (ToDollars(bid.getPrice()) + ToDollars(offer.getPrice())) / 2;
    }

    void init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) override {
        ViperStrategyBaseImpl<RelativeValue>::init(frequency, internalHeartbeatPeriod, pnlLimitsChecker);
        initializeModel();
        _latencyTracking.unset();
        _updatedContracts.reserve(_viperConfig._allContracts.size());
    }

    void initializeModel() {
        _widthMultiplier = boost::lexical_cast<double>(_viperConfig.getGlobal(RV_WIDTH_MULTIPLIER));
        initializeContracts();
        _viperConfig.populatePricingMatrix(_pricingMatrix);
        _riskMatrix = _pricingMatrix.inverse().transpose();
        initializeVectorStructures(_viperConfig._vectorConfigs);
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        initializeFromPricingConfig(_viperConfig._pricingConfigs);
        initializeFromRelativeValue(_viperConfig._relativeValueConfigs, _viperConfig._positions[toUInt(StrategyId::RelativeValue)]);
        _viperConfig._indexOfFirstTradedContract = _indexOfFirstTradedContract;
        initializeLeanMatrix(_viperConfig._leanMatrix);
        initializeStructures();
        calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
        _overdeterminedPrices.initialize(1, _numberOfPricingContracts);
        _midPoints.initialize(1, _numberOfAllContracts);
        calculateOverdeterminedMatrix();
    }

    void initializeFromPricingConfig(std::vector<PricingConfig>& config) {
        _weights.init(_numberOfAllContracts);
        _useWeightedMid.init(_numberOfAllContracts);
        _maxTicksWide.init(_numberOfAllContracts);
        for (uint32_t index = 0; index < _numberOfAllContracts; ++index) {
            PricingConfig& pc = config[index];
            RvContract& contract = _contracts[index];
            contract._establishedLevel = pc._establishedLevel;
            double weight = pc._weight;
            _weights.setValue(index, weight);
            contract._usedInPricing = !IsZero(weight);
            _useWeightedMid.setValue(index, pc._useWeightedMid);
            contract._pricingMultiplier = pc._pricingMultiplier;
            contract._quantityMultiplier = pc._quantityMultiplier;
            _maxTicksWide.setValue(index, config[index]._maxTicksWide < 0 ? std::numeric_limits<int64_t>::max() : config[index]._maxTicksWide * contract._definition._tickSize);
        }

        _risk_X_qtyMultiplier_Matrix.initialize(_riskMatrix._numberOfRows, _riskMatrix._numberOfColumns);
        _risk_Div_pricingMultiplier_Matrix.initialize(_riskMatrix._numberOfRows, _riskMatrix._numberOfColumns);
        _vectorRiskMatrix.initialize(_riskMatrix._numberOfRows, _numberOfAllContracts);
        _vectorRisk_Div_qtyMultiplier_Matrix.initialize(_riskMatrix._numberOfRows, _numberOfAllContracts);
        for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfRows; ++vectorIndex) {
            for (uint32_t contractId = 0; contractId < _riskMatrix._numberOfColumns; ++contractId) {
                const auto& contract = _contracts[contractId];
                const auto riskValue = _riskMatrix.getValue(vectorIndex, contractId);
                _risk_X_qtyMultiplier_Matrix.setValue(vectorIndex, contractId, riskValue * contract._quantityMultiplier);
                _risk_Div_pricingMultiplier_Matrix.setValue(vectorIndex, contractId, riskValue / contract._pricingMultiplier);
            }
        }

        double vectorRisk = 0;
        for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfRows; ++vectorIndex) {
            for (const auto& contract : _contracts) {
                if (contract._definition._legs.size() > 1) {
                    vectorRisk = 0;
                    for (const auto& leg: contract._definition._legs) {
                        vectorRisk += _riskMatrix.getValue(vectorIndex, leg._index) * leg._ratio;
                    }
                    vectorRisk = Round<2>(vectorRisk);
                    _vectorRiskMatrix.setValue(vectorIndex, contract._internalId, vectorRisk);
                    _vectorRisk_Div_qtyMultiplier_Matrix.setValue(vectorIndex, contract._internalId,
                                                                  (vectorRisk / contract._quantityMultiplier));
                } else {
                    _vectorRiskMatrix.setValue(vectorIndex, contract._internalId,
                                               Round<2>(_riskMatrix.getValue(vectorIndex, contract._internalId)));
                    _vectorRisk_Div_qtyMultiplier_Matrix.setValue(vectorIndex, contract._internalId,
                                                                  (_vectorRiskMatrix.getValue(vectorIndex,contract._internalId) / contract._quantityMultiplier));
                }
            }
        }
    }

    void calculateOverdeterminedMatrix() {
        Matrix A;
        A.initialize(_numberOfAllContracts, _numberOfPricingContracts);
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            RvContract& contract = _contracts[row];
            for (uint32_t col = 0; col < _numberOfPricingContracts; ++col) {
                double value = 0;
                if (contract._definition._legs.size() > 1) {
                    for (const auto& leg : contract._definition._legs) {
                        if (leg._index == col) {
                            value = leg._ratio;
                        }
                    }
                } else {
                    if (row == col) {
                        value = 1;
                    }
                }
                A.setValue(row, col, value);
            }
        }

        Matrix W;
        W.initialize(_numberOfAllContracts, _numberOfAllContracts);
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            for (uint32_t column = 0; column < _numberOfAllContracts; ++column) {
                double value = 0;
                if (row == column) {
                    value = _weights.getValue(column);
                }
                W.setValue(row, column, value);
            }
        }

        Matrix At = A.transpose();
        Matrix Wt = W.transpose();

        _ODM = At.multiply(Wt).multiply(W).multiply(A).inverse().multiply(At.multiply(Wt).multiply(W));
    }

    void initializeVectorStructures(std::vector<VectorConfig>& config) {
        const uint32_t size = config.size();
        _numberOfVectors = size;
        _vectorsRange.init(size);
        _vectorsMaxPosition.init(size);
        _fareMoveForOneUnitPosition.init(size);
        _vectorMarketPrices[0].init(size);
        _vectorMarketPrices[1].init(size);
        _useEMAInVector.init(size);
        _setToMarket.init(size);
        _emaPeriodInVector.init(size);
        _emaOnTime.init(size);
        _emaOffTime.init(size);
        _longTermFairsInVector.init(size);
        _positionsInVector.init(size);
        _factorHedged.init(size);
        _ltfDelta.init(size);
        auto now = clockUtil::GetCycles();
        for (uint32_t index = 0; index < size; ++index) {
            VectorConfig& vc = config[index];
            _vectorsRange.setValue(index, vc._range);
            _vectorsMaxPosition.setValue(index, vc._maxPos);
            _setToMarket.setValue(index, vc._setToMarket);
            _useEMAInVector.setValue(index, vc._useEMA);
            uint64_t periodInCycles = GetCyclesFromSeconds(vc._EMALength, _frequency);
            _emaPeriodInVector.setValue(index, periodInCycles);
            _emaOnTime.setValue(index, (vc._EMAOn == (uint32_t)-1) ? static_cast<uint64_t>(-1) : convertTimeToCycles(vc._EMAOn / 100, vc._EMAOn % 100, 0, now, _frequency));
            _emaOffTime.setValue(index, (vc._EMAOff == (uint32_t)-1) ? static_cast<uint64_t>(-1) : convertTimeToCycles(vc._EMAOff / 100, vc._EMAOff % 100, 0, now, _frequency));
            _longTermFairsInVector.setValue(index, vc._longTermFair);
            _vectorNames.push_back(vc._vectorName);
            _positionsInVector.setValue(index, 0);
            _fareMoveForOneUnitPosition.setValue(index, 0);
            _vectorMarketPrices[0].setValue(index, 0);
            _vectorMarketPrices[1].setValue(index, 0);
            _factorHedged.setValue(index, vc._hedged);
            _ltfDelta.setValue(index, vc._ltfDelta);
            checkEMAOnOffTime(now, index);
        }
        _currentVectorMarketPrices = &_vectorMarketPrices[0];
        _priorVectorMarketPrices = &_vectorMarketPrices[1];
    }

    void initializeStructures() {
        _hedgeRatios.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _factorHedgedIndexes.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _quoteSizes.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _shortTermFairsInVector.init(_numberOfVectors);
        _shortTermFairsMoveInVector.init(_numberOfVectors);
        _possibleHedgeRatios.initialize(_numberOfVectors, 3);
        _hedgeVectorRiskExposures.initialize(_numberOfVectors, _numberOfVectors);
        _hedgeVectorRiskExposuresSums.init(_numberOfVectors);
        _normalizationMatrix.initialize(_numberOfPricingContracts, 3);
        _commissions.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _shortTermFairsForContracts.initDefault(_numberOfAllContracts);
        _shortTermFairsCache.init(_numberOfAllContracts);
        _shortTermFairsMoveForContract.init(_numberOfAllContracts);
        _rhoneMaxClipSize.init(_numberOfAllContracts);
        _lastOrderRequests.resize(_numberOfAllContracts);
    }

    void initializeLeanMatrix(std::vector<VectorWithContract>& config) {
        _leanMatrix.initialize(_numberOfAllContracts, _numberOfAllContracts);
        for (uint32_t row = 0; row < config.size(); ++row) {
            for (uint32_t col = 0; col < config.size(); ++col) {
                VectorWithContract& rowV = config[row];
                VectorWithContract& colV = config[col];
                RvContract* rowContract = getContract(rowV._contract);
                RvContract* colContract = getContract(colV._contract);
                bool included = config[row]._values[col] == 1;
                _leanMatrix.setValue(rowContract->_internalId, colContract->_internalId, included);
            }
        }
    }

    void initializeFromRelativeValue(const std::vector<RelativeValueConfig>& config, const std::vector<int32_t>& relativeValueInitialPositions) {
        _commissionsInput.init(_numberOfAllContracts);
        _contractPositions.init(_numberOfAllContracts);
        _contractQuoteExternal.init(_numberOfAllContracts);
        _staticQuote.init(_numberOfAllContracts);
        _contractWidths.init(_numberOfAllContracts);
        _extraContractWidths.init(_numberOfAllContracts);
        _pctStfMoves.init(_numberOfAllContracts);
        _maxTicksThroughMarket.init(_numberOfAllContracts);
        _staticQuoteSize.init(_numberOfAllContracts);
        for (uint32_t contractIndex = 0; contractIndex < config.size(); ++contractIndex) {
            const RelativeValueConfig& rvConfig = config[contractIndex];
            RvContract* contract = getContract(rvConfig._contract);
            if (contractIndex == 0) {
                _indexOfFirstTradedContract = contract->_internalId;
            }
            if (contract != nullptr) {
                _commissionsInput.setValue(contract->_internalId, rvConfig._commission);
                _contractWidths.setValue(contract->_internalId, ToRhonePrice(rvConfig._width));
                _contractQuoteExternal.setValue(contract->_internalId, rvConfig._quoteExternal);
                _pctStfMoves.setValue(contract->_internalId, rvConfig._pctStfMove);
                _maxTicksThroughMarket.setValue(contract->_internalId, rvConfig._maxTicksThroughMarket * contract->_definition._tickSize);
                _staticQuote.setValue(contract->_internalId, rvConfig._staticQuote);
                _staticQuoteSize.setValue(contract->_internalId, rvConfig._quoteSize);
                contract->_traded = true;
            } else {
                logging::GetLogger().logErrorAndThrow("Got unknown contract when initializing rel value");
            }
        }
        for(unsigned i = 0; i < relativeValueInitialPositions.size(); ++i) {
            auto& initialPos = relativeValueInitialPositions[i];
            if(initialPos) {
                addPosition(_contracts[i], initialPos, false, true);
            }
        }

        calculateExtraContractWidths();
        checkVectorPositionLimits(true);
    }

    // unhide base class function
    using ViperResource::getContract;

    RvContract* getContract(const std::string& name) {
        for (auto& c : _contracts) {
            if (c._definition._name == name) {
                return &c;
            }
        }
        return nullptr;
    }

    void initializeContracts() {
        for (uint32_t contractIndex = 0; contractIndex < _viperConfig._allContracts.size(); ++contractIndex) {
            const ConfigContract& configContract = _viperConfig._allContracts[contractIndex];
            RvContract contract;
            contract._internalId = contractIndex;
            contract.initMarkets(contractIndex);
            auto& definition = contract._definition;
            definition = configContract;
            contract._priceChangeThreshold = 0.05 * definition._tickSize;
            _contracts.push_back(contract);
        }
        _numberOfAllContracts = _viperConfig._allContracts.size();
        _numberOfPricingContracts = _viperConfig._numberOfPricingContracts;

        _costMultiplierMatrix.initialize(_numberOfAllContracts, _numberOfAllContracts);
        for (auto& contract1 : _contracts) {
            for (auto& contract2 : _contracts) {
                _costMultiplierMatrix.setValue(contract1._internalId, contract2._internalId,
                                          contract1._pricingMultiplier / contract2._pricingMultiplier * contract1._quantityMultiplier / contract2._quantityMultiplier);
            }
        }
    }

    void calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes() {
        for (uint32_t rowIndex = _indexOfFirstTradedContract; rowIndex < _numberOfAllContracts; ++rowIndex) {
            for (uint32_t columnIndex = _indexOfFirstTradedContract; columnIndex < _numberOfAllContracts; ++columnIndex) {
                double hedgeRatio = 0;
                if (rowIndex != columnIndex) {
                    int vectorIndex;
                    hedgeRatio = calculateHedgeRatio(rowIndex, columnIndex, vectorIndex);
                    _factorHedgedIndexes.setValue(rowIndex, columnIndex, vectorIndex);
                } else {
                    calculateVectorRiskExposureForContracts(rowIndex, columnIndex);
                    _factorHedgedIndexes.setValue(rowIndex, columnIndex, -1);
                }
                _hedgeRatios.setValue(rowIndex, columnIndex, hedgeRatio);
                calculateNormalization(rowIndex, columnIndex, hedgeRatio);
                const double commission = calculateCommission(rowIndex, columnIndex, hedgeRatio);
                _commissions.setValue(rowIndex, columnIndex, ToRhonePrice(commission));
                const double quoteSize = calculateQuoteSize(rowIndex, columnIndex, hedgeRatio);
                _quoteSizes.setValue(rowIndex, columnIndex, quoteSize);
            }
        }
    }

    enum PossibleHedgeRatiosColumns {ContractOneRisk = 0, ContractTwoRisk = 1, PossibleHedgeRatio = 2};

    void calculateVectorRiskExposureForContracts(uint32_t contractOneIndex, uint32_t contractTwoIndex) {
        auto& contractOneDefinition = _contracts[contractOneIndex]._definition;
        auto& contractTwoDefinition = _contracts[contractTwoIndex]._definition;
        calculateVectorRiskExposureForContract(PossibleHedgeRatiosColumns::ContractOneRisk, contractOneDefinition, _contracts[contractOneIndex]._quantityMultiplier);
        calculateVectorRiskExposureForContract(PossibleHedgeRatiosColumns::ContractTwoRisk, contractTwoDefinition, _contracts[contractTwoIndex]._quantityMultiplier);
    }

    double calculateHedgeRatio(uint32_t contractOneIndex, uint32_t contractTwoIndex, int& vectorIndex) {
        calculateVectorRiskExposureForContracts(contractOneIndex, contractTwoIndex);
        calculatePossibleHedgeRatios();
        calculateVectorRisks();
        sumUpVectorRisks();
        return pickBestHedge(vectorIndex);
    }

    void calculateVectorRiskExposureForContract(uint32_t tableColumn, ConfigContract& contractDefinition, double quantityMultiplier) {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double exposure = 0;
            if (contractDefinition._legs.size() > 1) {
                for (const auto& leg : contractDefinition._legs) {
                    double vectorRik = _riskMatrix.getValue(vectorIndex, leg._index);
                    exposure += vectorRik * leg._ratio * quantityMultiplier;
                }
            } else {
                exposure = _riskMatrix.getValue(vectorIndex, contractDefinition._internalId) * quantityMultiplier;
            }
            _possibleHedgeRatios.setValue(vectorIndex, tableColumn, exposure);
        }
    }

    void calculatePossibleHedgeRatios() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const double hedgeValue = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractTwoRisk);
            double hedgeRatio = 0;
            if (!IsZero(hedgeValue)) {
                hedgeRatio = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractOneRisk) / hedgeValue;
            }
            _possibleHedgeRatios.setValue(vectorIndex, PossibleHedgeRatiosColumns::PossibleHedgeRatio, hedgeRatio);
        }
    }

    void calculateVectorRisks() {
        for (uint32_t vectorIndexRow = 0; vectorIndexRow < _numberOfVectors; ++vectorIndexRow) {
            for (uint32_t vectorIndexColumn = 0; vectorIndexColumn < _numberOfVectors; ++vectorIndexColumn) {
                double vectorRisk = _possibleHedgeRatios.getValue(vectorIndexRow, PossibleHedgeRatiosColumns::ContractOneRisk)
                                    - _possibleHedgeRatios.getValue(vectorIndexRow, PossibleHedgeRatiosColumns::ContractTwoRisk)
                                      * _possibleHedgeRatios.getValue(vectorIndexColumn, PossibleHedgeRatiosColumns::PossibleHedgeRatio);
                _hedgeVectorRiskExposures.setValue(vectorIndexRow, vectorIndexColumn, vectorRisk);
            }
        }
    }

    void sumUpVectorRisks() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposures.multiplyAbsColumn(vectorIndex, _fareMoveForOneUnitPosition);
            _hedgeVectorRiskExposuresSums.setValue(vectorIndex, sum);
        }
    }

    double pickBestHedge(int& vectorIndexOut) {
        std::vector<uint32_t> minIndexes;
        double min = _hedgeVectorRiskExposuresSums.getValue(0);
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposuresSums.getValue(vectorIndex);
            bool isTheSame = std::abs(sum - min) < 0.000001;
            if (isTheSame) {
                minIndexes.push_back(vectorIndex);
            } else if (sum < min) {
                minIndexes.clear();
                minIndexes.push_back(vectorIndex);
                min = sum;
            }
        }
        uint32_t index = minIndexes[0];
        if (minIndexes.size() > 1) {
            double move = _fareMoveForOneUnitPosition.getValue(index);
            for (auto i : minIndexes) {
                double m = _fareMoveForOneUnitPosition.getValue(i);
                if (m > move) {
                    move = m;
                    index = i;
                }
            }
        }
        vectorIndexOut = _factorHedged.getValue(index) ? (int)index : -1;

        return Round<4>(_possibleHedgeRatios.getValue(index, PossibleHedgeRatiosColumns::PossibleHedgeRatio));
    }

    double calculateCommission(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedge) {
        RvContract& oneContract = _contracts[contractOneIndex];
        RvContract& twoContract = _contracts[contractTwoIndex];
        const double commissionOne = _commissionsInput.getValue(contractOneIndex);
        const double commissionTwo = _commissionsInput.getValue(contractTwoIndex);
        const double normalizedValue = findNormalizer();
        return (commissionOne * oneContract._pricingMultiplier + std::abs(hedge) * commissionTwo * twoContract._pricingMultiplier) / normalizedValue;
    }

    enum NormalizationMatrixColumns {ContractOneRatio = 0, ContractTwoRatio = 1, Total = 2};

    void calculateNormalization(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedge) {
        auto& contractDefinitionOne = _contracts[contractOneIndex]._definition;
        setHedgeRatioForNormalization(contractDefinitionOne, 1, NormalizationMatrixColumns::ContractOneRatio);
        auto& contractDefinitionTwo = _contracts[contractTwoIndex]._definition;
        setHedgeRatioForNormalization(contractDefinitionTwo, hedge, NormalizationMatrixColumns::ContractTwoRatio);
        sumUpRatiosForNormalization();
    }

    void setHedgeRatioForNormalization(ConfigContract& contractDefinition, double multiplier, uint32_t column) {
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
            double value = 0;
            if (contractDefinition._legs.size() > 1) {
                for (uint32_t legIndex = 0; legIndex < contractDefinition._legs.size(); ++legIndex) {
                    auto& leg = contractDefinition._legs[legIndex];
                    if (leg._index == outrightIndex) {
                        value = leg._ratio;
                    }
                }
            } else if (contractDefinition._internalId == outrightIndex) {
                value = 1;
            }
            _normalizationMatrix.setValue(outrightIndex, column, value * multiplier);
        }
    }

    void sumUpRatiosForNormalization() {
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
            double value = _normalizationMatrix.getValue(outrightIndex, NormalizationMatrixColumns::ContractOneRatio) - _normalizationMatrix.getValue(outrightIndex, NormalizationMatrixColumns::ContractTwoRatio);
            _normalizationMatrix.setValue(outrightIndex, NormalizationMatrixColumns::Total, value);
        }
    }

    double findNormalizer() {
        double min = std::numeric_limits<double>::max();
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
            double value = _normalizationMatrix.getValue(outrightIndex, NormalizationMatrixColumns::Total);
            if (!IsZero(value)) {
                double absValue = std::abs(value);
                if (absValue < min) {
                    min = absValue;
                }
            }
        }
        return min;
    }

    double calculateQuoteSize(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedgeRatio) {
        Vector<double> oneLotMoves;
        oneLotMoves.init(_numberOfVectors);

        RvContract& quoteContract = _contracts[contractOneIndex];
        RvContract& leanContract = _contracts[contractTwoIndex];

        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const double riskExposure = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractOneRisk)
                                        - _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractTwoRisk) * hedgeRatio;
            const double fairMove = _fareMoveForOneUnitPosition.getValue(vectorIndex);
            oneLotMoves.setValue(vectorIndex, riskExposure * fairMove);
        }
        Vector<double> fairMoveForContract;
        fairMoveForContract.init(_numberOfAllContracts);
        for (uint32_t contractIndex = 0; contractIndex < _numberOfPricingContracts; ++contractIndex) {
            RvContract& contract = _contracts[contractIndex];
            double move = 0;
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                move += oneLotMoves.getValue(vectorIndex) * _risk_Div_pricingMultiplier_Matrix.getValue(vectorIndex, contractIndex);
            }
            fairMoveForContract.setValue(contractIndex, move);
        }

        constexpr double InitMinQuoteSizeValue = std::numeric_limits<double>::max();
        double minQuoteSize = InitMinQuoteSizeValue;

        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            double move = 0;
            RvContract& contract = _contracts[spreadIndex];
            if (contract._definition._legs.size() > 0) {
                for (const auto& leg : contract._definition._legs) {
                    const double legMove = fairMoveForContract.getValue(leg._index) * leg._ratio;
                    move += legMove;
                }
            } else {
                move = fairMoveForContract.getValue(spreadIndex);
            }
            double moveAsPercentage = std::abs(move) / ToDollars(_contractWidths.getValue(spreadIndex));
            minQuoteSize = std::min(minQuoteSize, _pctStfMoves.getValue(spreadIndex) / moveAsPercentage);
        }

        if(minQuoteSize < 0 || minQuoteSize == InitMinQuoteSizeValue) {
            logging::GetLogger().logError(boost::str(boost::format("Calculate quote size.  Min quote size was invalid, setting to zero.  Contract=%1% Lean=%2% InvalidValue=%3%") % quoteContract._definition._name % leanContract._definition._name % minQuoteSize));
            minQuoteSize =  0;
        }

        logging::GetLogger().logInfo(boost::str(boost::format("Calculated quote size. Contract=%1% Lean=%2% Size=%3%") % quoteContract._definition._name % leanContract._definition._name % minQuoteSize));

        return minQuoteSize;
    }

    void sendQuoteSizesToGUI() {
        sendGenericMessageToFront(ViperMessageType::QuoteSizesUpdate);
    }

    double sendHedges(RvContract& contract, double size, uint32_t side, bool& removeRequest, double residual) {
        LOG(0, "RV {C[i1]}. Send hedges for fill. Size={d1} Side={i2}", getContract(contract._internalId), size, side)
        const uint64_t hedgeStartTimestamp = clockUtil::GetCycles();
        Quote& quote = contract._quotes[side];
        double needToHedge = size;
        hf::price_t roundedPrice = hf::NULL_PRICE;
        bool sentHedge = false;
        for (uint32_t leanIndex = 0; leanIndex < quote._leans._leanIndex; ++leanIndex) {
            Lean& lean = quote._leans._leans[leanIndex];
            RvContract& leanContract = _contracts[lean._instrumentIndex];
            double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, lean._instrumentIndex);
            hf::price_t price = RoundToTickSize(lean._price, leanContract._definition._tickSize, side);
            if (roundedPrice == hf::NULL_PRICE) {
                roundedPrice = price;
            }
            const bool belongsToClub = price == roundedPrice;
            if (belongsToClub) {
                double quoteSize = _quoteSizes.getValue(contract._definition._internalId, lean._instrumentIndex);
                Market& hedgeMarket = *lean._market;
                const double oldNeedToHedge = needToHedge;
                const hf::qty_t orderSize = getOrderSize(leanContract._definition, hedgeRatio, hedgeMarket.getAdjustedSize(), needToHedge, lean._size, residual, quoteSize);
                LOG(0, "RV {C[i1]}. Hedging in {C[i2]} Size={i3} HedgeRatio={d1} NeedToHedge={d2} LearnQuote={i4}@{d3}",
                    getContract(contract._internalId), getContract(leanContract._internalId), orderSize, hedgeRatio,
                    needToHedge, leanContract._quotes[side]._bestSize, ToDollars(leanContract._quotes[side]._bestPrice))
                if (orderSize > 0) {
                    const uint32_t otherSide = GetOtherSide(side);
                    Quote& leanQuote = leanContract._quotes[side];
                    const hf::price_t leanQuotePrice = leanQuote._bestPrice;
                    const hf::price_t hedgePrice = hedgeMarket.getPrice();
                    const bool isHedgePriceCrossingQuotePrice = (leanQuotePrice == hf::NULL_PRICE) ? false : IsBetterOrEqual(hedgePrice, leanQuotePrice, otherSide);
                    if (isHedgePriceCrossingQuotePrice || hedgePrice == hf::NULL_PRICE ||
                        IsTicksWorse(hedgePrice, leanQuotePrice, side, hedgeRatio, leanContract, contract, 2)) {
                        removeRequest = true;
                        needToHedge = oldNeedToHedge;
                        LOG(0, "RV {C[i1]}. Hedging. Skipping club member b/c of potential self match or 2 ticks worse. QuotePrice={d1} HedgePrice={d2} Lean={C[i2]}",
                            getContract(contract._internalId), ToDollars(leanQuotePrice), ToDollars(hedgePrice), getContract(leanContract._internalId))
                        if (++_skipClubCount >= 50) {
                            disableRelativeValue("Skip club reached 50 times");
                            break;
                        }
                        continue;
                    }
                    const uint32_t hedgeId = getHedgeId();
                    LOG(0, "RV Sending hedge Contract={C[i1]} Side={i2} Price={d1} Size={i3} External={i4} HedgeId={i5} QuoteContract={C[i6]} MarketId={i7}",
                        getContract(leanContract._internalId), otherSide, ToDollars(hedgePrice), orderSize,
                        (int) lean._isExternal, hedgeId, getContract(contract._internalId), lean._marketIndex)

                    hedgeMarket.addFAKAdjustment(hedgeId, orderSize);
                    sendHedgeOrder(leanContract, otherSide, hedgePrice, orderSize, lean._isExternal, hedgeId, lean._marketIndex);
                    addPosition(leanContract, otherSide == BID ? orderSize : -orderSize, false, false);
                    sentHedge = true;
                }

                if ((needToHedge < 0) | (IsZero(needToHedge))) {
                    LOG(0, "RV {C[i1]}. Fully hedged. Side={i2} NeedToHedge={d1}.", getContract(contract._internalId), side, needToHedge)
                    removeRequest = true;
                    break;
                }
            } else {
                if (sentHedge) {
                    LOG(0, "RV {C[i1]}. Not Fully hedged but sent hedge so will recalculate. Side={i2} NeedToHedge={d1}.", getContract(contract._internalId), side, needToHedge)
                } else {
                    LOG(0, "RV {C[i1]}. Not Fully hedged and did not send hedge. Side={i2} NeedToHedge={d1}.", getContract(contract._internalId), side, needToHedge)
                    removeRequest = true;
                }
                break;
            }
        }

        if (!sentHedge) {
            LOG(0, "RV {C[i1]}. Did not send hedge. Removing. Side={i2} NeedToHedge={d1}.", getContract(contract._internalId), side, needToHedge)
            removeRequest = true;
        }

        const uint64_t latency = clockUtil::GetCycles() - hedgeStartTimestamp;
        LOG(0, "RV HedgeLatency {i1}", latency)
        return needToHedge;
    }

    bool IsTicksWorse(hf::price_t lhs, hf::price_t rhs, uint32_t side, double hedgeRatio, RvContract& leanContract, RvContract& quoteContract, uint32_t nticks) {
        if (IsBetterOrEqual(lhs, rhs, side)) {
            return false;
        }
        auto delta = std::abs(lhs - rhs);
        hf::price_t perspective_quote = (delta * hedgeRatio * _costMultiplierMatrix.getValue(leanContract._internalId, quoteContract._internalId));
        return perspective_quote >= (nticks * quoteContract._definition._tickSize);
    }

    uint32_t getHedgeId() {
        return ++_hedgeId;
    }

    uint32_t getLastHedgeId() const {
        return _hedgeId;
    }

    void sendHedgeOrder(RvContract& contract,
                        uint32_t side,
                        hf::price_t price,
                        hf::qty_t size,
                        bool external,
                        uint32_t hedgeId,
                        uint32_t marketId) {
        _lastOrderRequests[contract._internalId]._hedge[side] = _orderRequestMgr.sendHedgeRequest(
                contract._internalId, orderId(StrategyId::RelativeValue, Order::Type::Hedging), side,
                price, price, size, external, false, 0,
                _shortTermFairsForContracts.getRef(contract._internalId)._fair, marketId, hedgeId);
        // send blocked quotes
        bool quoteExternal = canQuoteExternal(contract._internalId, contract._definition._venue);
        sendQuoteOrder(contract, quoteExternal, true);
    }

    hf::qty_t getOrderSize(ConfigContract& contract, double hedgeRatio, hf::qty_t minAvailableLeanSize, double& needToHedge, double size, double residuals, double quoteSize) {
        if (IsZero(hedgeRatio)) {
            // this is the no quote path
            LOG(0, "No hedge path. Reduce NeedToHedge {d1} by {d2}", needToHedge, quoteSize)
            needToHedge -= quoteSize;
            return 0;
        }
        double maxHedgeSize = (size + residuals) * hedgeRatio;
        double availableLeanSize = minAvailableLeanSize;
        if (availableLeanSize > maxHedgeSize) {
            availableLeanSize = maxHedgeSize;
        }
        double needToHedgeInHedgeTerms = needToHedge * hedgeRatio;
        LOG(0, "Need to hedge in terms of lean. {d1} MinAvailable={i1} Cap={d2} Available={d3}", needToHedgeInHedgeTerms, minAvailableLeanSize, maxHedgeSize, availableLeanSize)
        hf::qty_t orderSize;
        if (needToHedgeInHedgeTerms >= availableLeanSize) {
            orderSize = contract.toValidQty(static_cast<hf::qty_t>(std::round(availableLeanSize)));
        } else {
            orderSize = contract.toValidQty(static_cast<hf::qty_t>(std::round(needToHedgeInHedgeTerms)));
        }
        needToHedge = (needToHedgeInHedgeTerms - orderSize) / hedgeRatio;
        LOG(0, "Hedge OrderSize={i1} Remaining NeedToHedge={d1}", orderSize, needToHedge)
        return orderSize;
    }

    void addPosition(RvContract& contract, double size, bool checkLimit, bool isStartUp) {
        if (isStartUp) {
            logging::GetLogger().logInfo(boost::str(boost::format("RV Adding position. Contract=%1% Size=%2%") % getContract(contract._internalId) % size));
        } else {
            LOG(0, "RV {C[i1]}. Adding position. Size={d1}", getContract(contract._internalId), size)
        }
        addPositionToVector(contract, size, checkLimit, isStartUp);
        _contractPositions.add(contract._definition._internalId, size);
        // logVectorPositions(isStartUp);
        // logContractPositions(isStartUp);
    }

    void addPositionToVector(RvContract& contract, double positionChange, bool checkLimit, bool isStartUp) {
        if (contract._definition._legs.size() > 1) {
            for (const auto& leg : contract._definition._legs) {
                const double legPositionChange = positionChange * leg._ratio;
                for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                    const double changeInVector = legPositionChange * _risk_X_qtyMultiplier_Matrix.getValue(vectorIndex, leg._index);
                    _positionsInVector.add(vectorIndex, changeInVector);
                }
            }
            if (checkLimit) {
                for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                    checkVectorPositionLimit(vectorIndex, isStartUp);
                }
            }
        } else {
            for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                const double changeInVector = positionChange * _risk_X_qtyMultiplier_Matrix.getValue(vectorIndex, contract._definition._internalId);
                _positionsInVector.add(vectorIndex, changeInVector);
                if (checkLimit) {
                    checkVectorPositionLimit(vectorIndex, isStartUp);
                }
            }
        }
    }

    void checkVectorPositionLimit(uint32_t vectorIndex, bool isStartUp) {
        const double vectorPosition = _positionsInVector.getValue(vectorIndex);
        const double limit = _vectorsMaxPosition.getValue(vectorIndex);
        if (std::abs(vectorPosition) > limit) {
            if (isStartUp) {
                logging::GetLogger().logError("Vector position max breached");
            } else {
                LOG(0, "Vector position max breached. Vector={V[i1]} Position={d1} Limit={d2}", getVectorName(vectorIndex), vectorPosition, limit)
            }
            disableRelativeValue(boost::str(boost::format("Vector position max breached. Vector=%1% Position=%2% Limit=%3%") % _vectorNames.at(vectorIndex) % vectorPosition % limit));
        }
    }

    void checkVectorPositionLimits(bool isStartUp) {
        for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
            checkVectorPositionLimit(vectorIndex, isStartUp);
        }
    }

    void logContractPositions(bool isStartup) {
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            if (!isStartup) {
                LOG(0, "Contract position. Contract={C[i1]} Position={d1}", getContract(spreadIndex), _contractPositions.getValue(spreadIndex))
            }
        }
    }

    void logVectorPositions(bool isStartup) {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            if (!isStartup) {
                LOG(0, "Vector position. {V[i1]} Position={d1}", getVectorName(vectorIndex), _positionsInVector.getValue(vectorIndex))
            }
        }
    }

    void setShortTermFairToMarket() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            setShortTermFairToMarket(vectorIndex);
        }
    }

    void setShortTermFairToMarket(uint32_t vectorIndex) {
        const bool setToMarket = _setToMarket.getValue(vectorIndex);
        double market = _currentVectorMarketPrices->getValue(vectorIndex);
        if (!setToMarket) {
            double ltf = market + _positionsInVector.getValue(vectorIndex) * _fareMoveForOneUnitPosition.getValue(vectorIndex);
            _longTermFairsInVector.setValue(vectorIndex, ltf);
            sendGenericMessageToFront(ViperMessageType::StoreLtf, vectorIndex);
        }
        _shortTermFairsInVector.setValue(vectorIndex, market);
    }

    void calculateShortTermFairs() {
        // LOG(0, "calculateShortTermFairs {i1}", 0);

        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const bool setToMarket = _setToMarket.getValue(vectorIndex);
            double value;
            if (!setToMarket) {
                value = _longTermFairsInVector.getValue(vectorIndex) - _positionsInVector.getValue(vectorIndex) * _fareMoveForOneUnitPosition.getValue(vectorIndex);
            } else {
                value = _currentVectorMarketPrices->getValue(vectorIndex);
            }
            _shortTermFairsInVector.setValue(vectorIndex, value);
        }

        for (uint32_t contractIndex = 0; contractIndex < _numberOfPricingContracts; ++contractIndex) {
            RvContract& contract = _contracts[contractIndex];
            double shortTermFair = 0;
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                const double value = _shortTermFairsInVector.getValue(vectorIndex) * _risk_Div_pricingMultiplier_Matrix.getValue(vectorIndex, contractIndex);
                shortTermFair += value;
            }
            const hf::price_t rhonePrice = ToRhonePrice(shortTermFair);
            auto& stf = _shortTermFairsForContracts.getRef(contractIndex);
            auto oldValue = stf._fair;
            stf._fair = rhonePrice;
            if (stf._subscribed && oldValue != rhonePrice) {
                _shortTermFairUpdate.emplace(contractIndex, &stf);
            }
        }

        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            auto& contractDefinition = _contracts[spreadIndex]._definition;
            hf::price_t shortTermFair = 0;
            if (contractDefinition._legs.size() > 1) {
                for (const auto& leg : contractDefinition._legs) {
                    hf::price_t value = _shortTermFairsForContracts.getRef(leg._index)._fair * leg._ratio;
                    shortTermFair += value;
                }
            } else {
                shortTermFair = _shortTermFairsForContracts.getRef(spreadIndex)._fair;
            }
            const hf::price_t rhonePrice = shortTermFair;
            auto& stf = _shortTermFairsForContracts.getRef(spreadIndex);
            auto oldValue = stf._fair;
            stf._fair = rhonePrice;
            if (stf._subscribed && oldValue != rhonePrice) {
                _shortTermFairUpdate.emplace(contractDefinition._internalId, &stf);
            }
        }

        // logShortTermFairsForVectors();
        logShortTermFairsForContracts();
        _hasDataForCharts = true;
    }

    void sendSTFUpdates(bool reset = false) {
        if (!_shortTermFairUpdate.empty()) {
            if (isEnabled() || reset) {
                uint32_t dataSize = _shortTermFairUpdate.size() * sizeof(STFUpdate::STF);
                getStrategy(StrategyId::ArbieSpreader)->sendMessage<STFUpdate>(
                        ViperMessageType::STFUpdate,
                        [this, reset](STFUpdate& msg) {
                            msg._numSTF = _shortTermFairUpdate.size();
                            STFUpdate::STF* data = msg._data;
                            for (auto& kvp : _shortTermFairUpdate) {
                                data->_contractId = kvp.first;
                                if (reset) {
                                    data->_fair = hf::NULL_PRICE;
                                    kvp.second->_fair = 0;
                                    kvp.second->_firstUpdateSent = false;
                                } else {
                                    data->_fair = kvp.second->_fair;
                                    if (BOOST_UNLIKELY(!kvp.second->_firstUpdateSent)) {
                                        kvp.second->_firstUpdateSent = true;
                                        logging::GetLogger().logInfo(boost::str(boost::format("RV. Send %1% stf. stf=%2%") % _contracts[kvp.first]._definition._name % ToDollars(kvp.second->_fair)));
                                    }
                                }
                                ++data;
                            }
                        }, dataSize);
                _shortTermFairUpdate.clear();
            }
        }
    }

    void logShortTermFairsForVectors() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            LOG(0, "Vector short term fairs Vector={V[i1]} Fair={d1}", getVectorName(vectorIndex), _shortTermFairsInVector.getValue(vectorIndex))
        }
    }

    void logShortTermFairsForContracts() {
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            auto fair = _shortTermFairsForContracts.getRef(spreadIndex)._fair;
            if (std::abs(fair - _shortTermFairsCache.getValue(spreadIndex)) > _contracts[spreadIndex]._priceChangeThreshold) {
                LOG(0, "RV {C[i1]}. short term fairs. Fair={d1}", getContract(spreadIndex), ToDollars(fair))
            }
            _shortTermFairsCache.setValue(spreadIndex, fair);
        }
    }

    struct ValidateMaxTicksThroughMarketResult {
        hf::price_t quoteWithoutCap = hf::NULL_PRICE;
        bool isValid = true;
    };

    //Returns hf::null_price if passes
    template<uint32_t side>
    ValidateMaxTicksThroughMarketResult validateMaxTicksThroughMarket(hf::price_t calculatedPrice, uint32_t leanIndex, RvContract& contract, uint32_t instrumentIndex) {
        static_assert((side == BID) || (side == ASK), "Bad side");

        ValidateMaxTicksThroughMarketResult result;

        hf::price_t leanFair = _shortTermFairsForContracts.getRef(leanIndex)._fair;
        hf::price_t fair = _shortTermFairsForContracts.getRef(instrumentIndex)._fair;
        hf::price_t width = _contractWidths.getValue(instrumentIndex) + _extraContractWidths.getValue(instrumentIndex);
        double hedgeRatio = _hedgeRatios.getValue(instrumentIndex, leanIndex);

        auto& leanMarkets = _contracts[leanIndex];

        hf::price_t otherSideBest = contract.bestMarket<GetOtherSide<side>()>().getPrice();
        if(otherSideBest == hf::NULL_PRICE) {
            return result;
        }

        auto& bestMarket = leanMarkets.template bestMarket<side>();

        //One of these formulas should have produced the calculatedPrice
        BOOST_ASSERT((calculatedPrice == calculateQuote<side, true>(leanMarkets.intenralMarket<side>(), contract, leanIndex, leanFair, fair, width, hedgeRatio, false))
                || (calculatedPrice == calculateQuote<side, true>(leanMarkets.externalMarket<side>(), contract, leanIndex, leanFair, fair, width, hedgeRatio, true)));

        result.quoteWithoutCap = calculateQuote<side, false>(bestMarket, contract, leanIndex, leanFair, fair, width, hedgeRatio, bestMarket.getId() == MarketIds::MARKET_ID_EXTERNAL);
        if(calculateTicksThroughMarket<side>(result.quoteWithoutCap, otherSideBest) >= _maxTicksThroughMarket.getValue(instrumentIndex)) {
            result.isValid = false;
        }
        return result;
    }

    bool calculateQuotes() {
        SCOPED_PERF_POINT(PerfId::RV_CalculateQuote);
        // LOG(0, "CalculateQuotes {i1}", 0);
        const uint64_t startTimeStamp = clockUtil::GetCycles();
        // logCurrentExternalMarkets();
        bool noErrorsOnCreateQuote = true;
        std::vector<uint32_t> quotedContract;
        quotedContract.reserve(_numberOfAllContracts);

        for (uint32_t instrumentIndex = _indexOfFirstTradedContract; instrumentIndex < _numberOfAllContracts; ++instrumentIndex) {
            RvContract& contract = _contracts[instrumentIndex];
            contract.clearLeans();
            if (contract._traded) {
                // LOG(0, "Calculating quotes for {C[i1]}", getContract(contract._internalId))
                for (uint32_t leanIndex = _indexOfFirstTradedContract; leanIndex < _numberOfAllContracts; ++leanIndex) {
                    const bool canLeanOn = _leanMatrix.getValue(instrumentIndex, leanIndex);
                    const bool canQuote = _hedgeRatios.getValue(instrumentIndex, leanIndex) >= 0;
                    if (canLeanOn && canQuote) {
                        const hf::price_t fair = _shortTermFairsForContracts.getRef(instrumentIndex)._fair;
                        const hf::price_t width = _contractWidths.getValue(instrumentIndex) + _extraContractWidths.getValue(instrumentIndex);
                        const hf::price_t leanFair = _shortTermFairsForContracts.getRef(leanIndex)._fair;
                        const double maxQuoteSize = _quoteSizes.getValue(contract._definition._internalId, leanIndex);

                        RvContract& leanContract = _contracts[leanIndex];

                        auto& leanBids = leanContract.bestMarket<BID>();
                        calculateQuote<BID>(leanBids, contract, leanIndex, leanFair, fair, width, maxQuoteSize, leanBids.getId() == MarketIds::MARKET_ID_EXTERNAL);

                        auto& leanOffers = leanContract.bestMarket<ASK>();
                        calculateQuote<ASK>(leanOffers, contract, leanIndex, leanFair, fair, width, maxQuoteSize, leanOffers.getId() == MarketIds::MARKET_ID_EXTERNAL);
                    }
                }

                const bool quoteExternal = canQuoteExternal(instrumentIndex, contract._definition._venue);

                bool staticQuote = _staticQuote.getValue(contract._internalId);
                uint32_t quoteSize = _staticQuoteSize.getValue(contract._internalId);

                Quote& bidQuote = contract._quotes[BID];
                noErrorsOnCreateQuote &= bidQuote.createQuote(contract._definition._tickSize, BID, quoteExternal, _contracts, instrumentIndex, isEnabled(), staticQuote, quoteSize,
                        [&](hf::price_t calculatedPrice, uint32_t leanIndex) { return validateMaxTicksThroughMarket<BID>(calculatedPrice, leanIndex, contract, instrumentIndex); },
                        [&](const std::string& errorMsg) { sendErrorMessage(errorMsg, ErrorType::Critical); });

                Quote& askQuote = contract._quotes[ASK];
                noErrorsOnCreateQuote &= askQuote.createQuote(contract._definition._tickSize, ASK, quoteExternal, _contracts, instrumentIndex, isEnabled(), staticQuote, quoteSize,
                        [&](hf::price_t calculatedPrice, uint32_t leanIndex) { return validateMaxTicksThroughMarket<ASK>(calculatedPrice, leanIndex, contract, instrumentIndex); },
                        [&](const std::string& errorMsg) { sendErrorMessage(errorMsg, ErrorType::Critical); });

                if(BOOST_UNLIKELY(!noErrorsOnCreateQuote)) {
                    std::string error = "MaxTicksThroughMarketCheckFailed.  Contract=";
                    disableRelativeValue(error + contract._definition._name);
                    LOG(0, "MaxTicksThroughMarketCheckFailed. Disable External Quoting. OffendingContract={C[i1]}", getContract(contract._internalId))
                    sendErrorMessage("Disabling QuoteExternal. Reason=RV MaxTicksThroughMarketCheckFailed", ErrorType::Critical);
                    disableQuoteExternal();
                    break;
                }

                sendQuoteOrder(contract, quoteExternal);
            }
        }

        const uint64_t latency = clockUtil::GetCycles() - startTimeStamp;
        DLOG(0, "RV CalculateQuotes Latency {i1}", latency)

        //noErrorsOnCreateQuote will likely disable RV, causing canSendQuotes() to return false
        if (BOOST_LIKELY((!noErrorsOnCreateQuote) | canSendQuotes())) {
            LOG(0, "RV Start of internal fills transaction {i1}", _fillsTransactionNumber);
            _withinInternalFillsOfCycle = true;
            sendGenericMessageToExecutor(ViperMessageType::EndOfQuotes, _fillsTransactionNumber++);
        }

        if (noErrorsOnCreateQuote) {
            sendSTFUpdates();
        }
        return noErrorsOnCreateQuote;
    }

    bool canSendQuotes() {
        return isEnabled() & _canSendQuotes;
    }

    bool isContractQuoteExternal(uint32_t contractId) const noexcept override {
        return _contractQuoteExternal.getValue(contractId);
    }

    bool canQuoteExternal(uint32_t instrumentIndex, hf::VENUE venue) const {
        return _contractQuoteExternal.getValue(instrumentIndex) & canQuoteExternally()
            & s_systemState._exchQuoteExternally.load(std::memory_order_relaxed).test(static_cast<uint8_t>(venue));
    }

    void sendQuoteOrder(RvContract& contract, bool quoteExternal, bool unblockedByHedge = false) {
        if (canSendQuotes() || unblockedByHedge) {
            Quote& bidQuote = contract._quotes[BID];
            Quote& askQuote = contract._quotes[ASK];

            bool hasChange = false;
            auto& lastRequest = _lastOrderRequests[contract._internalId];
            if (bidQuote._hasChange) {
                bidQuote._hasChange = false;
                hasChange = true;
                lastRequest._quote[BID] = _orderRequestMgr.sendQuoteRequest(
                        contract._internalId,
                        orderId(StrategyId::RelativeValue, Order::Type::Quoting), BID,
                        bidQuote._bestPrice, bidQuote._bestSize, bidQuote._externalPrice, bidQuote._externalSize,
                        quoteExternal);
            }

            if (askQuote._hasChange) {
                askQuote._hasChange = false;
                hasChange = true;
                lastRequest._quote[ASK] = _orderRequestMgr.sendQuoteRequest(
                        contract._internalId,
                        orderId(StrategyId::RelativeValue, Order::Type::Quoting), ASK,
                        askQuote._bestPrice, askQuote._bestSize, askQuote._externalPrice, askQuote._externalSize,
                        quoteExternal);
            }

            if (hasChange || unblockedByHedge) {
                _orderRequestMgr.notifyOrderRequest(contract._internalId, StrategyId::RelativeValue, lastRequest);
                if (!canSendQuotes()) {
                    LOG(0, "RV {C[i1]}. Unblocked Quote due to hedge order update.", getContract(contract._internalId))
                }
            }
        } else {
            TLOG(0, "RV {C[i1]}. Quote blocked by IME transaction.", getContract(contract._internalId))
        }
    }

    template<uint32_t Side>
    void calculateQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double maxQuoteSize, bool isExternal) {
        const double hedgeRatio = _hedgeRatios.getValue(contract._internalId, leanIndex);
        hf::price_t price = calculateQuote<Side, true>(leanMarket, contract, leanIndex, leanFair, fair, width, hedgeRatio, isExternal);
        if(price != hf::NULL_PRICE) {
            const double size = calculateSize(contract, leanIndex, Side, hedgeRatio, leanMarket.getAdjustedSize(), maxQuoteSize);
            contract._quotes[Side]._leans.add(leanMarket, leanIndex, leanMarket.getId(), price, size, isExternal);
        }
    }

    template<uint32_t Side, bool ShouldCapNegativeCostToPctWidth>
    hf::price_t calculateQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double hedgeRatio, bool isExternal) {
        if ((leanMarket.getPrice() != hf::NULL_PRICE && !IsZero(hedgeRatio))    // on hedge path: must have price
            || (IsZero(hedgeRatio) && isExternal)) {                            // on no hedge path: must external
            RvContract& leanContract = _contracts[leanIndex];
            const hf::price_t bidHedgingCost = calculateHedgeCost<Side>(leanMarket, leanFair);
            const hf::price_t hedgingCost = calculateHedgingCost<ShouldCapNegativeCostToPctWidth>(width, bidHedgingCost, hedgeRatio, contract, leanContract);
            const hf::price_t multipliedCommission = static_cast<hf::price_t>(_commissions.getValue(contract._definition._internalId, leanIndex) / contract._pricingMultiplier);
            return calculateQuotePrice<Side>(fair, width, hedgingCost, multipliedCommission);
        }
        return hf::NULL_PRICE;
    }

    template<bool ShouldCapNegativeCostToPctWidth = true>
    hf::price_t calculateHedgingCost(hf::price_t width, hf::price_t cost, double hedgeRatio, RvContract& contract, RvContract& leanContract) {
        const hf::price_t hedgingCost = static_cast<hf::price_t>(cost * hedgeRatio * _costMultiplierMatrix.getValue(leanContract._internalId, contract._internalId));
        if (ShouldCapNegativeCostToPctWidth && (hedgingCost < 0)) {
            const hf::price_t maxHedgingCost = static_cast<hf::price_t>(0.95 * width);
            if (-hedgingCost > maxHedgingCost) {
                return -maxHedgingCost;
            }
        }
        return hedgingCost;
    }

    double calculateSize(RvContract& contract, uint32_t leanIndex, uint32_t side, double hedgeRatio, double marketSize, double maxQuoteSize) {
        double size = 0;
        if (hedgeRatio > 0) {
            const double possibleSize = marketSize / hedgeRatio;
            if (possibleSize > maxQuoteSize) {
                size = maxQuoteSize;
            } else {
                size = possibleSize;
            }
            // LOG(0, "Calculated lean size. Contract={C[i1]} Lean={C[i2]} Side={i3} Market={d1} Max={d2} PossibleSize={d3} Size={d4} HedgeRatio={d5}", getContract(contract._internalId), getContract(leanIndex), side, marketSize, maxQuoteSize, possibleSize, size, hedgeRatio)
        } else {
            // LOG(0, "Calculated lean size. Contract={C[i1]} Lean={C[i2]} Side={i3} Market={d1} Max={d2} Size={d3} HedgeRatio is not positive. Ratio={d4}", getContract(contract._internalId), getContract(leanIndex), side, marketSize, maxQuoteSize, size, hedgeRatio)
            size = maxQuoteSize;
        }
        return size;
    }

    void logCurrentExternalMarkets() {
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            RvContract& contract = _contracts[spreadIndex];
            logCurrentExternalMarket(contract);
        }
    }

    void logCurrentExternalMarket(RvContract& contract) {
        hf::price_t bidPrice = 0;
        hf::qty_t bidUnadjustedSize = 0;
        hf::qty_t bidAdjustedSize = 0;
        Market& bid = contract.externalMarket<BID>();
        if (bid.getPrice() != hf::NULL_PRICE) {
            bidPrice = bid.getPrice();
            bidUnadjustedSize = bid.getUnAdjustedSize();
            bidAdjustedSize = bid.getAdjustedSize();
        }
        hf::price_t askPrice = 0;
        hf::qty_t askUnadjustedSize = 0;
        hf::qty_t askAdjustedSize = 0;
        Market& offer = contract.externalMarket<ASK>();
        if (offer.getPrice() != hf::NULL_PRICE) {
            askPrice = offer.getPrice();
            askAdjustedSize = offer.getAdjustedSize();
            askUnadjustedSize = offer.getUnAdjustedSize();
        }
        LOG(0, "Market: Contract={C[i1]} Bid: {i2}/{i3}@{d1} Offer: {i4}/{i5}@{d2}", getContract(contract._internalId), bidAdjustedSize, bidUnadjustedSize, ToDollars(bidPrice), askAdjustedSize, askUnadjustedSize, ToDollars(askPrice))
    }

    void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size, hf::price_t price, hf::price_t stf) {
        sendMessageToFront<AcceptedManualFillMessage>(ViperMessageType::AcceptedManualFill, [&](AcceptedManualFillMessage& msg) {
           msg._contractId = internalId;
           msg._isInternal = false;
           msg._size = size;
           msg._price = price;
           msg._stf = stf;
        });
    }

    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&, WindowIds)>&& send, std::function<bool(WindowIds)>&& hasSubscription) const override;
    void handleViperMessage(const ViperMessage& message);

    proto::GridUpdate getQuoteSizeUpdate() const;
    proto::GridUpdate getHedgeRatiosUpdate() const;
    proto::GridUpdate getFactorHedgedUpdate() const;
    proto::GridUpdate getExtraContractWidth_PctStfMove_MaxTicksThroughMarket_Update() const;
    proto::GridUpdate getCommissionUpdate() const;
    proto::GridUpdate getParametersUpdate() const;
    proto::GridUpdate getVectorParametersUpdate() const;

private:
    void populateParametersGrid(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void populateVectorParameters(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void populateVectorParameterCells(::google::protobuf::RepeatedPtrField< ::Python::proto::GridCell >* cells, uint32_t contractId, bool checkForCross) const;
    void populatePricingMatrix(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void populateRiskMatrix(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void populateHedgeRatios(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void populateFactorHedged(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void populateQuoteSizes(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void populateCommissions(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const;
    void onShiftUpdate(uint32_t shift);
    void onEconomicNumberEnded(EconomicNumber::Color color);
    void handleSTFSubscription(uint32_t strategyId, uint32_t contractId);
public:
    Matrix _pricingMatrix; // x,y - vectors - square
    Matrix _riskMatrix; // x,y - vectors - square
    Matrix _risk_X_qtyMultiplier_Matrix;
    Matrix _risk_Div_pricingMultiplier_Matrix;
    Matrix _vectorRiskMatrix;
    Matrix _vectorRisk_Div_qtyMultiplier_Matrix;
    Matrix _costMultiplierMatrix;
    uint32_t _numberOfVectors;

    Vector<double> _vectorsRange;
    Vector<double> _vectorsMaxPosition;
    Vector<double> _fareMoveForOneUnitPosition;
    Vector<bool> _useWeightedMid;
    Vector<double> _weights;
    Vector<hf::price_t> _contractWidths;
    Vector<hf::price_t> _extraContractWidths;
    Vector<double> _pctStfMoves;

    SimpleMatrix<double> _hedgeRatios; // x,y - outrights, spreads - square
    SimpleMatrix<int> _factorHedgedIndexes; // x,y - outrights, spreads - square
    Matrix _quoteSizes; // x,y - outrights, spreads - square

    std::vector<RvContract> _contracts; // outrights, spreads
    uint32_t _numberOfAllContracts;
    uint32_t _numberOfPricingContracts;
    uint32_t _indexOfFirstTradedContract;

    Vector<double> _contractPositions; // outrights
    Vector<double> _positionsInVector; // vectors
    Vector<bool> _factorHedged;

    std::vector<std::string> _vectorNames;

    SimpleMatrix<bool> _leanMatrix;
    Vector<bool> _contractQuoteExternal;
    Vector<bool> _staticQuote;
    Vector<uint32_t> _staticQuoteSize;
    SimpleMatrix<hf::price_t> _commissions; // row, col - all contracts, all contracts
    Vector<double> _commissionsInput;
    Vector<hf::price_t> _maxTicksThroughMarket;
    Vector<hf::price_t> _maxTicksWide;

    struct ShortTermFairs {
        hf::price_t _fair = 0;
        bool _subscribed = false;
        bool _firstUpdateSent = false;
    };
    Vector<ShortTermFairs> _shortTermFairsForContracts;
    Vector<hf::price_t> _shortTermFairsCache;
    std::unordered_map<uint32_t, ShortTermFairs*> _shortTermFairUpdate;
    Vector<double> _shortTermFairsInVector;
    Vector<double> _shortTermFairsMoveInVector;
    Vector<double> _shortTermFairsMoveForContract;
    Vector<double> _longTermFairsInVector;
    bool _hasDataForCharts = false;

    Matrix _ODM;
    Matrix _midPoints;
    Matrix _overdeterminedPrices;
    Vector<double> _vectorMarketPrices[2];
    Vector<double>* _priorVectorMarketPrices;
    Vector<double>* _currentVectorMarketPrices;
    Vector<int> _rhoneMaxClipSize;
    bool _calculatedVectorPrices = false;
    bool _canCalculateEma = false;

    Vector<bool> _useEMAInVector;
    Vector<bool> _setToMarket;
    Vector<uint64_t> _emaPeriodInVector;
    Vector<uint64_t> _emaOnTime;
    Vector<uint64_t> _emaOffTime;
    Vector<double> _ltfDelta;
    uint64_t _lastEmaTime;

    double _widthMultiplier = 0;

private:
    std::unordered_set<uint32_t> _updatedContracts;
    UpdateSelector<RelativeValue> _updateSelector;

    bool _canSendQuotes = true;
    bool _endOfQuotes = false;
    bool _calculateDueToFront = false;
    bool _hadInternalMarketData = false;
    bool _hadExternalMarketData = false;
    uint32_t _numberOfFills = 0;
    uint32_t _skipClubCount = 0;

    om::client_id_t _id;

    std::unordered_map<uint32_t, double> _hedgeRequests;
    uint32_t _hedgeId = 0;
    bool _withinInternalFillsOfCycle = false;
    bool _hasPendingQuoteCalculation = false;
    int64_t _fillsTransactionNumber = 0;
    //We will set this if unset on a fill, and reset it after processing hedges
    LatencyTrackingFields _latencyTracking;

    OrderRequestManager& _orderRequestMgr;
    std::vector<ReadyOrderRequests> _lastOrderRequests;

    // ********** TEMP working structures TEMP *********** //
    Matrix _possibleHedgeRatios; // row, col - vectors, 3
    Matrix _hedgeVectorRiskExposures; // row, col - vectors, vectors
    Vector<double> _hedgeVectorRiskExposuresSums; // vectors
    Matrix _normalizationMatrix; // row, col - outrights, 3
    // *************************************************** //

};

#undef NumberOfMarkets

}

#endif //VIPER_RELATIVE_VALUE_H
