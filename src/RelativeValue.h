#ifndef VIPER_RELATIVE_VALUE_H
#define VIPER_RELATIVE_VALUE_H

#include <boost/format.hpp>
#include <md/md.h>
#include <functional>
#include <limits>
#include <set>
#include <hf/ConfigManager.h>
#include "Common.h"
#include "ViperConfig.h"
#include "MathStructures.h"
#include "ClockUtil.h"
#include "UpdateSelector.h"
#include "logging/Coolog.h"
#include "ViperStrategyBase.h"
#include "OrderRequestManager.h"
#include "MarketDataConsumer.h"

namespace Python {

struct Lean {
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
    bool _isExternal = false;
    hf::price_t _price;
    double _size;
};


template<uint32_t Side>
struct LeanSorter {
    bool isBetterPrice(hf::price_t p1, hf::price_t p2);

    bool operator()(Lean& a, Lean& b) const {
        auto aValid = a._size >= 1.0;
        auto bValid = b._size >= 1.0;
        double priceFactor = (Side == BID) ? 1.0 : -1.0;
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

    void add(uint32_t leanIndex, uint32_t marketIndex, hf::price_t price, double size, bool isExternal) {
        Lean& lean = _leans[_leanIndex];
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

struct ContractDefinition {

    struct Leg {
        int32_t _size = 0;
        uint32_t _index = 0;
    };

    bool _usedInPricing = false;
    hf::symbol_id_t _sid = 0;
    hf::VENUE _venue = hf::VENUE::UNKNOWN;
    hf::price_t _tickSize = 0;
    double _pricingMultiplier;
    double _quantityMultiplier;
    uint32_t _internalId;
    std::string _name;

    uint32_t _numberOfLegs = 0;
    Leg _legs[10];
};

struct Quote {
    Leans _leans;

    hf::price_t _bestPrice = hf::NULL_PRICE;
    hf::qty_t _bestSize = 0;
    uint32_t _bestLeanIndex;

    hf::price_t _externalPrice = hf::NULL_PRICE;
    hf::qty_t _externalSize = 0;

    uint32_t _bestLean[2] = {0, 0};

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
            _bestSize = static_cast<hf::qty_t>(bestLean._size + 0.0001);
            _bestLeanIndex = bestLean._instrumentIndex;

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
                _externalSize = staticQuote ? quoteSize : std::max(_bestSize, static_cast<hf::qty_t>(clubSize / clubCount + 0.0001));
            }
            else {
                _externalPrice = hf::NULL_PRICE;
                _externalSize = 0;
                if(BOOST_LIKELY(isEnabled)) {
                    const auto &validationResult = validateMaxTicksThroughMarketFunc(bestLean._price, bestLean._instrumentIndex);
                    if (BOOST_UNLIKELY(!validationResult.isValid)) {
                        logValidateMaxTicksThroughMarketError(side, contractId, validationResult, bestLean, contracts, logErrorFunc);
                        return false;
                    }
                }
            }

            Lean& lean = _leans._leans[0];
            auto& leanContract = contracts[lean._instrumentIndex];
            auto& market = lean._isExternal ? leanContract._externalMarket[side] : leanContract._internalMarkets[side].getMarket(lean._marketIndex);
            auto& contract = contracts[contractId];
            bool internalPriceChanged = std::abs(oldBestPrice - _bestPrice) > contract._priceChangeThreshold;
            bool externalPriceChanged = std::abs(oldExtPrice - _externalPrice) > contract._priceChangeThreshold;

            if (oldBestSize != _bestSize || oldExtSize != _externalSize || _bestLean[side] != lean._instrumentIndex ||
                internalPriceChanged || externalPriceChanged) {
                LOG(0, "RV {C[i1]}. Quote. Side={i2} Internal: {i3}@{d1} External: {i4}@{d2} Lean={C[i5]} {i6}@{d3} MarketId={i7}",
                    getContract(contractId, contracts), side, _bestSize, ToDollars(_bestPrice), _externalSize, ToDollars(_externalPrice), getContract(lean._instrumentIndex, contracts),
                    market.getFakAdjustedSize(), ToDollars(market.getPrice()), lean._marketIndex)
            }

            _bestLean[side] = lean._instrumentIndex;
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

            LOG(0, "Quote Violated MaxTicksThroughMarket.  Contract={C[i1]} Side={i2} Lean={C[i3]} InternalQuote={i4}@{d1} ExternalQuote={i5}@{d2} HedgeMarket(ext:0;intmm:1;inthedge:2)={i6} quoteWithoutCap=({d3};{d4};{d5}) leanMarket(ss)=({d6};{d7};{d8})",
                getContract(contractId, contracts), side, getContract(lean._instrumentIndex, contracts), _bestSize, ToDollars(_bestPrice), _externalSize, ToDollars(_externalPrice),
                (lean._isExternal ? 0 : lean._marketIndex == orderId(StrategyId::MarketMaker, Order::Type::Hedging) ? 1 : 2), ToDollars(validationResult.externalMarketQuote),
                ToDollars(validationResult.internalMarketsQuotes[orderId(StrategyId::MarketMaker, Order::Type::Hedging)]), ToDollars(validationResult.internalMarketsQuotes[orderId(StrategyId::MarketMaker, Order::Type::Hedging)]),
                ToDollars(leanContract._externalMarket[side].getPrice()), ToDollars(leanContract._internalMarkets[side].getMarket(1).getPrice()), ToDollars(leanContract._internalMarkets[side].getMarket(0).getPrice()))
            LOG(0, "  ...QuoteViolatedMaxTicksThroughMarket (continues) quoteMarket(os)=({d1};{d2};{d3})",
                ToDollars(quoteContract._externalMarket[otherSide()].getPrice()), ToDollars(quoteContract._internalMarkets[otherSide()].getMarket(1).getPrice()),
                ToDollars(quoteContract._internalMarkets[otherSide()].getMarket(0).getPrice()))

            //We really need a "log both" method . . .
            boost::format firstFormatString("Quote Violated MaxTicksThroughMarket.  Contract=%1% Side=%2% Lean=%3% rvExternalQuote=%4%@%5% rvInternalQuote=%6%@%7% HedgeMarket(ext:0,intmm:1,inthedge:2)=%8%");
            logErrorFunc(boost::str(firstFormatString
                                    % contracts[contractId]._definition._name % side % contracts[lean._instrumentIndex]._definition._name
                                    %  _externalSize % ToDollars(_externalPrice) % _bestSize % ToDollars(_bestPrice)
                                    % (lean._isExternal ? 0 : lean._marketIndex == orderId(StrategyId::MarketMaker, Order::Type::Hedging) ? 1 : 2)));

            boost::format secondFormatString("    rvQuoteWithoutCap=(%1%,%2%,%3%) quoteMarket=(%4%,%5%,%6%) leanMarket=(%7%,%8%,%9%)");
            logErrorFunc(boost::str(secondFormatString % ToDollars(validationResult.externalMarketQuote)
                                    % ToDollars(validationResult.internalMarketsQuotes[orderId(StrategyId::MarketMaker, Order::Type::Hedging)]) % ToDollars(validationResult.internalMarketsQuotes[orderId(StrategyId::MarketMaker, Order::Type::Hedging)])
                                    % ToDollars(quoteContract._externalMarket[otherSide()].getPrice()) % ToDollars(quoteContract._internalMarkets[otherSide()].getMarket(1).getPrice())
                                    % ToDollars(quoteContract._internalMarkets[otherSide()].getMarket(0).getPrice())
                                    % ToDollars(leanContract._externalMarket[side].getPrice()) % ToDollars(leanContract._internalMarkets[side].getMarket(1).getPrice())
                                    % ToDollars(leanContract._internalMarkets[side].getMarket(0).getPrice())));
        }
};

struct RvContract {
    ContractDefinition _definition;
    Markets _internalMarkets[2];
    Market _externalMarket[2];
    Quote _quotes[2];

    hf::qty_t _establishedLevel;

    void clearLeans() {
        _quotes[BID]._leans.clear();
        _quotes[ASK]._leans.clear();
    }

    bool _canTradeStatus = false;
    bool _traded = false;
    uint32_t _internalId;
    double _priceChangeThreshold;

    void setIdsForMarkets(uint32_t id) {
        _externalMarket[BID].setContractId(id);
        _externalMarket[ASK].setContractId(id);
        _internalMarkets[BID].setContractId(id);
        _internalMarkets[ASK].setContractId(id);
    }
};

class RelativeValue : public ViperStrategyBaseImpl<RelativeValue> {

public:
    RelativeValue()
        : _updateSelector(*this)
        , _orderRequestMgr(OrderRequestManager::get())
    {
    }

    ResourceId id() const noexcept override { return ResourceId::RelativeValue; }
    StrategyId strategyId() const noexcept override { return StrategyId::RelativeValue; }

    void registerOrderClient() {
        for (uint32_t contractIndex = _indexOfFirstTradedContract; contractIndex < _numberOfAllContracts; ++contractIndex) {
            RvContract& contract = _contracts[contractIndex];
            sendRegisterOrderClientRequest(contract._internalId, {StrategyId::MarketMaker});
        }
    }

