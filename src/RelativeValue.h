#ifndef VIPER_RELATIVE_VALUE_H
#define VIPER_RELATIVE_VALUE_H

#include <boost/format.hpp>
#include <md/md.h>
#include <functional>
#include <limits>
#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include <md/md_event.h>
#include <md/queue/MDQueue.h>
#include "Common.h"
#include "ViperConfig.h"
#include "MathStructures.h"
#include "ClockUtil.h"
#include "UpdateSelector.h"
#include <set>
#include <hf/monitor/Monitor.h>
#include <md/subscription/RequestMessage.h>
#include <hf/util/SocketUtils.h>
#include "logging/Coolog.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <md/queue/MDQueueResource.h>
#include <hf/queue/DebugQueueResource.h>
#include <hf/ResourceManager.h>

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

    uint32_t getNumberOfLeans() {
        return _leanIndex;
    }

    Lean* getLean_FOR_TESTING(uint32_t instrumentIndex) {
        for (uint32_t index = 0; index < _leanIndex; ++index) {
            Lean& lean = _leans[index];
            if (lean._instrumentIndex == instrumentIndex) {
                return &lean;
            }
        }
        return nullptr;
    }

    void clear() {
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

    template <typename ContractsT>
    void createQuote(hf::price_t tickSize, uint32_t side, bool quoteExternal, ContractsT& contracts, uint32_t contractId) {
        if (_leans.getNumberOfLeans() > 0) {
            if (side == BID) {
                _leans.sortBids();
            } else {
                _leans.sortAks();
            }

            Lean& bestLean = _leans._leans[0];
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
                        // LOG(0, "Quote {C[i1]}. Added lean {C[i2]} Size={d1} Price={d2}", getContractName(contractId, contracts),  getContractName(lean._instrumentIndex, contracts), lean._size, ToDollars(lean._price))
                    } else if (roundedPrice == price) {
                        clubSize += static_cast<hf::qty_t>(lean._size);
                        ++clubCount;
                       // LOG(0, "Quote {C[i1]}. Added lean {C[i2]} Size={d1} Price={d2}", getContractName(contractId, contracts),  getContractName(lean._instrumentIndex, contracts), lean._size, ToDollars(lean._price))
                    } else {
                        break;
                    }
                }
                _externalPrice = roundedPrice;
                _externalSize = std::max(_bestSize, static_cast<hf::qty_t>(clubSize / clubCount + 0.0001));
            }
            else {
                _externalPrice = hf::NULL_PRICE;
                _externalSize = 0;
            }

            Lean& lean = _leans._leans[0];
            auto& leanContract = contracts[lean._instrumentIndex];
            auto& market = lean._isExternal ? leanContract._externalMarket[side] : leanContract._internalMarkets[BID].getMarket(lean._marketIndex);

            LOG(0, "Quote. Contract={C[i1]} Side={i2} Internal: {i3}@{d1} External: {i4}@{d2} Lean={C[i5]} {i6}@{d3}",
                getContractName(contractId, contracts), side, _bestSize, ToDollars(_bestPrice), _externalSize, ToDollars(_externalPrice), getContractName(lean._instrumentIndex, contracts),
                market.getFakAdjustedSize(), ToDollars(market.getPrice()))
        }
    }


#ifdef IS_DEV
        template <typename ContractsT>
        std::string getContractName(uint32_t id, ContractsT& contracts) {
            return contracts[id]._definition._name;
        }
#else
        template <typename ContractsT>
        uint32_t getContractName(uint32_t id, ContractsT& contracts) {
            return id;
        }
#endif
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

    void setIdsForMarkets(uint32_t id) {
        _externalMarket[BID].setContractId(id);
        _externalMarket[ASK].setContractId(id);
        _internalMarkets[BID].setContractId(id);
        _internalMarkets[ASK].setContractId(id);
    }
};

class RelativeValue : public virtual hf::Service {

public:

    RelativeValue()
    :
        _updateSelector(*this)
    {
        _manualFills.resize(5);
    }

    void onStratBegin() {
    }

    void onStratEnd() {
    }

    void onPostCreate(const libconfig::Setting& config) override;

    void onRun() override {
        logging::GetLogger().logInfo(boost::str(boost::format("RV logger %1%") % _kdbLogger));
        _kdbLogger->resetW();
        logging::setKdbLogger(_kdbLogger);
        for (auto queue : _marketDataQueues) {
            subscribeToMarketData(queue);
        }
        LOG(0, "RV Frequency. {d1}", _frequency)

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            runCycle();
            const uint64_t now = clockUtil::GetCycles();
            sendInternalHeartbeat(now);
        }
    }

    void sendInternalHeartbeat(uint64_t now) {
        if (BOOST_UNLIKELY(now > _nextInternalHeartbeat)) {
            _nextInternalHeartbeat = now + _internalHeartbeatPeriod;
            sendGenericMessageToFront(ViperMessageType::InternalHeartbeat);
        }
    }

    void runCycle() {
        checkSystemState();
        const bool calculateDueToFront = drainFromFront();
        bool endOfQuotes = false;
        const bool hadInternalMarketData = drainFromExecutor(endOfQuotes);
        const bool hadExternalMarketData = drainAllMarketData();
        const bool calculateDueToHedges = processHedgeRequests();

        if (hadExternalMarketData) {
            recalculateAfterExternalMarketDataChange();
        }
        _canSendQuotes = _hedgeRequests.empty() & !_withinInternalFillsOfCycle;
        const bool recalculateQuotes = (canActOnMarketData() & (calculateDueToFront | hadInternalMarketData | hadExternalMarketData)) | calculateDueToHedges;
        if (recalculateQuotes) {
            _hasPendingQuoteCalculation = _withinInternalFillsOfCycle;
            calculateQuotes();
        } else if (endOfQuotes & _hasPendingQuoteCalculation){
            _hasPendingQuoteCalculation = false;
            calculateQuotes();
        }
    }

    om::debug_t& getDebugInfo() {
        static om::debug_t info;
        return info;
    }

#ifdef IS_DEV
        std::string getContractName(uint32_t id) {
            return _contracts[id]._definition._name;
        }

        std::string getVectorName(uint32_t id) {
            return _vectorNames[id];
        }
#else
        uint32_t getContractName(uint32_t id) {
            return id;
        }

         uint32_t getVectorName(uint32_t id) {
            return id;
        }