    void runCycle(uint64_t now) {
        if (_numberOfFills > 0) {
            LOG(0, "RV drained number of fills. {i1}", _numberOfFills)
        }

        const bool calculateDueToHedges = processHedgeRequests();
        if (_hadExternalMarketData) {
            recalculateAfterExternalMarketDataChange();
        }

        _canSendQuotes = _hedgeRequests.empty() & !_withinInternalFillsOfCycle;
        const bool recalculateQuotes = (canActOnMarketData() & (_calculateDueToFront | _hadInternalMarketData  | _hadExternalMarketData))
                | calculateDueToHedges | checkEMAOnOffTime(now);
        if (recalculateQuotes) {
            _hasPendingQuoteCalculation = _withinInternalFillsOfCycle;
            calculateQuotes();
        } else if (_endOfQuotes & _hasPendingQuoteCalculation){
            _hasPendingQuoteCalculation = false;
            calculateQuotes();
        }

        // reset flags
        _calculateDueToFront = false;
        _hadInternalMarketData = false;
        _hadExternalMarketData = false;
        _numberOfFills = 0;
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

    bool processHedgeRequests() {
        const bool hasHedgeRequests = !_hedgeRequests.empty();
        if (hasHedgeRequests) {
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
                    addPosition(contract, std::abs(requestSize), requestSide, true, false);
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
                    addPosition(contract, hedgedSize, requestSide, true, false);
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
            uint32_t vectorIndex = (uint32_t) vectorIndexSigned;
            if (_factorHedged.getValue(vectorIndex)) {
                double vectorRisk = 0;
                if (contract._definition._numberOfLegs > 1) {
                    for (uint32_t legIndex = 0; legIndex < contract._definition._numberOfLegs; ++legIndex) {
                        ContractDefinition::Leg& leg = contract._definition._legs[legIndex];
                        vectorRisk += _riskMatrix.getValue(vectorIndex, leg._index) * leg._size;
                    }
                } else {
                    vectorRisk = _riskMatrix.getValue(vectorIndex, contractId);
                }
                vectorRisk = Round<2>(vectorRisk);
                if (!IsZero(vectorRisk)) {
                    double vectorPos = _positionsInVector.getValue(vectorIndex);
                    position += vectorPos / vectorRisk / contract._definition._quantityMultiplier;
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
            if (contract._definition._usedInPricing) {
                const bool missingBook = (contract._externalMarket[BID].getPrice() == hf::NULL_PRICE) | (contract._externalMarket[ASK].getPrice() == hf::NULL_PRICE);
                if (missingBook) {
                    hasMarketData = false;
                    sendErrorMessage(boost::str(boost::format("RV - Does not have market data for. Contract=%1%") % contract._definition._name), ErrorType::Critical);
                    LOG(0, "RV - Does not have market data for {C[i1]}", getContract(contractIndex))
                    break;
                }
                if((contract._externalMarket[BID].getFakAdjustedSize() < 0) || (contract._externalMarket[BID].getUnAdjustedSize() < 0)
                   || (contract._externalMarket[ASK].getFakAdjustedSize() < 0) || (contract._externalMarket[ASK].getUnAdjustedSize() < 0)) {
                    hasMarketData = false;
                    std::string errorMessageForGUI = boost::str(boost::format("Contract %1% has bad size.  BidUA=%2% AskUA=%3% BidAdj=%4% AskAdj=%5%")
                            % contract._definition._name % contract._externalMarket[BID].getUnAdjustedSize() % contract._externalMarket[ASK].getUnAdjustedSize()
                            % contract._externalMarket[BID].getFakAdjustedSize() % contract._externalMarket[ASK].getFakAdjustedSize());
                    sendErrorMessage(errorMessageForGUI, ErrorType::Critical);
                    LOG(0, "RV - Invalid quantities for {C[i1]} - bUA={i2} aUA={i3} bA={i4} aA={i5}", getContract(contractIndex), contract._externalMarket[BID].getUnAdjustedSize(),
                            contract._externalMarket[ASK].getUnAdjustedSize(), contract._externalMarket[BID].getFakAdjustedSize(), contract._externalMarket[ASK].getFakAdjustedSize());
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
        if (_maxTicksWide.getValue(contract._definition._internalId) != std::numeric_limits<int64_t>::max() &&
            (contract._externalMarket[ASK].getPrice() == hf::NULL_PRICE ||
            contract._externalMarket[BID].getPrice() == hf::NULL_PRICE)) {
            return false;
        }
        return (contract._externalMarket[ASK].getPrice() - contract._externalMarket[BID].getPrice()) <= _maxTicksWide.getValue(contract._definition._internalId);
    }

    bool isWithinPositionLimits() {
        bool withinLimits = true;
        for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
            const double vectorPosition = _positionsInVector.getValue(vectorIndex);
            const double limit = _vectorsMaxPosition.getValue(vectorIndex);
            if (std::abs(vectorPosition) > limit) {
                sendErrorMessage(boost::str(boost::format(
                        "RV - not within vector position limits. Vector=%1%, Position=%2%, Limit=%3%")
                        % getVectorName(vectorIndex) % vectorPosition % limit), ErrorType::Critical);
                LOG(0, "Vector position not within limits. Vector={V[i1]} Position={d1} Limit={d2}", getVectorName(vectorIndex), vectorPosition, limit)
                withinLimits = false;
            }
        }
        return withinLimits;
    }

    void doInitialCalculations() {
        logVectorPositions(false);
        logContractPositions(false);
        calculateAllMidPoints();
        calculateOverdeterminedPrices();
        calculateVectorMarketPrices();
        calculateLongTermFairsInVector();
        calculateShortTermFairs();
        calculateQuotes();
        validateRhoneRiskLimitsExist();
    }

    void calculateAllMidPoints() {
        for (uint32_t i = 0; i < _numberOfAllContracts; ++i) {
            double weight = _weights.getValue(i);
            if (!IsZero(weight)) {
                RvContract& contract = _contracts[i];
                Market& bid = contract._externalMarket[BID];
                Market& ask = contract._externalMarket[ASK];
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
        assert(message._marketIndex < 2);
        RvContract& contract = _contracts[message._instrumentIndex];
        Market& market = contract._internalMarkets[message._side]._markets[message._marketIndex];
        market.handleInternalHedgeDone(message._hedgeId);
    }

    void handleHedgeMarketDataAdjustmentAck(const HedgeMarketDataAdjustmentAck& hedgeAck) {
        RvContract& contract = _contracts[hedgeAck._instrumentIndex];
        LOG(0, "RV {C[i1]}. Received hedge md ack. HedgeId={i2} Adj={i3} OrderId={i4}", getContract(contract._internalId), hedgeAck._hedgeId, hedgeAck._size, (uint32_t) hedgeAck._orderId)
        Market& market = contract._externalMarket[hedgeAck._side];
        market.ackAdjustment(hedgeAck._hedgeId, hedgeAck._size, hedgeAck._orderId);
    }

    void handleFill(const FillMessage& fill) {
        const hf::qty_t fillSize = fill._side == BID ? fill._qty : -fill._qty;
        if (isEnabled()) {
            const_cast<FillMessage&>(fill)._rvStf = _shortTermFairsForContracts.getValue(fill._contractId);
            sendFillToFront(fill);
            RvContract& contract = _contracts[fill._contractId];
            LOG(0, "RV {C[i1]}. got filled. Side={i2} Price={d1} Qty={i3}", getContract(contract._internalId), fill._side, ToDollars(fill._price), fill._qty);
            auto hedgeIt = _hedgeRequests.find(fill._contractId);
            if (hedgeIt != _hedgeRequests.end()) {
                hedgeIt->second += fillSize;
            } else {
                _hedgeRequests.insert({fill._contractId, fillSize});
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

        bool foundation = false;
        bool longTermFairs = false;
        bool shortTermFairs = false;
        std::vector<uint32_t> checkEMA;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            VectorParametersColumns columnType = static_cast<VectorParametersColumns>(gridCell.col());
            uint32_t vectorId = gridCell.row();
            switch (columnType) {
                case VectorParametersColumns::Range: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    if (isChangeWithinStfLimits(_vectorsRange.getValue(vectorId), value, _vectorsMaxPosition.getValue(vectorId), _longTermFairsInVector.getValue(vectorId), vectorId)) {
                        _vectorsRange.setValue(gridCell.row(), value);
                        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
                        foundation = true;
                        shortTermFairs = true;
                        LOG(0, "RV range changed. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                    } else {
                        LOG(0, "RV could not change range. Change would be over 5% of range. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                        sendErrorMessage(boost::str(boost::format("RV could not change range. Change would be over 5 percent of range. Range=%1% Factor=%2%") % value % _vectorNames[vectorId]), ErrorType::Critical);
                        gridCell.set_value(std::to_string(_vectorsRange.getValue(vectorId)));
                    }
                    break;
                }
                case VectorParametersColumns::MaxPos: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    if (isChangeWithinStfLimits(_vectorsRange.getValue(vectorId), _vectorsRange.getValue(vectorId), value, _longTermFairsInVector.getValue(vectorId), vectorId)) {
                        _vectorsMaxPosition.setValue(gridCell.row(), value);
                        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
                        foundation = true;
                        shortTermFairs = true;
                        LOG(0, "RV max_pos changed. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                    } else {
                        LOG(0, "RV could not change max_pos. Change would be over 5% of range. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
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
                        sendGenericMessageToFront(ViperMessageType::StoreLtf, gridCell.row());
                    }
                    break;
                }
                case VectorParametersColumns::EMA_Time: {
                    uint32_t value = boost::lexical_cast<uint32_t>(gridCell.value());
                    uint64_t periodInCycles = GetCyclesFromSeconds(value, _frequency);
                    _emaPeriodInVector.setValue(gridCell.row(), periodInCycles);
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
                    double value = boost::lexical_cast<double>(gridCell.value());
                    if (isChangeWithinStfLimits(_vectorsRange.getValue(vectorId), _vectorsRange.getValue(vectorId), _vectorsMaxPosition.getValue(vectorId), value, vectorId) && !_useEMAInVector.getValue(vectorId)) {
                        _longTermFairsInVector.setValue(gridCell.row(), value);
                        shortTermFairs = true;
                        LOG(0, "RV ltf changed. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                    } else {
                        LOG(0, "RV could not change ltf. Change would be over 5% of range. Vector={V[i1]} Value={d1}", getVectorName(vectorId), value)
                        sendErrorMessage(boost::str(boost::format("RV could not change LTF. Change would be over 5 percent of range. LTF=%1% Factor=%2%") % value % _vectorNames[vectorId]), ErrorType::Critical);
                        gridCell.set_value(std::to_string(_longTermFairsInVector.getValue(vectorId)));
                    }
                    break;
                }
                case VectorParametersColumns::SetToMarket: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _setToMarket.setValue(gridCell.row(), value);
                    shortTermFairs = true;
                    LOG(0, "RV set_to_market changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), value)
                    break;
                }
                case VectorParametersColumns::Hedged: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _factorHedged.setValue(gridCell.row(), value);
                    LOG(0, "RV hedged changed. Vector={V[i1]} Value={i2}", getVectorName(vectorId), value)
                    break;
                }
                default:
                    break;
            }
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
                    _maxTicksThroughMarket.setValue(gridCell.row(), value);
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
                    gridCell->set_value(std::to_string(_maxTicksThroughMarket.getValue(gridCell->row())));
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
                    auto oldUsedInPricing = contract._definition._usedInPricing;
                    contract._definition._usedInPricing = !IsZero(weight);
                    if (contract._definition._usedInPricing || oldUsedInPricing != contract._definition._usedInPricing) {
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
                case PricingConfigsColumns::SweepTicks: {
                    auto& ref = _sweepDefinition.getRef(gridCell.row());
                    uint32_t gridValue = boost::lexical_cast<uint32_t>(gridCell.value());
                    ref._sweepTicksAtPrice = static_cast<hf::price_t>(gridValue) * contract._definition._tickSize;
                    LOG(0, "RV sweepDefinition changed.  Contract={C[i1]} ExternalValue={i2} InTicks={i3}", getContract(contract._internalId), gridValue, ref._sweepTicksAtPrice)
                    break;
                }
                case PricingConfigsColumns::SweepQty: {
                    auto& ref = _sweepDefinition.getRef(gridCell.row());
                    ref._tradedQty = boost::lexical_cast<int32_t>(gridCell.value());
                    LOG(0, "RV sweepQty changed.  Contract={C[i1]} Value={i2}", getContract(contract._internalId), ref._tradedQty);
                    break;
                }
                case PricingConfigsColumns::PricingMultiplier:
                case PricingConfigsColumns::QuantityMultiplier:
                    break;
            }
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
                    contract._definition._usedInPricing = !IsZero(kvp.second);
                }
            }
        }

        for (int rowIndex : needToUpdateMidPoint) {
            RvContract& contract = _contracts[rowIndex];
            updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], rowIndex, contract._establishedLevel);
        }

        calculateOverdeterminedPrices();
        calculateVectorMarketPrices();
        calculateLongTermFairsInVector();
        calculateShortTermFairs();
        calculateQuotes();

        return true;
    }

    //Must be thread safe if verbose == false
    bool validateRvPricingConfigUpdate(GridUpdate& update, bool verbose = true) {
        bool errorsOccured = false;
        for(int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            PricingConfigsColumns columnType = static_cast<PricingConfigsColumns>(gridCell.col());
            switch (columnType) {
                case PricingConfigsColumns::SweepTicks: {
                    bool errorOccuredInThisCheck = false;
                    try {
                        int32_t value = boost::lexical_cast<int32_t>(gridCell.value());
                        if (value <= 0) {
                            if (verbose) { sendErrorMessage("PricingConfigsColumn::SweepTicks <= 0", ErrorType::Critical); }
                            errorsOccured = errorOccuredInThisCheck = true;
                        }
                        RvContract &contract = _contracts[gridCell.row()];
                        uint64_t maxValue = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) / contract._definition._tickSize;
                        if (static_cast<uint64_t>(value) > maxValue) {
                            if (verbose) { sendErrorMessage("PricingConfigsColumn::SweepTicks too big.  value=" + std::to_string(value), ErrorType::Critical); }
                            errorsOccured = errorOccuredInThisCheck = true;
                        }
                    }
                    catch(const boost::bad_lexical_cast& blc) {
                        if(verbose) { sendErrorMessage("Error parsing SweepTicks value", ErrorType::Critical); }
                        errorsOccured = errorOccuredInThisCheck = true;
                    }

                    if(errorOccuredInThisCheck && verbose) {
                        const auto& def = _sweepDefinition.getRef(gridCell.row());
                        gridCell.set_value(std::to_string(def._sweepTicksAtPrice / _contracts[gridCell.row()]._definition._tickSize));
                    }
                    break;
                }
                case PricingConfigsColumns::SweepQty: {
                    bool errorOccuredInThisCheck = false;
                    try {
                        if (boost::lexical_cast<int32_t>(gridCell.value()) < 0) {
                            if (verbose) { sendErrorMessage("PricingConfigsColumn::SweepQty < 0", ErrorType::Critical); }
                            errorsOccured = errorOccuredInThisCheck = true;
                        }
                    }
                    catch(const boost::bad_lexical_cast& blc) {
                        if(verbose) { sendErrorMessage("Error parsing SweepQty value", ErrorType::Critical); }
                        errorsOccured = errorOccuredInThisCheck = true;
                    }

                    if(errorOccuredInThisCheck && verbose) {
                        gridCell.set_value(std::to_string(_sweepDefinition.getRef(gridCell.row())._tradedQty));
                    }
                    break;
                }
                default:
                    break;
            }
        }
        return !errorsOccured;
    }

    void onManualFill(ManualFill &fill) {
        if (fill._type == toUInt(ManualFillType::RelativeValue)) {
            RvContract& contract = _contracts[fill._contract];
            uint32_t size = std::abs(fill._size);
            uint32_t side = fill._size > 0 ? BID : ASK;
            addPosition(contract, size, side, false, false);
            sendAcceptedManualFillToFront(contract._internalId, fill._size);
        }
    }

    void onEndOfManualFills() {
        checkVectorPositionLimits(false);
        calculateShortTermFairs();
        calculateQuotes();
    }

    bool onMarketMakerParametersUpdate(GridUpdate& update) { return true; }
    bool onStackerParametersUpdate(GridUpdate& update) { return true; }
    bool onPnlParametersUpdate(GridUpdate& update) { return true; }

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
                    LOG(0, "Invalid width multiplier value {i1}", 0)
                    gridCell.set_value(config._shifts[toUInt(shift)]);
                    sendErrorMessage(boost::str(boost::format("Invalid width multiplier value %1%") % gridCell.value()), ErrorType::Critical);
                }
            }
        }

        if (quotes) {
            calculateQuotes();
            sendQuoteSizesToGUI();
        }

        return true;
    }

    bool isWidthMultiplierWithinLimits(double value) {
        return IsSame(value, 1.0) | ((value > 1) & (value <= 10.0));
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

    void handleUncommonMDEvent(const md::MDEvent &mde, const md::MDBatch& batch) {
        switch (mde.type) {
            case md::MDEvent::TYPE::FAK_DONE: {
                const auto &event = static_cast<const md::MDFakDonenEvent &> (mde);
                handleFakDone(batch.venue, event);
                break;
            }
            case md::MDEvent::TYPE::INTRA_EVENT_SWEEP: {
                const auto &event = static_cast<const md::MDIntraEventSweepEvent &> (mde);
                const uint32_t internalId = _viperConfig.getInternalId(event.sid);

                if(BOOST_UNLIKELY(internalId == INVALID_INTERNAL_ID)) {
                    LOG(0, "EXE Sweep detected in unknown product.  sid={i1} extSeqNum={i2} diff={i3} qty={i4}", event.sid, batch.extSequenceNumber, event.diff, event.qty);
                }
                else {
                    LOG(0, "EXE {C[i1]}. Sweep detected. extSeqNum={i2} diff={i3} qty={i4}", getContract(internalId), batch.extSequenceNumber, event.diff, event.qty);
                    const auto &sweepLimits = _sweepDefinition.getRef(internalId);
                    if (sweepLimits.sweepDetected(event)) {
                        if (BOOST_LIKELY(isEnabled())) {
                            LOG(0, "RV {C[i1]}. Sweep detected, disabling RV, disabling ExternalQuote. PriceLimit={i2} QtyLimit={i3} event.diff={i4} event.qty={i5}",
                                getContract(internalId), sweepLimits._sweepTicksAtPrice, sweepLimits._tradedQty, event.diff, event.qty)
                            disableRelativeValue("Sweep detected by RV (more info to follow)", true);
                            disableQuoteExternal();
                            sendErrorMessage("Sweep info: disabling RV, disabling RV external quoting.  Contract=" + getContractName(internalId) + " PriceLimit=" +
                                             std::to_string(sweepLimits._sweepTicksAtPrice) + " QtyLimit=" + std::to_string(sweepLimits._tradedQty) +
                                             " event.diff=" + std::to_string(event.diff) + " event.qty=" + std::to_string(event.qty), ErrorType::Critical);
                        }
                    } 
                }
                break;
            }
            default:
                break;
        }
    }

    void onEndMDBatch() {
        _hadExternalMarketData = true;
    }

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent &event) {
        if (venue == hf::VENUE::CME) {
            for (uint32_t i = 0; i < event.num; ++i) {
                auto& trade = event.trades[i];
                const uint32_t internalId = _viperConfig.getInternalId(event.sid);
                if (internalId != INVALID_INTERNAL_ID) {
                    if (trade.side == hf::SIDE::BUY) {
                        //LOG(0, "RV: Contract {C[i1]} Bid book adjustment due to trade. {i2}@{d1}", getContract(internalId), trade.q, ToDollars(trade.pr))
                        onMarketDataUpdate(internalId, true, trade.pr, trade.q, false, 0, 0);
                    } else if (trade.side == hf::SIDE::SELL) {
                        //LOG(0, "RV: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}", getContract(internalId), trade.q, ToDollars(trade.pr))
                        onMarketDataUpdate(internalId, false, 0, 0, true, trade.pr, trade.q);
                    }
                }
            }
        }
    }

    void onMDStatus(hf::VENUE venue, uint64_t extSeqNum, const md::MDStatusEvent& event, uint32_t internalId) {
        RvContract& contract = _contracts[internalId];
        switch (event.statusType) {
            case md::MDStatusEvent::TYPE::RECOVERING:
            case md::MDStatusEvent::TYPE::TIMEOUT:
            case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
                contract._canTradeStatus = false;
                if (contract._definition._usedInPricing) {
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
                    case hf::VENUE::CME:
                        handleCMEMDStatus(event, contract);
                        break;
                    case hf::VENUE::ICE:
                        handleICEMDStatus(event, contract);
                        break;
                    case hf::VENUE::EUREX:
                        handleEUREXMDStatus(event, contract);
                        break;
                    default:
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
                LOG(0, "RV {C[i1]}. market data - closed. Used in pricing={i2}", getContract(contract._internalId), contract._definition._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed. Used in pricing=%2%") % contract._definition._name % contract._definition._usedInPricing));
                break;
            }
            case 'E': {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - expired. Used in pricing={i2}", getContract(contract._internalId), contract._definition._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - expired. Used in pricing=%2%") % contract._definition._name % contract._definition._usedInPricing));
                break;
            }
            case '1': {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - entered pre-open. Used in pricing={i2}", getContract(contract._internalId), contract._definition._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - entered pre-open.  Used in pricing=%2%") % contract._definition._name % contract._definition._usedInPricing));
                break;
            }
            default:
                break;
        }
    }

    void handleCMEMDStatus(const md::MDStatusEvent& event, RvContract& contract) {
        switch (event.extStatus) {
            case 17: {
                contract._canTradeStatus = true;
                LOG(0, "RV {C[i1]}. market data - opened.", getContract(contract._internalId))
                break;
            }
            case 2: {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - halted. Used in pricing={i2}", getContract(contract._internalId), contract._definition._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - halted. Used in pricing=%2%.") % contract._definition._name % contract._definition._usedInPricing));
                break;
            }
            case 18:
            case 4: {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]} market data - closed({i2}). Used in pricing={i3}", getContract(contract._internalId), (uint32_t)(event.extStatus), contract._definition._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._definition._usedInPricing));
                break;
            }
            case 21: {
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]} market data - entered pre-open. Used in pricing={i2}", getContract(contract._internalId), (uint32_t)(event.extStatus), contract._definition._usedInPricing)
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - entered pre-open. Used in pricing=%2%") % contract._definition._name % contract._definition._usedInPricing));
                break;
            }
            default: {
                break;
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
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._definition._usedInPricing));
                break;
            case 2:   /* Halt */       FALLTHROUGH
            case 201: /* Restricted */ FALLTHROUGH
            case 216: /* Freeze */     FALLTHROUGH
            case 213: /* IPO Auction Freeze */ FALLTHROUGH
            case 214: /* Pre Call */ FALLTHROUGH
            case 215: /* Call */
                // These are states that as described we will handle as a freeze, but have never been seen in market data
            LOG(0, "RV Contract {C[i1]}. market data - seen rare MD state we're treating as a halt  extStatus={i2}", getContract(contract._internalId), event.extStatus);
                FALLTHROUGH;
            case 204: /* Opening Auction */ FALLTHROUGH
            case 206: /* Intraday Auction */ FALLTHROUGH
            case 208: /* Circuit Breaker Auction */ FALLTHROUGH
            case 210: /* Closing Auction */ FALLTHROUGH
            case 212: /* IPO Auction */
                contract._canTradeStatus = false;
                LOG(0, "RV {C[i1]}. market data - auction.  extStatus={i2}", getContract(contract._internalId), event.extStatus);
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - auction(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._definition._usedInPricing));
                break;
            case 205: /* Opening Auction Freeze */ FALLTHROUGH
            case 207: /* Intraday Auction Freeze */ FALLTHROUGH
            case 209: /* Circuit Breaker Auction Freeze */ FALLTHROUGH
            case 211: /* Closing Auction Freeze */
                //These states are similar to CME no-cancel, except instead of a cancel reject, the exchange will "hold" the cancel until the market re-opens and
                //cancel the order if it's not matched.  Note that this state is not guaranteed to occur before the market re-opens!
                contract._canTradeStatus  = false;
                LOG(0, "RV {C[i1]}. market data - Auction Freeze.  extStatus={i2}", getContract(contract._internalId), event.extStatus);
                disableRelativeValue(boost::str(boost::format("Contract %1% market data - auction freeze(%2%). Used in pricing=%3%.") % contract._definition._name % event.extStatus % contract._definition._usedInPricing));
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

    void handleFakDone(hf::VENUE venue, const md::MDFakDonenEvent& event) {
        LOG(0, "Received FAK done. {i1}", 0)
        const uint32_t internalId = _viperConfig.getInternalId(event.sid);
        if (internalId != INVALID_INTERNAL_ID) {
            RvContract& contract = _contracts[internalId];
            const uint32_t side = ToViperSide(event._side);
            Market& book = contract._externalMarket[side];
            book.handleFakDone(event._oid.value());
        }
    }

    void onMDUpdate(const md::MDUpdateEvent &event, uint32_t internalId) {
        const md::top_t::level_t& bid = event.buy.top.l[0];
        const md::top_t::level_t& ask = event.sell.top.l[0];

        RvContract& contract = _contracts[internalId];
        const Market& rvBid = contract._externalMarket[BID];
        const Market& rvAsk = contract._externalMarket[ASK];

        const bool bidChanged = (event.buy.updatedQtyNum == 1) && ((rvBid.getPrice() != bid.pr) || (rvBid.getUnAdjustedSize() != bid.q));
        const bool askChanged = (event.sell.updatedQtyNum == 1) && ((rvAsk.getPrice() != ask.pr) || (rvAsk.getUnAdjustedSize() != ask.q));

        onMarketDataUpdate(contract, internalId, bidChanged, bid.pr, bid.q, askChanged, ask.pr, ask.q);
    }

    void onMarketDataUpdate(RvContract& contract, uint32_t internalId, bool bidChanged, hf::price_t bidPrice, hf::qty_t bidSize, bool askChanged,  hf::price_t askPrice, hf::qty_t askSize) {
        BOOST_ASSERT(internalId == contract._internalId);
        if (bidChanged) {
            Market& bid = contract._externalMarket[BID];
            bid.update<BID>(bidPrice, bidSize);
        }
        if (askChanged) {
            Market& ask = contract._externalMarket[ASK];
            ask.update<ASK>(askPrice, askSize);
        }
        {
            if (bidChanged | askChanged) {
                _updatedContracts.insert(internalId);
                if (contract._definition._usedInPricing) {
                    const bool missingBook = (contract._externalMarket[BID].getPrice() == hf::NULL_PRICE) | (contract._externalMarket[ASK].getPrice() == hf::NULL_PRICE);
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
    }

    void onMarketDataUpdate(uint32_t internalId, bool bidChanged, hf::price_t bidPrice, hf::qty_t bidSize, bool askChanged, hf::price_t askPrice, hf::qty_t askSize) {
        RvContract& contract = _contracts[internalId];
        onMarketDataUpdate(contract, internalId, bidChanged, bidPrice, bidSize, askChanged, askPrice, askSize);
    }

    bool canActOnMarketData() {
        return isEnabled() | (_state.load(std::memory_order_relaxed) == SystemState::CheckQuotes);
    }

    void recalculateAfterExternalMarketDataChange() {
        if (canActOnMarketData()) {
            bool needToRecalculate = false;
            for (uint32_t id : _updatedContracts) {
                RvContract& contract = _contracts[id];
                if (contract._definition._usedInPricing) {
                    needToRecalculate = true;
                    updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], id, contract._establishedLevel);
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
        }
    }

    void disableRelativeValue(const std::string& reason) {
        disableRelativeValue(reason, isEnabled());
    }

    void processInternalMarketUpdate(const MarketUpdate& update) {
        RvContract& contract = _contracts[update._instrumentIndex];
        Market& bid = contract._internalMarkets[BID].getMarket(update._marketId);
        const hf::price_t bidPrice = update._bidSize == 0 ? hf::NULL_PRICE : update._bidPrice;
        bid.updateInternalMarket<BID>(bidPrice, update._bidSize);
        const hf::price_t askPrice = update._askSize == 0 ? hf::NULL_PRICE : update._askPrice;
        Market& ask = contract._internalMarkets[ASK].getMarket(update._marketId);
        ask.updateInternalMarket<ASK>(askPrice, update._askSize);
    }

    void updateMidPoint(Market& bid, Market& offer, uint32_t instumentIndex, hf::qty_t establishedLevel) {
        const bool useWightedMid = _useWeightedMid.getValue(instumentIndex);
        double midpoint;
        if (useWightedMid) {
            midpoint = calculateWeightedMidpoint(bid, offer, establishedLevel);
        } else {
            midpoint = calculateSimpleAverage(bid, offer);
        }
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
                sum += _pricingMatrix.getValue(vectorIndex, outrightIndex) * _overdeterminedPrices.getValue(0, outrightIndex) * contract._definition._pricingMultiplier;
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

    double calculateWeightedMidpoint(Market& bid, Market& offer, hf::qty_t establishedLevel) {
        const hf::qty_t offerSize = std::min(offer.getUnAdjustedSize(), establishedLevel);
        const hf::qty_t bidSize = std::min(bid.getUnAdjustedSize(), establishedLevel);
        return (ToDollars(offer.getPrice()) * bidSize + ToDollars(bid.getPrice()) * offerSize) / (bidSize + offerSize);
    }

    double calculateSimpleAverage(Market& bid, Market& offer) {
        return (ToDollars(bid.getPrice()) + ToDollars(offer.getPrice())) / 2;
    }

    void init(double frequency, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) override {
        ViperStrategyBaseImpl<RelativeValue>::init(frequency, internalHeartbeatPeriod, pnlLimitsChecker);
        initializeModel();
    }

    void initializeModel() {
        _widthMultiplier = boost::lexical_cast<double>(_viperConfig.getGlobal(RV_WIDTH_MULTIPLIER));
        initializeContracts();
        _viperConfig.populatePricingMatrix(_pricingMatrix);
        _riskMatrix = _pricingMatrix.inverse().transpose();
        initializeVectorStructures(_viperConfig._vectorConfigs);
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        initializeFromPricingConfig(_viperConfig._pricingConfigs);
        initializeFromRelativeValue(_viperConfig._relativeValueConfigs, _viperConfig._rvPosition);
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
        _sweepDefinition.initDefault(_numberOfAllContracts);
        _maxTicksWide.init(_numberOfAllContracts);
        for (uint32_t index = 0; index < _numberOfAllContracts; ++index) {
            PricingConfig& pc = config[index];
            RvContract& contract = _contracts[index];
            contract._establishedLevel = pc._establishedLevel;
            double weight = pc._weight;
            _weights.setValue(index, weight);
            contract._definition._usedInPricing = !IsZero(weight);
            _useWeightedMid.setValue(index, pc._useWeightedMid);
            contract._definition._pricingMultiplier = pc._pricingMultiplier;
            contract._definition._quantityMultiplier = pc._quantityMultiplier;
            _sweepDefinition.setValue(index, {._sweepTicksAtPrice=pc._sweepTicks * contract._definition._tickSize, ._tradedQty=pc._sweepQuantity});
            _maxTicksWide.setValue(index, config[index]._maxTicksWide < 0 ? std::numeric_limits<int64_t>::max() : config[index]._maxTicksWide * contract._definition._tickSize);
        }
    }

    void calculateOverdeterminedMatrix() {
        Matrix A;
        A.initialize(_numberOfAllContracts, _numberOfPricingContracts);
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            RvContract& contract = _contracts[row];
            for (uint32_t col = 0; col < _numberOfPricingContracts; ++col) {
                double value = 0;
                if (contract._definition._numberOfLegs > 1) {
                    for (uint32_t legIndex = 0; legIndex < contract._definition._numberOfLegs; ++legIndex) {
                        auto& leg = contract._definition._legs[legIndex];
                        if (leg._index == col) {
                            value = leg._size;
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
        _possibleHedgeRatios.initialize(_numberOfVectors, 3);
        _hedgeVectorRiskExposures.initialize(_numberOfVectors, _numberOfVectors);
        _hedgeVectorRiskExposuresSums.init(_numberOfVectors);
        _normalizationMatrix.initialize(_numberOfPricingContracts, 3);
        _commissions.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _shortTermFairsForContracts.init(_numberOfAllContracts);
        _shortTermFairsCache.init(_numberOfAllContracts);
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
                _maxTicksThroughMarket.setValue(contract->_internalId, rvConfig._maxTicksThroughMarket);
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
                   uint32_t side = initialPos > 0 ? BID : ASK;
                   addPosition(_contracts[i], std::abs(initialPos), side, false, true);
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
            contract.setIdsForMarkets(contractIndex);
            auto& definition = contract._definition;
            definition._name = configContract._name;
            definition._sid = configContract._sid;
            definition._tickSize = configContract._tickSize;
            definition._venue = configContract._venue;
            ContractDefinition& contractDefinition = contract._definition;
            contractDefinition._internalId = contractIndex;
            contractDefinition._sid = configContract._sid;
            contractDefinition._tickSize = configContract._tickSize;
            contractDefinition._venue = configContract._venue;
            for (uint32_t legIndex = 0; legIndex < configContract._legs.size(); ++legIndex) {
                ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                leg._index = configContract._legs[legIndex]._index;
                leg._size = configContract._legs[legIndex]._ratio;
            }
            contractDefinition._numberOfLegs = configContract._legs.size();
            contract._priceChangeThreshold = 0.05 * definition._tickSize;
            _contracts.push_back(contract);
            contract._externalMarket[BID].setId(MarketIds::MARKET_ID_EXTERNAL);
            contract._externalMarket[ASK].setId(MarketIds::MARKET_ID_EXTERNAL);
        }
        _numberOfAllContracts = _viperConfig._allContracts.size();
        _numberOfPricingContracts = _viperConfig._numberOfPricingContracts;
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
        ContractDefinition& contractOneDefinition = _contracts[contractOneIndex]._definition;
        ContractDefinition& contractTwoDefinition = _contracts[contractTwoIndex]._definition;
        calculateVectorRiskExposureForContract(PossibleHedgeRatiosColumns::ContractOneRisk, contractOneDefinition);
        calculateVectorRiskExposureForContract(PossibleHedgeRatiosColumns::ContractTwoRisk, contractTwoDefinition);
    }

    double calculateHedgeRatio(uint32_t contractOneIndex, uint32_t contractTwoIndex, int& vectorIndex) {
        calculateVectorRiskExposureForContracts(contractOneIndex, contractTwoIndex);
        calculatePossibleHedgeRatios();
        calculateVectorRisks();
        sumUpVectorRisks();
        return pickBestHedge(vectorIndex);
    }

    void calculateVectorRiskExposureForContract(uint32_t tableColumn, ContractDefinition& contractDefinition) {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double exposure = 0;
            if (contractDefinition._numberOfLegs > 1) {
                for (uint32_t legIndex = 0; legIndex < contractDefinition._numberOfLegs; ++legIndex) {
                    ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                    double vectorRik = _riskMatrix.getValue(vectorIndex, leg._index);
                    exposure += vectorRik * leg._size * contractDefinition._quantityMultiplier;
                }
            } else {
                exposure = _riskMatrix.getValue(vectorIndex, contractDefinition._internalId) * contractDefinition._quantityMultiplier;
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
        return (commissionOne * oneContract._definition._pricingMultiplier + std::abs(hedge) * commissionTwo * twoContract._definition._pricingMultiplier) / normalizedValue;
    }

    enum NormalizationMatrixColumns {ContractOneRatio = 0, ContractTwoRatio = 1, Total = 2};

    void calculateNormalization(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedge) {
        ContractDefinition& contractDefinitionOne = _contracts[contractOneIndex]._definition;
        setHedgeRatioForNormalization(contractDefinitionOne, 1, NormalizationMatrixColumns::ContractOneRatio);
        ContractDefinition& contractDefinitionTwo = _contracts[contractTwoIndex]._definition;
        setHedgeRatioForNormalization(contractDefinitionTwo, hedge, NormalizationMatrixColumns::ContractTwoRatio);
        sumUpRatiosForNormalization();
    }

    void setHedgeRatioForNormalization(ContractDefinition& contractDefinition, double multiplier, uint32_t column) {
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
            double value = 0;
            if (contractDefinition._numberOfLegs > 1) {
                for (uint32_t legIndex = 0; legIndex < contractDefinition._numberOfLegs; ++legIndex) {
                    ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                    if (leg._index == outrightIndex) {
                        value = leg._size;
                    }
                }
            } else {
                if (contractDefinition._internalId == outrightIndex) {
                    value = 1;
                }
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
                move += oneLotMoves.getValue(vectorIndex) * _riskMatrix.getValue(vectorIndex, contractIndex) / contract._definition._pricingMultiplier;
            }
            fairMoveForContract.setValue(contractIndex, move);
        }

        constexpr double InitMinQuoteSizeValue = std::numeric_limits<double>::max();
        double minQuoteSize = InitMinQuoteSizeValue;

        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            double move = 0;
            RvContract& contract = _contracts[spreadIndex];
            if (contract._definition._numberOfLegs > 0) {
                for (uint32_t legIndex = 0; legIndex < contract._definition._numberOfLegs; ++legIndex) {
                    auto& leg = contract._definition._legs[legIndex];
                    const double legMove = fairMoveForContract.getValue(leg._index) * leg._size;
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
                Market& hedgeMarket = lean._isExternal ? leanContract._externalMarket[side] : leanContract._internalMarkets[side].getMarket(lean._marketIndex);
                const double oldNeedToHedge = needToHedge;
                const hf::qty_t orderSize = getOrderSize(hedgeRatio, hedgeMarket.getFakAdjustedSize(), needToHedge, lean._size, residual, quoteSize);
                LOG(0, "RV {C[i1]}. Hedging in {C[i2]} Size={i3} HedgeRatio={d1} NeedToHedge={d2} LearnQuote={i4}@{d3}",
                    getContract(contract._internalId), getContract(leanContract._internalId), orderSize, hedgeRatio,
                    needToHedge, leanContract._quotes[side]._bestSize, ToDollars(leanContract._quotes[side]._bestPrice))
                if (orderSize > 0) {
                    const uint32_t otherSide = GetOtherSide(side);
                    Quote& leanQuote = leanContract._quotes[side];
                    const hf::price_t leanQuotePrice = leanQuote._bestPrice;
                    const hf::price_t hedgePrice = hedgeMarket.getPrice();
                    const bool isHedgePriceCrossingQuotePrice = (leanQuotePrice == hf::NULL_PRICE) ? false : IsBetterOrEqual(hedgePrice, leanQuotePrice, otherSide);
                    if (isHedgePriceCrossingQuotePrice) {
                        removeRequest = true;
                        needToHedge = oldNeedToHedge;
                        LOG(0, "RV {C[i1]}. Hedging. Skipping club member b/c of potential self match. QuotePrice={d1} HedgePrice={d2} Lean={C[i2]}",
                            getContract(contract._internalId), ToDollars(leanQuotePrice), ToDollars(hedgePrice), getContract(leanContract._internalId))
                        std::string message = boost::str(boost::format("Hedge. Club skip. C=%1% H=%2% Pc=%3% Ph=%4%")
                                                         % contract._definition._name % leanContract._definition._name % ToDollars(leanQuotePrice) % ToDollars(hedgePrice));
                        sendErrorMessage(message, ErrorType::Normal);
                        continue;
                    }
                    const uint32_t hedgeId = getHedgeId();
                    LOG(0, "RV Sending hedge Contract={C[i1]} Side={i2} Price={d1} Size={i3} External={i4} HedgeId={i5} QuoteContract={C[i6]} MarketId={i7}",
                        getContract(leanContract._internalId), otherSide, ToDollars(hedgePrice), orderSize,
                        (int) lean._isExternal, hedgeId, getContract(contract._internalId), lean._marketIndex)

                    hedgeMarket.addAdjustment(hedgeId, orderSize);
                    sendHedgeOrder(leanContract._internalId, otherSide, hedgePrice, orderSize, lean._isExternal, hedgeId, lean._marketIndex);
                    addPosition(leanContract, orderSize, otherSide, false, false);
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

    uint32_t getHedgeId() {
        return ++_hedgeId;
    }

    void sendHedgeOrder(uint32_t internalId,
                        uint32_t side,
                        hf::price_t price,
                        hf::qty_t size,
                        bool external,
                        uint32_t hedgeId,
                        uint32_t marketId) {
        _lastOrderRequests[internalId]._hedge[side] = _orderRequestMgr.sendHedgeRequest(
                internalId, orderId(StrategyId::RelativeValue, Order::Type::Hedging), side, price,
                price, size, external, false, _shortTermFairsForContracts.getValue(internalId), marketId,
                hedgeId);
        _orderRequestMgr.notifyOrderRequest(internalId, StrategyId::RelativeValue, _lastOrderRequests[internalId]);
    }

    hf::qty_t getOrderSize(double hedgeRatio, hf::qty_t minAvailableLeanSize, double& needToHedge, double size, double residuals, double quoteSize) {
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
            orderSize = static_cast<hf::qty_t>(std::round(availableLeanSize));
        } else {
            orderSize = static_cast<hf::qty_t>(std::round(needToHedgeInHedgeTerms));
        }
        needToHedge = (needToHedgeInHedgeTerms - orderSize) / hedgeRatio;
        LOG(0, "Hedge OrderSize={i1} Remaining NeedToHedge={d1}", orderSize, needToHedge)
        return orderSize;
    }

    void addPosition(RvContract& contract, double size, uint32_t side, bool checkLimit, bool isStartUp) {
        if (isStartUp) {
            logging::GetLogger().logInfo(boost::str(boost::format("RV Adding position. Contract=%1% Side=%2% Size=%3%") % getContract(contract._internalId) % side % size));
        } else {
            LOG(0, "RV {C[i1]}. Adding position. Side={i2} Size={d1}", getContract(contract._internalId), side, size)
        }
        const double positionChange = side == BID ? size : -size;
        addPositionToVector(contract, positionChange, checkLimit, isStartUp);
        _contractPositions.add(contract._definition._internalId, positionChange);
        // logVectorPositions(isStartUp);
        // logContractPositions(isStartUp);
    }

    void addPositionToVector(RvContract& contract, double positionChange, bool checkLimit, bool isStartUp) {
        if (contract._definition._numberOfLegs > 1) {
            for (uint32_t index = 0; index < contract._definition._numberOfLegs; ++index) {
                ContractDefinition::Leg& leg = contract._definition._legs[index];
                const double legPositionChange = positionChange * leg._size;
                for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                    const double changeInVector = _riskMatrix.getValue(vectorIndex, leg._index) * legPositionChange * contract._definition._quantityMultiplier;
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
                const double changeInVector = _riskMatrix.getValue(vectorIndex, contract._definition._internalId) * positionChange * contract._definition._quantityMultiplier;
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
            const bool setToMarket = _setToMarket.getValue(vectorIndex);
            double market = _currentVectorMarketPrices->getValue(vectorIndex);
            if (!setToMarket) {
                double ltf = market + _positionsInVector.getValue(vectorIndex) * _fareMoveForOneUnitPosition.getValue(vectorIndex);
                _longTermFairsInVector.setValue(vectorIndex, ltf);
                sendGenericMessageToFront(ViperMessageType::StoreLtf, vectorIndex);
            }
            _shortTermFairsInVector.setValue(vectorIndex, market);
        }
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
                const double value = _shortTermFairsInVector.getValue(vectorIndex) * _riskMatrix.getValue(vectorIndex, contractIndex) / contract._definition._pricingMultiplier;
                shortTermFair += value;
            }
            const hf::price_t rhonePrice = ToRhonePrice(shortTermFair);
            _shortTermFairsForContracts.setValue(contractIndex, rhonePrice);
        }
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            ContractDefinition& contractDefinition = _contracts[spreadIndex]._definition;
            hf::price_t shortTermFair = 0;
            if (contractDefinition._numberOfLegs > 1) {
                for (uint32_t legIndex = 0; legIndex < contractDefinition._numberOfLegs; ++legIndex) {
                    ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                    hf::price_t value = _shortTermFairsForContracts.getValue(leg._index) * leg._size;
                    shortTermFair += value;
                }
            } else {
                shortTermFair = _shortTermFairsForContracts.getValue(spreadIndex);
            }
            const hf::price_t rhonePrice = shortTermFair;
            _shortTermFairsForContracts.setValue(spreadIndex, rhonePrice);
        }
        // logShortTermFairsForVectors();
        logShortTermFairsForContracts();
        _hasDataForCharts = true;
    }

    void logShortTermFairsForVectors() {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            LOG(0, "Vector short term fairs Vector={V[i1]} Fair={d1}", getVectorName(vectorIndex), _shortTermFairsInVector.getValue(vectorIndex))
        }
    }

    void logShortTermFairsForContracts() {
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            auto fair = _shortTermFairsForContracts.getValue(spreadIndex);
            if (std::abs(fair - _shortTermFairsCache.getValue(spreadIndex)) > _contracts[spreadIndex]._priceChangeThreshold) {
                LOG(0, "RV {C[i1]}. short term fairs. Fair={d1}", getContract(spreadIndex), ToDollars(fair))
            }
            _shortTermFairsCache.setValue(spreadIndex, fair);
        }
    }

    struct ValidateMaxTicksThroughMarketResult {
        hf::price_t internalMarketsQuotes[Markets::_numberOfMarkets];
        hf::price_t externalMarketQuote;
        bool isValid;
    };

    //Returns hf::null_price if passes
    template<uint32_t side>
    ValidateMaxTicksThroughMarketResult validateMaxTicksThroughMarket(hf::price_t calculatedPrice, uint32_t leanIndex, RvContract& contract, uint32_t instrumentIndex) {
        static_assert((side == BID) || (side == ASK), "Bad side");

        ValidateMaxTicksThroughMarketResult result;
        result.isValid = true;

        hf::price_t leanFair = _shortTermFairsForContracts.getValue(leanIndex);
        hf::price_t fair = _shortTermFairsForContracts.getValue(instrumentIndex);
        hf::price_t width = _contractWidths.getValue(instrumentIndex) + _extraContractWidths.getValue(instrumentIndex);
        double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);

        auto& leanMarkets = _contracts[leanIndex];
        auto& quoteMarkets = _contracts[instrumentIndex];

        auto checkedMin = [](hf::price_t target, hf::price_t candidate) {
            static_assert(hf::NULL_PRICE == INT64_MIN, "Can't use naked std::min since null will always win");
            if(candidate == hf::NULL_PRICE) { return target; }
            return (target == hf::NULL_PRICE ? candidate : std::min(target, candidate));
        };

        auto checkedMax = [](hf::price_t target, hf::price_t candidate) {
            static_assert(hf::NULL_PRICE == INT64_MIN, "We are assuming Max doesn't care about null case");
            return std::max(target, candidate);
        };

        auto swapSide = []() { return side == BID ? ASK : BID; };

        if(side == BID) {

            hf::price_t bestOffer = hf::NULL_PRICE;
            for(uint32_t i = 0; i < quoteMarkets._internalMarkets->_numberOfMarkets; ++i) {
                hf::price_t candidateMarket = quoteMarkets._internalMarkets[swapSide()]._markets[i].getPrice();
                bestOffer = checkedMin(bestOffer, candidateMarket);
            }
            hf::price_t externalFurthestMarketBid = quoteMarkets._externalMarket[swapSide()].getPrice();
            bestOffer = checkedMin(bestOffer, externalFurthestMarketBid);
            if(bestOffer == hf::NULL_PRICE) {
                result.isValid = true;
                return result;
            }

            //One of these formulas should have produced the calculatedPrice
            static_assert(Markets::_numberOfMarkets == 2, "New market needs to be added to assert");
            BOOST_ASSERT((calculatedPrice == calculateBid<true>(leanMarkets._internalMarkets[side]._markets[0], contract, leanIndex, leanFair, fair, width, hedgeRatio, false))
                    || (calculatedPrice == calculateBid<true>(leanMarkets._internalMarkets[side]._markets[1], contract, leanIndex, leanFair, fair, width, hedgeRatio, false))
                    || (calculatedPrice == calculateBid<true>(leanMarkets._externalMarket[side], contract, leanIndex, leanFair, fair, width, hedgeRatio, true)));

            for(int i = 0; i < static_cast<int>(leanMarkets._internalMarkets->_numberOfMarkets); ++i) {
                result.internalMarketsQuotes[i] = calculateBid<false>(leanMarkets._internalMarkets[side]._markets[i], contract, leanIndex, leanFair, fair, width, hedgeRatio, false);
                calculatedPrice = checkedMax(calculatedPrice, result.internalMarketsQuotes[i]);
            }
            result.externalMarketQuote = calculateBid<false>(leanMarkets._externalMarket[side], contract, leanIndex, leanFair, fair, width, hedgeRatio, true);
            calculatedPrice = checkedMax(calculatedPrice, result.externalMarketQuote);

            //TODO: store the multiplier value
            if((calculatedPrice - bestOffer) >= contract._definition._tickSize * _maxTicksThroughMarket.getValue(instrumentIndex)) {
                   result.isValid = false;
            }
        }
        else {
            hf::price_t bestBid = hf::NULL_PRICE;
            for(uint32_t i = 0; i < quoteMarkets._internalMarkets->_numberOfMarkets; ++i) {
                hf::price_t candidateBestBid = quoteMarkets._internalMarkets[swapSide()]._markets[i].getPrice();
                bestBid = checkedMax(bestBid, candidateBestBid);
            }
            bestBid = std::max(bestBid, quoteMarkets._externalMarket[swapSide()].getPrice());
            if(bestBid == hf::NULL_PRICE) {
                result.isValid = true;
                return result;
            }

            //One of these formulas should have produced the calculatedPrice
            static_assert(Markets::_numberOfMarkets == 2, "New market needs to be added to assert");
            BOOST_ASSERT((calculatedPrice == calculateAsk<true>(leanMarkets._internalMarkets[side]._markets[0], contract, leanIndex, leanFair, fair, width, hedgeRatio, false))
                    || (calculatedPrice == calculateAsk<true>(leanMarkets._internalMarkets[side]._markets[1], contract, leanIndex, leanFair, fair, width, hedgeRatio, false))
                    || (calculatedPrice == calculateAsk<true>(leanMarkets._externalMarket[side], contract, leanIndex, leanFair, fair, width, hedgeRatio, true)));

            for(uint32_t i = 0; i < leanMarkets._internalMarkets->_numberOfMarkets; ++i) {
                result.internalMarketsQuotes[i] = calculateAsk<false>(leanMarkets._internalMarkets[side]._markets[i], contract, leanIndex, leanFair, fair, width, hedgeRatio, false);
                calculatedPrice = checkedMin(calculatedPrice, result.internalMarketsQuotes[i]);
            }
            result.externalMarketQuote = calculateAsk<false>(leanMarkets._externalMarket[side], contract, leanIndex, leanFair, fair, width, hedgeRatio, true);
            calculatedPrice = checkedMin(calculatedPrice, result.externalMarketQuote);

            if((bestBid - calculatedPrice) >= contract._definition._tickSize * _maxTicksThroughMarket.getValue(instrumentIndex)) {
                result.isValid = false;
            }
        }
        return result;
    }

    void calculateQuotes() {
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
                        const hf::price_t fair = _shortTermFairsForContracts.getValue(instrumentIndex);
                        const hf::price_t width = _contractWidths.getValue(instrumentIndex) + _extraContractWidths.getValue(instrumentIndex);
                        const hf::price_t leanFair = _shortTermFairsForContracts.getValue(leanIndex);
                        const double maxQuoteSize = _quoteSizes.getValue(contract._definition._internalId, leanIndex);

                        RvContract& leanContract = _contracts[leanIndex];

                        Markets& leanBids = leanContract._internalMarkets[BID];
                        for (uint32_t leanBidIndex = 0; leanBidIndex < leanBids._numberOfMarkets; ++leanBidIndex) {
                            Market& market = leanBids._markets[leanBidIndex];
                            calculateBidQuote(market, contract, leanIndex, leanFair, fair, width, maxQuoteSize, false);
                        }

                        Market& externalBid = leanContract._externalMarket[BID];
                        calculateBidQuote(externalBid, contract, leanIndex, leanFair, fair, width, maxQuoteSize, true);

                        Markets& leanOffers = leanContract._internalMarkets[ASK];
                        for (uint32_t leanBidIndex = 0; leanBidIndex < leanOffers._numberOfMarkets; ++leanBidIndex) {
                            Market& market = leanOffers._markets[leanBidIndex];
                            calculateAskQuote(market, contract, leanIndex, leanFair, fair, width, maxQuoteSize, false);
                        }

                        Market& externalAsk = leanContract._externalMarket[ASK];
                        calculateAskQuote(externalAsk, contract, leanIndex, leanFair, fair, width, maxQuoteSize, true);
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
                    std::string error = "MaxQuotesThroughMarketCheckFailed.  Contract=";
                    disableRelativeValue(error + contract._definition._name);
                    LOG(0, "MaxQuotesThroughMarketCheckFailed, Disable External Quoting. OffendingContract={C[i1]}", getContract(contract._internalId))
                    sendErrorMessage("Disabling QuoteExternal. Reason=RV MaxQuotesThroughMarketCheckFailed", ErrorType::Critical);
                    disableQuoteExternal();
                    break;
                }
                sendQuoteOrder(contract, quoteExternal);
            }
        }

        const uint64_t latency = clockUtil::GetCycles() - startTimeStamp;
        DLOG(0, "CalculateQuotes Latency {i1}", latency);

        //noErrorsOnCreateQuote will likely disable RV, causing canSendQuotes() to return false
        if (BOOST_LIKELY((!noErrorsOnCreateQuote) | canSendQuotes())) {
            LOG(0, "RV Start of internal fills transaction {i1}", _fillsTransactionNumber);
            _withinInternalFillsOfCycle = true;
            sendGenericMessageToExecutor(ViperMessageType::EndOfQuotes, _fillsTransactionNumber++);
        }
    }

    bool canSendQuotes() {
        return isEnabled() & _canSendQuotes;
    }

    bool canQuoteExternal(uint32_t instrumentIndex, hf::VENUE venue) const {
        return _contractQuoteExternal.getValue(instrumentIndex) & canQuoteExternally()
            & s_systemState._exchQuoteExternally.load(std::memory_order_relaxed).test(static_cast<uint8_t>(venue));
    }

    void sendQuoteOrder(RvContract& contract, bool quoteExternal) {
        if (canSendQuotes()) {
            Quote& bidQuote = contract._quotes[BID];
            Quote& askQuote = contract._quotes[ASK];

            if ((bidQuote._bestPrice != hf::NULL_PRICE) | (askQuote._bestPrice != hf::NULL_PRICE)) {
                auto& lastRequest = _lastOrderRequests[contract._definition._internalId];
                lastRequest._quote[BID] = _orderRequestMgr.sendQuoteRequest(
                        contract._definition._internalId,
                        orderId(StrategyId::RelativeValue, Order::Type::Quoting), BID,
                        bidQuote._bestPrice, bidQuote._bestSize, bidQuote._externalPrice, bidQuote._externalSize,
                        quoteExternal);
                lastRequest._quote[ASK] = _orderRequestMgr.sendQuoteRequest(
                        contract._definition._internalId,
                        orderId(StrategyId::RelativeValue, Order::Type::Quoting), ASK,
                        askQuote._bestPrice, askQuote._bestSize, askQuote._externalPrice, askQuote._externalSize,
                        quoteExternal);
                _orderRequestMgr.notifyOrderRequest(contract._definition._internalId, StrategyId::RelativeValue,
                                                    lastRequest);
            }
        } else {
            TLOG(0, "RV {C[i1]}. Quote blocked by IME transaction.", getContract(contract._internalId))
        }
    }

    void calculateBidQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double maxQuoteSize, bool isExternal) {
        const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
        hf::price_t bidPrice = calculateBid(leanMarket, contract, leanIndex, leanFair, fair, width, hedgeRatio, isExternal);
        if(bidPrice != hf::NULL_PRICE) {
            const double size = calculateSize(contract, leanIndex, ASK, hedgeRatio, leanMarket.getFakAdjustedSize(), maxQuoteSize);
            contract._quotes[BID]._leans.add(leanIndex, leanMarket.getId(), bidPrice, size, isExternal);
        }
    }

    template<bool ShouldCapNegativeCostToPctWidth = true>
    hf::price_t calculateBid(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double hedgeRatio, bool isExternal) {
        if ((leanMarket.getPrice() != hf::NULL_PRICE && !IsZero(hedgeRatio))    // on hedge path: must have price
            || (IsZero(hedgeRatio) && isExternal)) {                            // on no hedge path: must external
            RvContract& leanContract = _contracts[leanIndex];
            const hf::price_t bidHedgingCost = leanFair - leanMarket.getPrice();
            const hf::price_t hedgingCost = calculateHedgingCost<ShouldCapNegativeCostToPctWidth>(width, bidHedgingCost, hedgeRatio, contract, leanContract);
            const hf::price_t multipliedCommission = static_cast<hf::price_t>(_commissions.getValue(contract._definition._internalId, leanIndex) / contract._definition._pricingMultiplier);
            const hf::price_t bidPrice = fair - width - hedgingCost - multipliedCommission;
            return bidPrice;
        }
        return hf::NULL_PRICE;
    }

    void calculateAskQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double maxQuoteSize, bool isExternal) {
        const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
        hf::price_t askPrice = calculateAsk(leanMarket, contract, leanIndex, leanFair, fair, width, hedgeRatio, isExternal);
        if(askPrice != hf::NULL_PRICE) {
            const double size = calculateSize(contract, leanIndex, BID, hedgeRatio, leanMarket.getFakAdjustedSize(), maxQuoteSize);
            contract._quotes[ASK]._leans.add(leanIndex, leanMarket.getId(), askPrice, size, isExternal);
        }
    }

    template<bool ShouldCapNegativeCostToPctWidth = true>
    hf::price_t calculateAsk(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double hedgeRatio, bool isExternal) {
        if ((leanMarket.getPrice() != hf::NULL_PRICE && !IsZero(hedgeRatio))    // on hedge path: must have price
            || (IsZero(hedgeRatio) && isExternal)) {                            // on no hedge path: must external
            RvContract& leanContract = _contracts[leanIndex];
            const hf::price_t askHedgingCost = leanMarket.getPrice() - leanFair;
            const hf::price_t hedgingCost = calculateHedgingCost<ShouldCapNegativeCostToPctWidth>(width, askHedgingCost, hedgeRatio, contract, leanContract);
            const hf::price_t multipliedCommission = static_cast<hf::price_t>(_commissions.getValue(contract._definition._internalId, leanIndex) / contract._definition._pricingMultiplier);
            const hf::price_t askPrice = fair + width + hedgingCost + multipliedCommission;
            return askPrice;
        }
        return hf::NULL_PRICE;
    }

    template<bool ShouldCapNegativeCostToPctWidth = true>
    hf::price_t calculateHedgingCost(hf::price_t width, hf::price_t cost, double hedgeRatio, RvContract& contract, RvContract& leanContract) {
        const hf::price_t hedgingCost = static_cast<hf::price_t>(cost * hedgeRatio * leanContract._definition._pricingMultiplier / contract._definition._pricingMultiplier
                                                                 * leanContract._definition._quantityMultiplier / contract._definition._quantityMultiplier);
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
            // LOG(0, "Calculated lean size. Contract={C[i1]} Lean={C[i2]} Sid={i3} Market={d1} Max={d2} Size={d3} HedgeRatio is not positive. Ratio={d4}", getContract(contract._internalId), getContract(leanIndex), side, marketSize, maxQuoteSize, size, hedgeRatio)
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
        Market& bid = contract._externalMarket[BID];
        if (bid.getPrice() != hf::NULL_PRICE) {
            bidPrice = bid.getPrice();
            bidUnadjustedSize = bid.getUnAdjustedSize();
            bidAdjustedSize = bid.getFakAdjustedSize();
        }
        hf::price_t askPrice = 0;
        hf::qty_t askUnadjustedSize = 0;
        hf::qty_t askAdjustedSize = 0;
        Market& offer = contract._externalMarket[ASK];
        if (offer.getPrice() != hf::NULL_PRICE) {
            askPrice = offer.getPrice();
            askAdjustedSize = offer.getFakAdjustedSize();
            askUnadjustedSize = offer.getUnAdjustedSize();
        }
        LOG(0, "Market: Contract={C[i1]} Bid: {i2}/{i3}@{d1} Offer: {i4}/{i5}@{d2}", getContract(contract._internalId), bidAdjustedSize, bidUnadjustedSize, ToDollars(bidPrice), askAdjustedSize, askUnadjustedSize, ToDollars(askPrice))
    }

    void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size) {
        sendGenericMessageToFront(ViperMessageType::AcceptedManualFill, internalId, size);
    }

    void populateGUIInitialization(::google::protobuf::RepeatedPtrField< ::Python::proto::GridDefinition >* definitions) const override;
    void sendStateToGUI(std::function<void(proto::GridUpdate&)>&& send) const override;
    void handleViperMessage(const ViperMessage& message);

private:
    void sendVectorGridStateToGui(std::function<void(proto::GridUpdate&)>& send) const;
    void onShiftUpdate(uint32_t shift);

public:
    Matrix _pricingMatrix; // x,y - vectors - square
    Matrix _riskMatrix; // x,y - vectors - square
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
    Vector<double> _maxTicksThroughMarket;
    Vector<hf::price_t> _maxTicksWide;

    Vector<hf::price_t> _shortTermFairsForContracts;
    Vector<hf::price_t> _shortTermFairsCache;
    Vector<double> _shortTermFairsInVector;
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
    uint64_t _lastEmaTime;

    double _widthMultiplier = 0;

    struct SimpleSweepDefinition {
        hf::price_t _sweepTicksAtPrice;
        uint32_t _tradedQty;
        bool sweepDetected(const md::MDIntraEventSweepEvent& event) const {
            return (std::abs(event.diff) >= _sweepTicksAtPrice) && (event.qty >= _tradedQty);
        }
    };
    Vector<SimpleSweepDefinition> _sweepDefinition;

private:
    std::set<uint32_t> _updatedContracts;
    UpdateSelector<RelativeValue> _updateSelector;

    bool _canSendQuotes = true;
    bool _endOfQuotes = false;
    bool _calculateDueToFront = false;
    bool _hadInternalMarketData = false;
    bool _hadExternalMarketData = false;

    uint32_t _numberOfFills = 0;

    om::client_id_t _id;

    std::unordered_map<uint32_t, double> _hedgeRequests;
    uint32_t _hedgeId = 0;
    bool _withinInternalFillsOfCycle = false;
    bool _hasPendingQuoteCalculation = false;
    int64_t _fillsTransactionNumber = 0;

    OrderRequestManager& _orderRequestMgr;
    std::vector<ReadyOrderRequests> _lastOrderRequests;

    // ********** TEMP working structures TEMP *********** //
    Matrix _possibleHedgeRatios; // row, col - vectors, 3
    Matrix _hedgeVectorRiskExposures; // row, col - vectors, vectors
    Vector<double> _hedgeVectorRiskExposuresSums; // vectors
    Matrix _normalizationMatrix; // row, col - outrights, 3
    // *************************************************** //

#ifdef VIPER_UNIT_TEST
template<typename... T>
void initializeQueues(T&&... t) {}
//TODO: Fix unit tests
//    void initializeQueues(md::queue::MDQueueResource* inFromFront, md::queue::MDQueueResource* outToFront, md::queue::MDQueueResource* inFromMM, md::queue::MDQueueResource* outToMM) {
//        _inFromFront = inFromFront;
//        _outToFront = outToFront;
//        _inFromMM = inFromMM;
//        _outToMM = outToMM;
//    }
#endif
};

}

#endif //VIPER_RELATIVE_VALUE_H