#endif

    bool processHedgeRequests() {
        const bool hasHedgeRequests = !_hedgeRequests.empty();
        if (hasHedgeRequests) {
            LOG(0, "Processing hedge requests. {i1}", _hedgeRequests.size())
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
                LOG(0, "Hedge request. {C[i1]} AbsSize={d1} Side={i2} residuals={d2} reqSize={d3}", getContractName(contractId), absSize, hedgeSide, residuals, requestSize)
                bool removeRequest = false;
                const double leftToHedge = sendHedges(contract, absSize, hedgeSide, removeRequest, std::abs(residuals));
                if (removeRequest) {
                    addPosition(contract, std::abs(requestSize), requestSide, true, false);
                    it = _hedgeRequests.erase(it);
                    LOG(0, "Ending hedging loop. Name={C[i1]} LeftToHedge={d1}", getContractName(contract._internalId), leftToHedge);
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
                    LOG(0, "Not fully hedged. Name={C[i1]} LeftToHedge={d1} Left={d2} hedged={d3}", getContractName(contract._internalId), leftToHedge, left, hedgedSize)
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
                if (!IsZero(vectorRisk)) {
                    double vectorPos = _positionsInVector.getValue(vectorIndex);
                    position += vectorPos / vectorRisk / contract._definition._quantityMultiplier;
                }
            }
        }
        return position;
    }

    void checkSystemState() {
        if (_systemState->_relativeValue != _oldSystemState) {
            onSystemStateChange();
            _oldSystemState = _systemState->_relativeValue;
        }
    }

    void onSystemStateChange() {
        switch (_systemState->_relativeValue) {
            case SystemState::HardShutdown: {
                handleHardShutdown();
                break;
            }
            case SystemState::Disabled: {
                handleDisabled();
                break;
            }
            case SystemState::Enabled: {
                handleEnabled();
                break;
            }
            case SystemState::LoggedOut: {
                handleLogout();
                break;
            }
            case SystemState::LoggedIn: {
                handleLogin();
                break;
            }
            case SystemState::CheckQuotes: {
                handleCheckQuotes();
                break;
            }
            case SystemState::CancelGtcAndDisable: {
                cancelGtcAndDisable();
                break;
            }
        }
    }

    void cancelGtcAndDisable() {
        LOG(0, "RV. Got message disable and cancel GTC. Reason=Requested {i1}", 0)
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void handleHardShutdown() {
        LOG(0, "RV hard shutdown. {i1}", 0);
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateHardShutdown);
    }

    void handleDisabled() {
        LOG(0, "RV got disabled. {i1}", 0);
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void handleLogin() {
        LOG(0, "RV Logged in {i1}", 0);
    }

    void handleLogout() {
        LOG(0, "RV Logged out {i1}", 0);
    }

    void handleCheckQuotes() {
        LOG(0, "RV check quotes {i1}", 0);
        doInitialCalculations();
    }

    void handleEnabled() {
        if (canEnable()) {
            LOG(0, "RV got enabled {i1}", 0);
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled);
            doInitialCalculations();
            if (_firstCalculation) {
                sendGenericMessageToExecutor(ViperMessageType::FirstOrdersDone);
                _firstCalculation = false;
            }
        } else {
            LOG(0, "RV could NOT be enabled {i1}", 0);
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
            _systemState->_relativeValue = SystemState::Disabled;
        }
    }

    bool canEnable() {
        bool canEnable = true;
        if (!hasMarketData()) {
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
                sendErrorMessage(boost::str(boost::format("%%stfMove not within limits.  Value must be within [0,2].  Contract=%1% Value=%2%") % getContractName(i) % _pctStfMoves.getValue(i)), ErrorType::Critical);
                canEnable = false;
            }
        }

        return canEnable;
    }

    bool hasMarketData() {
        bool hasMarketData = true;
        for (uint32_t contractIndex = 0; contractIndex < _numberOfAllContracts; ++contractIndex) {
            RvContract& contract = _contracts[contractIndex];
            if (contract._definition._usedInPricing) {
                const bool missingBook = (contract._externalMarket[BID].getPrice() == hf::NULL_PRICE) | (contract._externalMarket[ASK].getPrice() == hf::NULL_PRICE);
                if (missingBook) {
                    hasMarketData = false;
                    sendErrorMessage(boost::str(boost::format("RV - Does not have market data for. Contract=%1%") % contract._definition._name), ErrorType::Critical);
                    LOG(0, "RV - Does not have market data for {C[i1]}", getContractName(contractIndex))
                }
                if((contract._externalMarket[BID].getFakAdjustedSize() < 0) || (contract._externalMarket[BID].getUnAdjustedSize() < 0)
                   || (contract._externalMarket[ASK].getFakAdjustedSize() < 0) || (contract._externalMarket[ASK].getUnAdjustedSize() < 0)) {
                    hasMarketData = false;
                    std::string errorMessageForGUI = boost::str(boost::format("Contract %1% has bad size.  BidUA=%2% AskUA=%3% BidAdj=%4% AskAdj=%5%")
                            % contract._definition._name % contract._externalMarket[BID].getUnAdjustedSize() % contract._externalMarket[ASK].getUnAdjustedSize()
                            % contract._externalMarket[BID].getFakAdjustedSize() % contract._externalMarket[ASK].getFakAdjustedSize());
                    sendErrorMessage(errorMessageForGUI, ErrorType::Critical);
                    LOG(0, "RV - Invalid quantities for {C[i1]} - bUA={i2} aUA={i3} bA={i4} aA={i5}", getContractName(contractIndex), contract._externalMarket[BID].getUnAdjustedSize(),
                            contract._externalMarket[ASK].getUnAdjustedSize(), contract._externalMarket[BID].getFakAdjustedSize(), contract._externalMarket[ASK].getFakAdjustedSize());
                    logging::GetLogger().logError(errorMessageForGUI);
                    hasMarketData = false;
                }
            }
        }
        return hasMarketData;
    }

    bool isPnlWithinLimits() {
        const bool withinLimits = _pnlLimitsChecker(toUInt(StrategyId::RelativeValue));
        if (!withinLimits) {
            sendErrorMessage("RV - not within pnl limits.",  ErrorType::Critical);
            LOG(0, "RV - is not within pnl limits {i1}", 0)
        }
        return withinLimits;
    }

    bool isWithinPositionLimits() {
        bool withinLimits = true;
        for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
            const double vectorPosition = _positionsInVector.getValue(vectorIndex);
            const double limit = _vectorsMaxPosition.getValue(vectorIndex);
            if (vectorPosition > limit) {
                sendErrorMessage("RV - not within vector position limits.",  ErrorType::Critical);
                LOG(0, "Vector position not witin limts. Vector={V[i1]} Position={d1} Limit={d2}", getVectorName(vectorIndex), vectorPosition, limit)
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
                    LOG(0, "Missing market on initial calculation of mid points. Name={C[i1]}", getContractName(contract._internalId))
                }
            }
        }
    }

    void sendGenericMessageToExecutor(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToMM->getPoolP()->getNextP());
        outMessage._type = type;
        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = value1;
        message._number_2 = value2;
        const auto outIdx = _outToMM->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = type;
        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = value1;
        message._number_2 = value2;
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void sendErrorMessage(const std::string& s, ErrorType type) {
        if (_systemState->_relativeValue != SystemState::LoggedOut) {
            auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
            outMessage._type = ViperMessageType::ErrorMessage;
            ErrorMessage& message = reinterpret_cast<ErrorMessage &> (outMessage._data[0]);
            const uint32_t size = std::min(CRITICAL_MESSAGE_MAX_SIZE, (uint32_t) s.size());
            strncpy(message._message, s.c_str(), size);
            message._size = size;
            message._type = type;
            const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(ErrorMessage));
            mb();
            _outToFront->appendNew(outIdx);
        }
    }

    void subscribeToMarketData(md::queue::MDQueue* mdQueue) {
        bool proxyIdSet = mdQueue->trySetProxyIdentifier(_id);
        if (!proxyIdSet) {
            throw std::runtime_error("Could not set proxy id on MD.");
        }

        int port;
        bool portOk = mdQueue->tryGetMonitorPort(port);
        if (!portOk) {
            throw std::runtime_error("Could not get port from MD.");
        }

        int fd = hf::util::SocketTCPUtils::createSocketREMOTE("127.0.0.1", false, hf::util::SocketUtils::Acceleration::DISABLED);

        if (fd == -1) {
            throw std::runtime_error("RV could not create socket to MD");
        }

        if (-1 == hf::util::SocketTCPUtils::connect(fd, "127.0.0.1", port)) {
            throw std::runtime_error("RV could not connect to MD socket");
        }

        sendMessageToMDSocket(fd, _id);

        for (uint32_t internalId = 0; internalId < _numberOfAllContracts; ++internalId) {
            RvContract& contract = _contracts[internalId];
            struct md::request::MDBookRequestMsg bookRequestMsg (_id, contract._definition._venue, contract._definition._sid, 1, true, true);
            sendMessageToMDSocket(fd, bookRequestMsg);
            LOG(0, "RV subscribed md for. Contract={C[i1]}", getContractName(contract._internalId))
        }
    }

    template <typename MessageT>
    void sendMessageToMDSocket(int fd, MessageT& message) {
        const auto len = sizeof(MessageT);
        const auto bytesSent = send(fd, &message, len, MSG_NOSIGNAL);
        if (BOOST_UNLIKELY(bytesSent != len)){
            throw std::runtime_error("Could not send message to MD from RV");
        }
    }

    bool drainFromFront() {
        bool recalculate = false;
        while (true) {
            uint64_t idx = _inFromFront->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                break;

            _inFromFront->freeNext();
            auto& message = *static_cast<ViperMessage*>(_inFromFront->getPoolP()->getData(idx));

            recalculate = true;

            switch (message._type) {
                case ViperMessageType::GridUpdate: {
                    GridUpdateMessage& gridChange = reinterpret_cast<GridUpdateMessage &> (message._data[0]);
                    bool accepted = processGridChange(*gridChange._gridUpdate);
                    sendGenericMessageToFront(ViperMessageType::GridUpdateAccepted, gridChange._gridUpdate->_update.gridid());
                    break;
                }
                default:
                    break;
            }
        }
        return recalculate;
    }

    bool drainFromExecutor(bool& endOfQuotes) {
        bool hadInternalMarketData = false;
        uint32_t drainSize = 0;
        uint32_t numberOfFills = 0;

        while (true) {
            uint64_t idx = _inFromMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                break;

            _inFromMM->freeNext();
            auto& message = *static_cast<ViperMessage*>(_inFromMM->getPoolP()->getData(idx));

            switch (message._type) {
                case ViperMessageType::MarketUpdate: {
                    MarketUpdate& marketUpdate = reinterpret_cast<MarketUpdate &> (message._data[0]);
                    processInternalMarketUpdate(marketUpdate);
                    hadInternalMarketData = true;
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    handleFill(fill);
                    ++numberOfFills;
                    break;
                }
                case ViperMessageType::HedgeMDAdjsutAck: {
                    HedgeMarketDataAdjustmentAck& hedgeAck = reinterpret_cast<HedgeMarketDataAdjustmentAck &> (message._data[0]);
                    handleHedgeMarketDataAdjustmentAck(hedgeAck);
                    break;
                }
                case ViperMessageType::InternalHedgeDone: {
                    InternalHedgeDone& hedgeDone = reinterpret_cast<InternalHedgeDone &> (message._data[0]);
                    handleInternalHedgeDone(hedgeDone);
                    break;
                }
                case ViperMessageType::EndOfQuotes: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    handleEndOfInternalFills(reply._number_1);
                    endOfQuotes = true;
                    break;
                }
                default:
                    break;
            }

            ++drainSize;
        }

        if (drainSize > 0) {
            LOG(0, "RV drain size from executor. {i1}", drainSize)
        }

        if (numberOfFills > 0) {
            LOG(0, "RV drained number of fills. {i1}", numberOfFills)
        }

        return hadInternalMarketData;
    }

    void handleInternalHedgeDone(InternalHedgeDone& message) {
        RvContract& contract = _contracts[message._instrumentIndex];
        Market& market = contract._internalMarkets[message._side]._markets[message._marketIndex];
        market.handleInternalHedgeDone(message._hedgeId);
    }

    void handleHedgeMarketDataAdjustmentAck(HedgeMarketDataAdjustmentAck& hedgeAck) {
        RvContract& contract = _contracts[hedgeAck._instrumentIndex];
        LOG(0, "Received hedge md ack. {C[i1]} HedgeId={i2} Adj={i3} OrderId={i4}", getContractName(contract._internalId), hedgeAck._hedgeId, hedgeAck._size, (uint32_t) hedgeAck._orderId)
        Market& market = contract._externalMarket[hedgeAck._side];
        market.ackAdjustment(hedgeAck._hedgeId, hedgeAck._size, hedgeAck._orderId);
    }

    void handleFill(FillMessage& fill) {
        if (isEnabled()) {
            fill._rvStf = _shortTermFairsForContracts.getValue(fill._contractId);
            sendFillToFront(fill);
            RvContract& contract = _contracts[fill._contractId];
            LOG(0, "RV got filled. Contract={C[i1]} Side={i2} Price={d1} Qty={i3}", getContractName(contract._internalId), fill._side, ToDollars(fill._price), fill._qty);
            const hf::qty_t fillSize = fill._side == BID ? fill._qty : -fill._qty;
            auto hedgeIt = _hedgeRequests.find(fill._contractId);
            if (hedgeIt != _hedgeRequests.end()) {
                hedgeIt->second += fillSize;
            } else {
                _hedgeRequests.insert({fill._contractId, fillSize});
            }
        } else {
            LOG(0, "RV got filled. But disabled. Sending back to Executor. Contract={C[i1]} Side={i2} Price={d1} Qty={i3}", getContractName(fill._contractId), fill._side, ToDollars(fill._price), fill._qty);
            sendFillBackToExecutor(fill);
        }
    }

    void handleEndOfInternalFills(int64_t tranNumber) {
        LOG(0, "RV End of internal fills transaction {i1}", tranNumber);
        _withinInternalFillsOfCycle = false;
    }

    void sendFillBackToExecutor(FillMessage& fill) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToMM->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage&> (outMessage._data[0]);
        message._contractId = fill._contractId;
        message._side = fill._side;
        message._qty = fill._qty;
        message._strategy = fill._strategy;
        message._type = fill._type;
        message._price = fill._price;
        message._source = fill._source;
        const auto outIdx = _outToMM->getPoolP()->append(sizeof(ViperMessageType) + sizeof(QuoteOrderMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void sendFillToFront(FillMessage& fill) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractId = fill._contractId;
        message._side = fill._side;
        message._qty = fill._qty;
        message._strategy = fill._strategy;
        message._type = fill._type;
        message._price = fill._price;
        message._source = fill._source;
        message._rvStf = fill._rvStf;
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToFront->appendNew(outIdx);
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
                    _useEMAInVector.setValue(gridCell.row(), value);
                    longTermFairs = true;
                    shortTermFairs = true;
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

        if (longTermFairs) {
            calculateLongTermFairsInVector();
        }

        if (shortTermFairs) {
            calculateShortTermFairs();
        }

        calculateQuotes();

        return true;
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
                    _quoteExternal.setValue(gridCell.row(), value);
                    LOG(0, "RV quote external changed. Contract={C[i1]} Value={i2}", getContractName(contract._internalId),value)
                    break;
                }
                case ContractParameterRVColumns::Commission: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _commissionsInput.setValue(gridCell.row(), value);
                    foundation = true;
                    LOG(0, "RV commission changed. Contract={C[i1]} Value={d1}", getContractName(contract._internalId), value)
                    break;
                }
                case ContractParameterRVColumns::Width: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _contractWidths.setValue(gridCell.row(), ToRhonePrice(value));
                    calculateOneExtraContractWidth(gridCell.row());
                    foundation = true;
                    widthChanged = true;
                    LOG(0, "RV width changed. Contract={C[i1]} Value={d1}", getContractName(contract._internalId), value)
                    break;
                }
                case ContractParameterRVColumns::PCTStfMove : {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _pctStfMoves.setValue(gridCell.row(), value);
                    foundation = true;
                    rvQuoteMaxPercentWidthChange = true;
                    LOG(0, "%%stfMove changed. Contract={C[i1]} Value={d1}", getContractName(contract._internalId), value)
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
                bool errorOccuredOnThisIter = false;
                try {
                    double value = boost::lexical_cast<double>(gridCell->value());
                    if(!isPctStfMoveWithinLimits(value)) {
                        sendErrorMessage(boost::str(boost::format("%%stfMove not within limits.  Value must be within [0,2].  Contract=%1% Value=%2%") % contract._definition._name % value), ErrorType::Critical);
                        isValid = false;
                        errorOccuredOnThisIter = true;
                    }
                }
                catch(boost::bad_lexical_cast& blc) {
                    sendErrorMessage(boost::str(boost::format("%%stfMove not parsable Contract=%1% Value=%2%") % contract._definition._name % gridCell->value()), ErrorType::Critical);
                    isValid = false;
                    errorOccuredOnThisIter = true;
                }

                if(errorOccuredOnThisIter) {
                    gridCell->set_value(std::to_string(_pctStfMoves.getValue(gridCell->row())));
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
            LOG(0, "RV lean_matrix changed. Contract={C[i1]} Contract={C[i2]} Value={i3}", getContractName(contractOne._internalId), getContractName(contractTwo._internalId), value)
        }

        calculateQuotes();

        return true;
    }

    bool validateRvLeanMatrixUpdate(GridUpdate& update) {
        return true;
    }

    bool onRvPricingConfigUpdate(GridUpdate& update) {
        bool valid = validateRvPricingConfigUpdate(update);

        if (!valid) {
            return false;
        }

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            PricingConfigsColumns columnType = static_cast<PricingConfigsColumns>(gridCell.col());
            RvContract& contract = _contracts[gridCell.row()];
            switch (columnType) {
                case PricingConfigsColumns::UseWeighted: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _useWeightedMid.setValue(gridCell.row(), value);
                    updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], gridCell.row(), contract._establishedLevel);
                    LOG(0, "RV use_weighted changed. Contract={C[i1]} Value={i2}", getContractName(contract._internalId), value)
                    break;
                }
                case PricingConfigsColumns::Weight: {
                    const double weight = boost::lexical_cast<double>(gridCell.value());
                    LOG(0, "RV weight changed. Contract={C[i1]} Value={d1}", getContractName(contract._internalId), weight)
                    _weights.setValue(gridCell.row(), weight);
                    contract._definition._usedInPricing = !IsZero(weight);
                    if (contract._definition._usedInPricing) {
                        calculateOverdeterminedMatrix();
                        updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], gridCell.row(), contract._establishedLevel);
                    }
                    break;
                }
                case PricingConfigsColumns::EstablishedLevel: {
                    hf::qty_t value = boost::lexical_cast<hf::qty_t>(gridCell.value());
                    contract._establishedLevel = value;
                    updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], gridCell.row(), contract._establishedLevel);
                    LOG(0, "RV established_level changed. Contract={C[i1]} Value={i2}", getContractName(contract._internalId), value)
                    break;
                }
            }
        }

        calculateOverdeterminedPrices();
        calculateVectorMarketPrices();
        calculateLongTermFairsInVector();
        calculateShortTermFairs();
        calculateQuotes();

        return true;
    }

    bool validateRvPricingConfigUpdate(GridUpdate& update) {
        return true;
    }

    bool onManualFills(GridUpdate& update) {
        int numberOfFills = update._update.cells_size() / 3;
        _manualFills.resize(numberOfFills);
        int currentManualFillIndex = -1;
        int priorGridCellRow = -1;
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            ManualFillsEntryColumns columnType = static_cast<ManualFillsEntryColumns>(gridCell.col());
            if(priorGridCellRow != static_cast<int>(gridCell.row())) {
                ++currentManualFillIndex;
                priorGridCellRow = static_cast<int>(gridCell.row());
            }
            ManualFill& fill = _manualFills[currentManualFillIndex];
            switch (columnType) {
                case ManualFillsEntryColumns::Contract: {
                    fill._contract = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Size: {
                    fill._size = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Type: {
                    fill._type = atoi(gridCell.value().c_str());
                    break;
                }
            }
        }

        for (int index = 0; index < numberOfFills; ++index) {
            ManualFill& fill = _manualFills[index];
            if (fill._type == toUInt(ManualFillType::RelativeValue)) {
                RvContract& contract = _contracts[fill._contract];
                uint32_t size = std::abs(fill._size);
                uint32_t side = fill._size > 0 ? BID : ASK;
                addPosition(contract, size, side, false, false);
                sendAcceptedManualFillToFront(contract._internalId, fill._size);
            }
        }

        checkVectorPositionLimits(false);
        calculateShortTermFairs();
        calculateQuotes();

        return true;
    }

    bool onMarketMakerParametersUpdate(GridUpdate& update) { return true; }
    bool onStackerParametersUpdate(GridUpdate& update) { return true; }
    bool onPnlParametersUpdate(GridUpdate& update) { return true; }

    bool onGlobalConfigsUpdate(GridUpdate& update) {
        bool change = false;
        bool quotes = false;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            proto::GridCell& gridCell = *update._update.mutable_cells(i);
            auto& config = _viperConfig->_globalConfigs[gridCell.row()];
            if (config._key == RV_WIDTH_MULTIPLIER) {
                double value = boost::lexical_cast<double>(gridCell.value());
                if (!isWidthMultiplierWithinLimits(value)) {
                    LOG(0, "RV could not change width multiplier. Value must be > 1 and < 10 Value={d1}", value)
                    sendErrorMessage(boost::str(boost::format("RV could not change width multiplier. Value must be > 1 and < 10 Value=%1%") % value), ErrorType::Critical);
                    gridCell.set_value(std::to_string(_widthMultiplier));
                } else {
                    _widthMultiplier = value;
                    calculateExtraContractWidths();
                    LOG(0, "RV width_multiplier changed. Value={d1}", _widthMultiplier)
                    quotes = true;
               }
            }
        }

        if (change) {
            calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
        }

        if (change | quotes) {
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

    void addMarketDataQueue(md::queue::MDQueue* q) {
        _marketDataQueues.push_back(q);
    }

    bool drainAllMarketData() {
        bool handMarketData = false;
        uint32_t drainSize = 0;
        for (md::queue::MDQueue* mdQueue : _marketDataQueues) {
            while (true) {
                const auto idx = mdQueue->getNext();

                if (BOOST_UNLIKELY(!idx))
                    break;

                mdQueue->freeNext();

                auto& batch = mdQueue->getPoolP()->getBatch(idx);
                onMDBatch(batch);

                {
                    mdQueue->poolBytesRead(batch.size);
                    uint64_t usage;
                    if (BOOST_UNLIKELY(mdQueue->poolThresholdExceeded(usage))) {
                        onPoolThresholdExceeded(mdQueue->getQueueID(), usage);
                    }
                    if (BOOST_UNLIKELY(mdQueue->queueThresholdExceeded(usage))) {
                        onQueueThresholdExceeded(mdQueue->getQueueID(), usage);
                    }
                }

                handMarketData = true;
                ++drainSize;
            }
        }

        if (drainSize > 0) {
            LOG(0, "MD Drain size. {i1}", drainSize)
        }

        return handMarketData;
    }

    void onPoolThresholdExceeded(const std::string& queueName, uint64_t usage) {
        logging::GetLogger().logError("RV Pool_threshold_exceeded");
    }

    void onQueueThresholdExceeded(const std::string& queueName, uint64_t usage) {
        logging::GetLogger().logError("RV Queue_threshold_exceeded");
    }

    void onMDBatch(const md::MDBatch &batch)  {
        LOG(0, "RV received MD batch. {i1}", batch.bid)
        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &mde = reinterpret_cast<const md::MDEvent &> (*data);
            data += mde.size;
            switch (mde.type) {
                case md::MDEvent::TYPE::UPDATE: {
                    const auto &event = static_cast<const md::MDUpdateEvent &> (mde);
                    onMDUpdate(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::TRADE: {
                    const auto &event = static_cast<const md::MDTradeEvent &> (mde);
                    onMDTrade(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::GAP: {
                    const auto &event = static_cast<const md::MDGapEvent &> (mde);
                    onMDGap(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::STATUS: {
                    const auto &event = static_cast<const md::MDStatusEvent &> (mde);
                    onMDStatus(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::FAK_DONE: {
                    const auto &event = static_cast<const md::MDFakDonenEvent &> (mde);
                    handleFakDone(batch.venue, event);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent &event) {
        if (venue == hf::VENUE::CME) {
            for (uint32_t i = 0; i < event.num; ++i) {
                auto& trade = event.trades[i];
                auto it = _sidToInternalId.find(event.sid);
                if (it != _sidToInternalId.end()) {
                    const uint32_t internalId = it->second;
                    if (trade.side == hf::SIDE::BUY) {
                        //LOG(0, "RV: Contract {C[i1]} Bid book adjustment due to trade. {i2}@{d1}", getContractName(internalId), trade.q, ToDollars(trade.pr))
                        onMarketDataUpdate(internalId, true, trade.pr, trade.q, false, 0, 0);
                    } else if (trade.side == hf::SIDE::SELL) {
                        //LOG(0, "RV: Contract {C[i1]} Ask book adjustment due to trade. {i2}@{d1}", getContractName(internalId), trade.q, ToDollars(trade.pr))
                        onMarketDataUpdate(internalId, false, 0, 0, true, trade.pr, trade.q);
                    }
                }
            }
        }
    }

    bool isEnabled() {
        return _systemState->_relativeValue == SystemState::Enabled;
    }

    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent& event) {
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            RvContract& contract = _contracts[internalId];
            switch (event.statusType) {
                case md::MDStatusEvent::TYPE::RECOVERING:
                case md::MDStatusEvent::TYPE::TIMEOUT:
                case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
                    contract._canTradeStatus = false;
                    if (contract._definition._usedInPricing) {
                        LOG(0, "Contract {C[i1]} market data not working. Reason={i2}", getContractName(contract._internalId), toUInt(event.statusType))
                        disableRelativeValue(boost::str(boost::format("Contract %1% market data not working. Reason=%2%.") % contract._definition._name % toUInt(event.statusType)));
                    } else {
                        LOG(0, "Contract {C[i1]} market data not working. Reason={i2}. RV.", getContractName(contract._internalId), toUInt(event.statusType))
                    }
                    break;
                }
                case md::MDStatusEvent::TYPE::RECOVERED: {
                    LOG(0, "Contract {C[i1]} market data recovered. RV.", getContractName(contract._internalId))
                    contract._canTradeStatus = true;
                    break;
                }
                case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                    switch (event.extStatus) {
                        case 'O': {
                            contract._canTradeStatus = true;
                            LOG(0, "Contract {C[i1]} market data - open. RV.", getContractName(contract._internalId))
                            break;
                        }
                        case 'C': {
                            contract._canTradeStatus = false;
                            if (contract._definition._usedInPricing) {
                                LOG(0, "Contract {C[i1]} market data - closed. Used in pricing.", getContractName(contract._internalId))
                                disableRelativeValue(boost::str(boost::format("Contract %1% market data - closed. Used in pricing.") % contract._definition._name));
                            } else {
                                LOG(0, "Contract {C[i1]} market closed. RV.", getContractName(contract._internalId))
                            }
                        }
                        case 'E': {
                            contract._canTradeStatus = false;
                            if (contract._definition._usedInPricing) {
                                LOG(0, "Contract {C[i1]} market data - expired. Used in pricing.", getContractName(contract._internalId))
                                disableRelativeValue(boost::str(boost::format("Contract %1% market data - expired. Used in pricing.") % contract._definition._name));
                            } else {
                                LOG(0, "Contract {C[i1]} market data - expired. RV.", getContractName(contract._internalId))
                            }
                        }
                        case '1': {
                            contract._canTradeStatus = false;
                            LOG(0, "Contract {C[i1]} market data - entered pre-open. RV.", getContractName(contract._internalId))
                            break;
                        }
                        default:
                            break;
                    }
                    break;
                }
                case md::MDStatusEvent::TYPE::UNKNOWN: {
                    std::string message = boost::str(boost::format("Contract %1% market data - received unknown md status. RV") % contract._definition._name);
                    sendErrorMessage( boost::str(boost::format("Contract %1% market data - received unknown md status. RV") % contract._definition._name), ErrorType::Critical);
                    LOG(0, "Contract {C[i1]} market data - received unknown md status. RV", getContractName(contract._internalId))
                    break;
                }
            }
        }
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent &event) {
        LOG(0, "MD feed gapped. {i1}", 0)
        disableRelativeValue("MD feed gapped.");
    }

    void handleFakDone(hf::VENUE venue, const md::MDFakDonenEvent& event) {
        LOG(0, "Received FAK done. {i1}", 0)
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            RvContract& contract = _contracts[internalId];
            const uint32_t side = ToViperSide(event._side);
            Market& book = contract._externalMarket[side];
            book.handleFakDone(event._oid);
        }
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent &event) {
        const bool bidChanged = event.buy.updatedQtyNum == 1;
        const md::top_t::level_t& bid = event.buy.top.l[0];
        const bool askChanged = event.sell.updatedQtyNum == 1;
        const md::top_t::level_t& ask = event.sell.top.l[0];
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            onMarketDataUpdate(internalId, bidChanged, bid.pr, bid.q, askChanged, ask.pr, ask.q);
        } else {
            logging::GetLogger().logError(boost::str(boost::format("Received book update for unrecognized SID='%1%'") % event.sid));
        }
    }

    void onMarketDataUpdate(uint32_t internalId, bool bidChanged, hf::price_t bidPrice, hf::qty_t bidSize, bool askChanged,  hf::price_t askPrice, hf::qty_t askSize) {
        RvContract& contract = _contracts[internalId];
        if (bidChanged) {
            Market& bid = contract._externalMarket[BID];
            bid.update(bidPrice, bidSize);
        }
        if (askChanged) {
            Market& ask = contract._externalMarket[ASK];
            ask.update(askPrice, askSize);
        }
        {
            if (bidChanged | askChanged) {
                _updatedContracts.insert(internalId);
                if (contract._definition._usedInPricing) {
                    const bool missingBook = (contract._externalMarket[BID].getPrice() == hf::NULL_PRICE) | (contract._externalMarket[ASK].getPrice() == hf::NULL_PRICE);
                    if (missingBook) {
                        LOG(0, "Lost book for contract. {C[i1]}", getContractName(contract._internalId))
                        disableRelativeValue(boost::str(boost::format("Do not have market data for pricing. Contract=%1%") % contract._definition._name));
                    }
                }
            }
        }
    }

    bool canExecute() {
        return _systemState->_relativeValue == SystemState::Enabled;
    }

    bool canActOnMarketData() {
        return canExecute() | (_systemState->_relativeValue == SystemState::CheckQuotes);
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

    void disableRelativeValue(const std::string& reason) {
        if (isEnabled()) {
            LOG(0, "Disabling Relative Value. {i1}", 0)
            sendErrorMessage(boost::str(boost::format("Disabling Relative Value. Reason=%1%") % reason), ErrorType::Critical);
            _systemState->_relativeValue = SystemState::Disabled;
            handleDisabled();
        }
    }

    void processInternalMarketUpdate(MarketUpdate& update) {
        RvContract& contract = _contracts[update._instrumentIndex];
        Market& bid = contract._internalMarkets[BID].getMarket(update._marketId);
        const hf::price_t bidPrice = update._bidSize == 0 ? hf::NULL_PRICE : update._bidPrice;
        bid.updateInternalMarket(bidPrice, update._bidSize);
        const hf::price_t askPrice = update._askSize == 0 ? hf::NULL_PRICE : update._askPrice;
        Market& ask = contract._internalMarkets[ASK].getMarket(update._marketId);
        ask.updateInternalMarket(askPrice, update._askSize);
    }

    void updateMidPoint(Market& bid, Market& offer, uint32_t instumentIndex, hf::qty_t establishedLevel) {
        LOG(0, "Updating midpoint for {C[i1]}", getContractName(instumentIndex))
        const bool useWightedMid = _useWeightedMid.getValue(instumentIndex);
        double midpoint;
        if (useWightedMid) {
            midpoint = calculateWeightedMidpoint(bid, offer, establishedLevel);
        } else {
            midpoint = calculateSimpleAverage(bid, offer);
        }
        _midPoints.setValue(0, instumentIndex, midpoint);
    }

    void calculateOverdeterminedPrices() {
        LOG(0, "CalculateOverdeterminedPrices. {i1}", 0)
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
                    const uint64_t period = _emaPeriodInVector.getValue(vectorIndex);
                    const double alpha = static_cast<double>(timeInCycles) / period;
                    const double mu = std::exp(-alpha);
                    const double nu = (1 - mu) / alpha;
                    const double lastEma = _longTermFairsInVector.getValue(vectorIndex);
                    const double newEma = mu * lastEma + (nu - mu) * _priorVectorMarketPrices->getValue(vectorIndex) + (1 - nu) * _currentVectorMarketPrices->getValue(vectorIndex);
                    _longTermFairsInVector.setValue(vectorIndex, newEma);
                    // LOG(0, "EMA calculation. Vector={V[i1]} ElapsedTime={i2} Period={i3} alpha={d1} mu={d2} nu={d3} lastEma={d4} newEma={d5}",
                    //    getVectorName(vectorIndex), GetMicrosFromCycles(timeInCycles, _frequency), GetMicrosFromCycles(period, _frequency), alpha, mu, nu, lastEma, newEma)
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

    void init(ViperConfig& viperConfig, double frequency, SharedSystemState& systemState, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) {
        _frequency = frequency;
        _systemState = &systemState;
        _pnlLimitsChecker = pnlLimitsChecker;
        _internalHeartbeatPeriod = internalHeartbeatPeriod;
        initializeModel(viperConfig);
    }

    void initializeModel(ViperConfig& config) {
        _widthMultiplier = boost::lexical_cast<double>(config.getGlobal(RV_WIDTH_MULTIPLIER));
        initializeContracts(config);
        config.populatePricingMatrix(_pricingMatrix);
        _riskMatrix = _pricingMatrix.inverse().transpose();
        initializeVectorStructures(config._vectorConfigs);
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        initializeFromPricingConfig(config._pricingConfigs);
        initializeFromRelativeValue(config._relativeValueConfigs);
        config._indexOfFirstTradedContract = _indexOfFirstTradedContract;
        initializeLeanMatrix(config._leanMatrix);
        initializeStructures();
        calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
        _overdeterminedPrices.initialize(1, _numberOfPricingContracts);
        _midPoints.initialize(1, _numberOfAllContracts);
        calculateOverdeterminedMatrix();
        _viperConfig = &config;
    }

    void initializeFromPricingConfig(std::vector<PricingConfig>& config) {
        _weights.init(_numberOfAllContracts);
        _useWeightedMid.init(_numberOfAllContracts);
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
        _longTermFairsInVector.init(size);
        _positionsInVector.init(size);
        _factorHedged.init(size);
        for (uint32_t index = 0; index < size; ++index) {
            VectorConfig& vc = config[index];
            _vectorsRange.setValue(index, vc._range);
            _vectorsMaxPosition.setValue(index, vc._maxPos);
            _setToMarket.setValue(index, vc._setToMarket);
            _useEMAInVector.setValue(index, vc._useEMA);
            uint64_t periodInCycles = GetCyclesFromSeconds(vc._EMALength, _frequency);
            _emaPeriodInVector.setValue(index, periodInCycles);
            _longTermFairsInVector.setValue(index, vc._longTermFair);
            _vectorNames.push_back(vc._vectorName);
            _positionsInVector.setValue(index, 0);
            _fareMoveForOneUnitPosition.setValue(index, 0);
            _vectorMarketPrices[0].setValue(index, 0);
            _vectorMarketPrices[1].setValue(index, 0);
            _factorHedged.setValue(index, vc._hedged);
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

    void initializeFromRelativeValue(std::vector<RelativeValueConfig>& config) {
        _commissionsInput.init(_numberOfAllContracts);
        _contractPositions.init(_numberOfAllContracts);
        _quoteExternal.init(_numberOfAllContracts);
        _contractWidths.init(_numberOfAllContracts);
        _extraContractWidths.init(_numberOfAllContracts);
        _pctStfMoves.init(_numberOfAllContracts);
        for (uint32_t contractIndex = 0; contractIndex < config.size(); ++contractIndex) {
            RelativeValueConfig& rvConfig = config[contractIndex];
            RvContract* contract = getContract(rvConfig._contract);
            if (contractIndex == 0) {
                _indexOfFirstTradedContract = contract->_internalId;
            }
            if (contract != nullptr) {
                _commissionsInput.setValue(contract->_internalId, rvConfig._commission);
                _contractWidths.setValue(contract->_internalId, ToRhonePrice(rvConfig._width));
                if (rvConfig._position != 0) {
                    uint32_t side = rvConfig._position > 0 ? BID : ASK;
                    addPosition(*contract, std::abs(rvConfig._position), side, false, true);
                }
                _contractPositions.setValue(contract->_internalId, rvConfig._position);
                _quoteExternal.setValue(contract->_internalId, rvConfig._quoteExternal);
                _pctStfMoves.setValue(contract->_internalId, rvConfig._pctStfMove);
                contract->_traded = true;
            } else {
                logging::GetLogger().logErrorAndThrow("Got unknown contract when initializing rel value");
            }
        }
        calculateExtraContractWidths();
        checkVectorPositionLimits(true);
    }

    RvContract* getContract(const std::string& name) {
        for (auto& c : _contracts) {
            if (c._definition._name == name) {
                return &c;
            }
        }
        return nullptr;
    }

    void initializeContracts(ViperConfig& config) {
        for (uint32_t contractIndex = 0; contractIndex < config._allContracts.size(); ++contractIndex) {
            ConfigContract& configContract = config._allContracts[contractIndex];
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
            _sidToInternalId.insert({configContract._sid, contractIndex});
            for (uint32_t legIndex = 0; legIndex < configContract._legs.size(); ++legIndex) {
                ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                leg._index = configContract._legs[legIndex]._index;
                leg._size = configContract._legs[legIndex]._ratio;
            }
            contractDefinition._numberOfLegs = configContract._legs.size();
            _contracts.push_back(contract);
            contract._externalMarket[BID].setId(contractIndex);
            contract._externalMarket[ASK].setId(contractIndex);
        }
        _numberOfAllContracts = config._allContracts.size();
        _numberOfPricingContracts = config._numberOfPricingContracts;
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
        uint32_t minIndex = 0;
        double min = _hedgeVectorRiskExposuresSums.getValue(0);
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposuresSums.getValue(vectorIndex);
            bool isTheSame = std::abs(sum - min) < 0.000001;
            if (isTheSame) {
                minIndexes.push_back(vectorIndex);
            } else if (sum < min) {
                minIndexes.clear();
                minIndexes.push_back(vectorIndex);
            }
            if (sum < min) {
                min = sum;
                minIndex = vectorIndex;
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

        return _possibleHedgeRatios.getValue(minIndex, 2);
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
        LOG(0, "Send hedges for fill. {C[i1]} Size={d1} Side={i2}", getContractName(contract._internalId), size, side)
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
                Market& hedgeMarket = lean._isExternal ? leanContract._externalMarket[side] : leanContract._internalMarkets[side].getMarket(lean._marketIndex);
                const double oldNeedToHedge = needToHedge;
                const hf::qty_t orderSize = getOrderSize(hedgeRatio, hedgeMarket.getFakAdjustedSize(), needToHedge, lean._size, residual);
                LOG(0, "Hedging {C[i1]} in {C[i2]} Size={i3} HedgeRatio={d1} NeedToHedge={d2}", getContractName(contract._internalId), getContractName(leanContract._internalId), orderSize, hedgeRatio, needToHedge)
                if (orderSize > 0) {
                    const uint32_t otherSide = GetOtherSide(side);
                    Quote& leanQuote = leanContract._quotes[side];
                    const hf::price_t leanQuotePrice = leanQuote._bestPrice;
                    const hf::price_t hedgePrice = hedgeMarket.getPrice();
                    const bool isHedgePriceCrossingQuotePrice = IsBetter(hedgePrice, leanQuotePrice, otherSide) | (leanQuotePrice == hedgePrice);
                    if (isHedgePriceCrossingQuotePrice) {
                        removeRequest = true;
                        needToHedge = oldNeedToHedge;
                        LOG(0, "Hedging. {C[i1]} Skipping club member b/c of potential self match. QuotePrice={d1} HedgePrice={d2} Lean={C[i2]}",
                            getContractName(contract._internalId), ToDollars(leanQuotePrice), ToDollars(hedgePrice), getContractName(leanContract._internalId))
                        std::string message = boost::str(boost::format("Hedge. Club skip. C=%1% H=%2% Pc=%3% Ph=%4%")
                                                         % contract._definition._name % leanContract._definition._name % ToDollars(leanQuotePrice) % ToDollars(hedgePrice));
                        sendErrorMessage(message, ErrorType::Normal);
                        continue;
                    }
                    const uint32_t hedgeId = getHedgeId();
                    hedgeMarket.addAdjustment(hedgeId, orderSize);
                    LOG(0, "Sending hedge. Contract={C[i1]} Side={i2} Price={d1} Size={i3} External={i4} HedgeId={i5} QuoteContract={C[i6]}",
                        getContractName(leanContract._internalId), otherSide, ToDollars(hedgePrice), orderSize, (int) lean._isExternal, hedgeId, getContractName(contract._internalId))
                    sendHedgeOrder(leanContract._internalId, otherSide, hedgePrice, orderSize, lean._isExternal, hedgeId, lean._marketIndex);
                    addPosition(leanContract, orderSize, otherSide, false, false);
                    sentHedge = true;
                }

                if ((needToHedge < 0) | (IsZero(needToHedge))) {
                    LOG(0, "Fully hedged. Contract={C[i1]} Side={i2} NeedToHedge={d1}.", getContractName(leanContract._internalId), side, needToHedge)
                    removeRequest = true;
                    break;
                }
            } else {
                if (sentHedge) {
                    LOG(0, "Not Fully hedged but sent hedge so will recalculate. Contract={C[i1]} Side={i2} NeedToHedge={d1}.", getContractName(contract._internalId), side, needToHedge)
                } else {
                    LOG(0, "Not Fully hedged and did not send hedge. Contract={C[i1]} Side={i2} NeedToHedge={d1}.", getContractName(contract._internalId), side, needToHedge)
                    removeRequest = true;
                }
                break;
            }
        }

        if (!sentHedge) {
            LOG(0, "Did not send hedge. Removing. Contract={C[i1]} Side={i2} NeedToHedge={d1}.", getContractName(contract._internalId), side, needToHedge)
            removeRequest = true;
        }

        const uint64_t latency = clockUtil::GetCycles() - hedgeStartTimestamp;
        LOG(0, "HedgeLatency {i1}", latency)
        return needToHedge;
    }

    uint32_t getHedgeId() {
        return ++_hedgeId;
    }

    void sendHedgeOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, bool external, uint32_t hedgeId, uint32_t marketId) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToMM->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::HedgeOrder;

        HedgeOrderMessage& message = reinterpret_cast<HedgeOrderMessage &> (outMessage._data[0]);
        message._instrumentIndex = internalId;
        message._side = side;
        message._price = price;
        message._size = size;
        message._isExternal = external;
        message._hedgeId = hedgeId;
        message._marketId = marketId;
        message._stf = _shortTermFairsForContracts.getValue(internalId);

        const auto outIdx = _outToMM->getPoolP()->append(sizeof(ViperMessageType) + sizeof(HedgeOrderMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    hf::qty_t getOrderSize(double hedgeRatio, hf::qty_t minAvailableLeanSize, double& needToHedge, double size, double residuals) {
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
            logging::GetLogger().logInfo(boost::str(boost::format("Adding position. Contract=%1% Side=%2% Size=%3%") % getContractName(contract._internalId) % side % size));
        } else {
            LOG(0, "Adding position. Contract={C[i1]} Side={i2} Size={d1}", getContractName(contract._internalId), side, size)
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
        if (vectorPosition > limit) {
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
                LOG(0, "Contract position. Contract={C[i1]} Position={d1}", getContractName(spreadIndex), _contractPositions.getValue(spreadIndex))
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
            LOG(0, "Contract short term fairs. Contract={C[i1]} Fair={d1}", getContractName(spreadIndex), ToDollars(_shortTermFairsForContracts.getValue(spreadIndex)))
        }
    }

    void calculateQuotes() {
        // LOG(0, "CalculateQuotes {i1}", 0);
        const uint64_t startTimeStamp = clockUtil::GetCycles();
        // logCurrentExternalMarkets();
        for (uint32_t instrumentIndex = _indexOfFirstTradedContract; instrumentIndex < _numberOfAllContracts; ++instrumentIndex) {
            RvContract& contract = _contracts[instrumentIndex];
            contract.clearLeans();
            if (contract._traded) {
                // LOG(0, "Calculating quotes for {C[i1]}", getContractName(contract._internalId))
                for (uint32_t leanIndex = _indexOfFirstTradedContract; leanIndex < _numberOfAllContracts; ++leanIndex) {
                    const bool canLeanOn = _leanMatrix.getValue(instrumentIndex, leanIndex);
                    if (canLeanOn) {
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

                const bool quoteExternal = _quoteExternal.getValue(instrumentIndex);

                Quote& bidQuote = contract._quotes[BID];
                bidQuote.createQuote(contract._definition._tickSize, BID, quoteExternal, _contracts, instrumentIndex);

                Quote& askQuote = contract._quotes[ASK];
                askQuote.createQuote(contract._definition._tickSize, ASK, quoteExternal, _contracts, instrumentIndex);

                sendQuoteOrder(contract, quoteExternal);
            }
        }

        const uint64_t latency = clockUtil::GetCycles() - startTimeStamp;
        LOG(0, "CalculateQuotes Latency {i1}", latency);

        if (canSendQuotes()) {
            LOG(0, "RV Start of internal fills transaction {i1}", _fillsTransactionNumber);
            _withinInternalFillsOfCycle = true;
            sendGenericMessageToExecutor(ViperMessageType::EndOfQuotes, _fillsTransactionNumber++);
        }
    }

    bool canSendQuotes() {
        return canExecute() & _canSendQuotes;
    }

    void sendQuoteOrder(RvContract& contract, bool quoteExternal) {
        if (canSendQuotes()) {
            Quote& bidQuote = contract._quotes[BID];
            Quote& askQuote = contract._quotes[ASK];

            if ((bidQuote._bestPrice != hf::NULL_PRICE) | (askQuote._bestPrice != hf::NULL_PRICE)) {
                auto& outMessage = *static_cast<ViperMessage*>(_outToMM->getPoolP()->getNextP());
                outMessage._type = ViperMessageType::QuoteOrder;

                QuoteOrderMessage& message = reinterpret_cast<QuoteOrderMessage&> (outMessage._data[0]);
                message._instrumentIndex = contract._definition._internalId;
                message._quoteExternally = quoteExternal;

                auto& messageBid = message._quotes[BID];
                messageBid._externalPrice = bidQuote._externalPrice;
                messageBid._externalSize = bidQuote._externalSize;
                messageBid._internalMarketPrice = bidQuote._bestPrice;
                messageBid._internalMarketSize = bidQuote._bestSize;

                auto& messageAsk = message._quotes[ASK];
                messageAsk._externalPrice = askQuote._externalPrice;
                messageAsk._externalSize = askQuote._externalSize;
                messageAsk._internalMarketPrice = askQuote._bestPrice;
                messageAsk._internalMarketSize = askQuote._bestSize;

                const auto outIdx = _outToMM->getPoolP()->append(sizeof(ViperMessageType) + sizeof(QuoteOrderMessage));
                mb();
                _outToMM->appendNew(outIdx);
            }
        } else {
            LOG(0, "Could not send RV quote for {C[i1]}. System Disabled.", getContractName(contract._internalId))
        }
    }

    void calculateBidQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double maxQuoteSize, bool isExternal) {
        if (leanMarket.getPrice() != hf::NULL_PRICE) {
            RvContract& leanContract = _contracts[leanIndex];
            const hf::price_t bidHedgingCost = leanFair - leanMarket.getPrice();
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t hedgingCost = calculateHedgingCost(width, bidHedgingCost, hedgeRatio, contract, leanContract);
            const hf::price_t multipliedCommission = static_cast<hf::price_t>(_commissions.getValue(contract._definition._internalId, leanIndex) / contract._definition._pricingMultiplier);
            const hf::price_t bidPrice = fair - width - hedgingCost - multipliedCommission;
            const double size = calculateSize(contract, leanIndex, ASK, hedgeRatio, leanMarket.getFakAdjustedSize(), maxQuoteSize);
            contract._quotes[BID]._leans.add(leanIndex, leanMarket.getId(), bidPrice, size, isExternal);
        }
    }

    void calculateAskQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double maxQuoteSize,  bool isExternal) {
        if (leanMarket.getPrice() != hf::NULL_PRICE) {
            RvContract& leanContract = _contracts[leanIndex];
            const hf::price_t askHedgingCost = leanMarket.getPrice() - leanFair;
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t hedgingCost = calculateHedgingCost(width, askHedgingCost, hedgeRatio, contract, leanContract);
            const hf::price_t multipliedCommission = static_cast<hf::price_t>(_commissions.getValue(contract._definition._internalId, leanIndex) / contract._definition._pricingMultiplier);
            const hf::price_t askPrice = fair + width + hedgingCost + multipliedCommission;
            const double size = calculateSize(contract, leanIndex, BID, hedgeRatio, leanMarket.getFakAdjustedSize(), maxQuoteSize);
            contract._quotes[ASK]._leans.add(leanIndex, leanMarket.getId(), askPrice, size, isExternal);
        }
    }

    hf::price_t calculateHedgingCost(hf::price_t width, hf::price_t cost, double hedgeRatio, RvContract& contract, RvContract& leanContract) {
        const hf::price_t hedgingCost = static_cast<hf::price_t>(cost * hedgeRatio * leanContract._definition._pricingMultiplier / contract._definition._pricingMultiplier
                                                                 * leanContract._definition._quantityMultiplier / contract._definition._quantityMultiplier);
        if (hedgingCost < 0) {
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
            // LOG(0, "Calculated lean size. Contract={C[i1]} Lean={C[i2]} Side={i3} Market={d1} Max={d2} PossibleSize={d3} Size={d4} HedgeRatio={d5}", getContractName(contract._internalId), getContractName(leanIndex), side, marketSize, maxQuoteSize, possibleSize, size, hedgeRatio)
        } else {
            // LOG(0, "Calculated lean size. Contract={C[i1]} Lean={C[i2]} Sid={i3} Market={d1} Max={d2} Size={d3} HedgeRatio is not positive. Ratio={d4}", getContractName(contract._internalId), getContractName(leanIndex), side, marketSize, maxQuoteSize, size, hedgeRatio)
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
        LOG(0, "Market: Contract={C[i1]} Bid: {i2}/{i3}@{d1} Offer: {i4}/{i5}@{d2}", getContractName(contract._internalId), bidAdjustedSize, bidUnadjustedSize, ToDollars(bidPrice), askAdjustedSize, askUnadjustedSize, ToDollars(askPrice))
    }

    void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size) {
        sendGenericMessageToFront(ViperMessageType::AcceptedManualFill, internalId, size);
    }

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
    Vector<bool> _quoteExternal;
    SimpleMatrix<hf::price_t> _commissions; // row, col - all contracts, all contracts
    Vector<double> _commissionsInput;

    Vector<hf::price_t> _shortTermFairsForContracts;
    Vector<double> _shortTermFairsInVector;
    Vector<double> _longTermFairsInVector;
    bool _hasDataForCharts = false;

    Matrix _ODM;
    Matrix _midPoints;
    Matrix _overdeterminedPrices;
    Vector<double> _vectorMarketPrices[2];
    Vector<double>* _priorVectorMarketPrices;
    Vector<double>* _currentVectorMarketPrices;
    bool _calculatedVectorPrices = false;
    bool _canCalculateEma = false;

    Vector<bool> _useEMAInVector;
    Vector<bool> _setToMarket;
    Vector<uint64_t> _emaPeriodInVector;
    uint64_t _lastEmaTime;

    double _widthMultiplier = 0;

    md::queue::MDQueueResource* _inFromFront;
    md::queue::MDQueueResource* _inFromMM;
    md::queue::MDQueueResource* _outToFront;
    md::queue::MDQueueResource* _outToMM;

    std::vector<md::queue::MDQueue*> _marketDataQueues;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;
    std::set<uint32_t> _updatedContracts;

    UpdateSelector<RelativeValue> _updateSelector;

    SharedSystemState* _systemState;
    SystemState _oldSystemState = SystemState::LoggedOut;

    bool _canSendQuotes = true;

    om::client_id_t _id;
    double _frequency;

    std::unordered_map<uint32_t, double> _hedgeRequests;
    uint32_t _hedgeId = 0;
    bool _withinInternalFillsOfCycle = false;
    bool _hasPendingQuoteCalculation = false;
    int64_t _fillsTransactionNumber = 0;

    std::function<bool(uint32_t)> _pnlLimitsChecker;

    ViperConfig* _viperConfig;

    hf::queue::DebugQueue* _kdbLogger;

    uint64_t _nextInternalHeartbeat;
    uint64_t _internalHeartbeatPeriod;

    bool _firstCalculation = true;

    // ********** TEMP working structures TEMP *********** //
    Matrix _possibleHedgeRatios; // row, col - vectors, 3
    Matrix _hedgeVectorRiskExposures; // row, col - vectors, vectors
    Vector<double> _hedgeVectorRiskExposuresSums; // vectors
    Matrix _normalizationMatrix; // row, col - outrights, 3
    std::vector<ManualFill> _manualFills;
    // *************************************************** //
};

}

#endif //VIPER_RELATIVE_VALUE_H
