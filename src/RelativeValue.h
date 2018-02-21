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
#include "Logger.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>

namespace Python {

struct Lean {
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
    bool _isExternal = false;
    hf::price_t _price;
    double _size;
};

struct BidLeanSorter {
    bool operator()(Lean& a, Lean& b) const {
        if (a._size == 0)
            return false;
        if (b._size == 0)
            return true;
        return a._price > b._price;
    }
};

struct AskLeanSorter {
    bool operator()(Lean& a, Lean& b) const {
        if (a._size == 0)
            return false;
        if (b._size == 0)
            return true;
        return a._price < b._price;
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
        std::sort(_leans.begin(), end, BidLeanSorter());
    }

    void sortAks() {
        auto end = _leans.begin() + _leanIndex;
        std::sort(_leans.begin(), end, AskLeanSorter());
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
    std::array<Lean, 50> _leans;
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
    void createQuote(hf::price_t tickSize, uint32_t side, bool quoteExternal, Logger& logger, ContractsT& contracts, uint32_t contractId) {
        if (side == BID) {
            _leans.sortBids();
        } else {
            _leans.sortAks();
        }

        Lean& bestLean = _leans._leans[0];
        _bestPrice = bestLean._price;
        _bestSize = static_cast<hf::qty_t>(std::round(bestLean._size));
        _bestLeanIndex = bestLean._instrumentIndex;

        hf::price_t roundedPrice = hf::NULL_PRICE;
        double size = 0;
        uint32_t clubCount = 0;

        if (quoteExternal) {
            for (uint32_t leanIndex = 0; leanIndex < _leans.getNumberOfLeans(); ++leanIndex) {
                Lean& lean = _leans._leans[leanIndex];
                hf::price_t price = RoundToTickSize(lean._price, tickSize, side);
                if (roundedPrice == hf::NULL_PRICE) {
                    roundedPrice = price;
                    size = lean._size;
                    ++clubCount;
                    logger.logInfo(boost::str(boost::format("Adding %4% quote for lean. Lean=%1% Size=%2% Price=%3%") % contracts[lean._instrumentIndex]._definition._name % lean._size % ToDollars(lean._price) % contracts[contractId]._definition._name));
                } else if (roundedPrice == price) {
                    size += lean._size;
                    ++clubCount;
                    logger.logInfo(boost::str(boost::format("Adding %4% quote for lean. Lean=%1% Size=%2% Price=%3%") % contracts[lean._instrumentIndex]._definition._name % lean._size % ToDollars(lean._price) % contracts[contractId]._definition._name));
                } else {
                    break;
                }
            }

            _externalPrice = roundedPrice;
            _externalSize = static_cast<hf::qty_t>(std::round(size / clubCount));

            logger.logInfo(boost::str(boost::format("Quote. Contract=%1% Side=%2% Price=%3% TotalSize=%4%") % contracts[contractId]._definition._name % GetSideString(side) % ToDollars(_externalPrice) % _externalSize));
        }
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

    bool _traded = false;
    uint32_t _internalId;
};

class RelativeValue : public virtual hf::Service {

public:

    RelativeValue()
    :
        _updateSelector(*this)
    {}

    void onStratBegin() {
    }

    void onStratEnd() {
    }

    void onPostCreate(const libconfig::Setting& config) override;

    void onRun() override {
        _inFromFront->resetR();
        _inFromMM->resetR();
        _outToMM->resetW();
        _outToFront->resetW();

        subscribeToMarketData();

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            runCycle();
        }
    }

    void runCycle() {
        checkSystemState();
        const bool calculateDueToFront = drainFromFront();
        const bool hadInternalMarketData = drainFromExecutor();
        const bool hadExternalMarketData = drainAllMarketData();
        const bool calculateDueToHedges = processHedgeRequests();

        if (hadExternalMarketData) {
            recalculateAfterExternalMarketDataChange();
        }
        const bool recalculateQuotes = (canActOnMarketData() & (calculateDueToFront | hadInternalMarketData | hadExternalMarketData)) | calculateDueToHedges;
        if (recalculateQuotes) {
            calculateQuotes();
        }
    }

    bool processHedgeRequests() {
        const bool hasHedgeRequests = !_hedgeRequests.empty();
        if (hasHedgeRequests) {
            _logger.logInfo(boost::str(boost::format("Processing hedge request. %1%") % _hedgeRequests.size()));
            for (auto it = _hedgeRequests.begin(); it != _hedgeRequests.end(); ) {
                const uint32_t contractId = it->first;
                double size = it->second;
                double absSize = std::abs(size);
                const uint32_t side = size > 0 ? BID : ASK;
                RvContract& contract = _contracts[contractId];
                _logger.logInfo(boost::str(boost::format("Hedge request. %1% Size=%2% Side=%3%") % contract._definition._name % absSize % GetSideString(side)));
                const double leftToHedge = sendHedges(contract, absSize, side);
                const double hedgedSize = absSize - leftToHedge;
                addPosition(contract, hedgedSize, side, true);
                if (IsZero(leftToHedge)) {
                    it = _hedgeRequests.erase(it);
                    _logger.logInfo(boost::str(boost::format("Fully hedged. Name=%1%") % contract._definition._name));
                } else {
                    it->second = side == BID ? leftToHedge : -leftToHedge;
                    _logger.logInfo(boost::str(boost::format("Not fully hedged. Name=%1% Remaining=%2%") % contract._definition._name % leftToHedge));
                    ++it;
                }
            }
            calculateShortTermFairs();
        }
        return hasHedgeRequests;
    }

    void checkSystemState() {
        if (_systemState->_relativeValue != _oldSystemState) {
            onSysteStateChange();
            _oldSystemState = _systemState->_relativeValue;
        }
    }

    void onSysteStateChange() {
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
        }
    }

    void handleHardShutdown() {
        _logger.logInfo("RV hard shutdown");
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateHardShutdown);
    }

    void handleDisabled() {
        _logger.logInfo("RV got disabled");
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void handleLogin() {
        simulateMarketData();
        _logger.logInfo("RV Logged in");
    }

    void simulateMarketData() {
        simulateOneMarketData(14);
        simulateOneMarketData(15);
        simulateOneMarketData(17);
        simulateOneMarketData(18);
        simulateOneMarketData(7);
    }

    void simulateOneMarketData(hf::symbol_id_t sid) {
        md::MDUpdateEvent event;
        event.sid = sid;
        event.buy.updatedQtyNum = 1;
        auto& bid = event.buy.top.l[0];
        bid.pr = ToRhonePrice(.29);
        bid.q = 20;
        event.sell.updatedQtyNum = 1;
        auto& ask = event.sell.top.l[0];
        ask.pr = ToRhonePrice(.35);
        ask.q = 20;
        onMDUpdate(hf::VENUE::ICE, event);
    }

    void handleLogout() {
        _logger.logInfo("RV Logged out");
    }

    void handleCheckQuotes() {
        _logger.logInfo("RV check quotes");
        doInitialCalculations();
    }

    void handleEnabled() {
        _logger.logInfo("RV got enabled");
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled);
        doInitialCalculations();
    }

    void doInitialCalculations() {
        logVectorPositions();
        logContractPositions();
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
                    _logger.logInfo(boost::str(boost::format("Missing market on initial calculation of mid points. Name=%1%") % contract._definition._name));
                }
            }
        }
    }

    void handleQuotingEnabled() {
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Quoting), proto::SystemState::SystemStateEnabled);
    }

    void handleQuotingDisabled() {
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Quoting), proto::SystemState::SystemStateDisabled);
    }

    void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0) {
        auto &outMessage = _outToFront->pool()->getNextMessage();
        outMessage._type = type;

        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = value1;
        message._number_2 = value2;

        const auto outIdx = _outToFront->pool()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void sendErrorMessage(const std::string& s, ErrorType type) {
        auto &outMessage = _outToFront->pool()->getNextMessage();
        outMessage._type = ViperMessageType::ErrorMessage;
        ErrorMessage& message = reinterpret_cast<ErrorMessage &> (outMessage._data[0]);
        const uint32_t size = std::min(CRITICAL_MESSAGE_MAX_SIZE, (uint32_t) s.size());
        strncpy(message._message, s.c_str(), size);
        message._size = size;
        message._type = type;
        const auto outIdx = _outToFront->pool()->append(sizeof(ViperMessageType) + sizeof(ErrorMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void subscribeToMarketData() {
        bool proxyIdSet = _marketDataQueue->trySetProxyIdentifier(_id);
        if (!proxyIdSet) {
            throw std::runtime_error("Could not set proxy id on MD.");
        }

        int port;
        bool portOk = _marketDataQueue->tryGetMonitorPort(port);
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
            _logger.logInfo(boost::str(boost::format("RV subscribed md for. Contract=%1%") % contract._definition._name));
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
            auto& message = _inFromFront->pool()->getMessage(idx);

            recalculate = true;

            switch (message._type) {
                case ViperMessageType::GridUpdate: {
                    GridUpdateMessage& gridChange = reinterpret_cast<GridUpdateMessage &> (message._data[0]);
                    bool accepted = processGridChange(*gridChange._gridUpdate);
                    ViperMessageType type = accepted ? ViperMessageType::GridUpdateAccepted : ViperMessageType::GridUpdateRejected;
                    sendGenericMessageToFront(type, gridChange._gridUpdate->_update.gridid());
                    break;
                }
                case ViperMessageType::Quoting: {
                    GenericMessage& gm = reinterpret_cast<GenericMessage &> (message._data[0]);
                    SystemState state = (SystemState) gm._number_1;
                    _canQuoteExternally = state == SystemState::Enabled;
                    break;
                }
                default:
                    break;
            }
        }
        return recalculate;
    }

    bool drainFromExecutor() {
        bool hadInternalMarketData = false;
        while (true) {
            uint64_t idx = _inFromMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                break;

            _inFromMM->freeNext();
            auto& message = _inFromMM->pool()->getMessage(idx);

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
                    break;
                }
                case ViperMessageType::HedgeMDAdjsutAck: {
                    HedgeMarketDataAdjustmentAck& hedgeAck = reinterpret_cast<HedgeMarketDataAdjustmentAck &> (message._data[0]);
                    handleHedgeMarketDataAdjustmentAck(hedgeAck);
                    break;
                }
                default:
                    break;
            }
        }
        return hadInternalMarketData;
    }

    void handleHedgeMarketDataAdjustmentAck(HedgeMarketDataAdjustmentAck& hedgeAck) {
        RvContract& contract = _contracts[hedgeAck._instrumentIndex];
        _logger.logInfo(boost::str(boost::format("Received hedge md ack. %1% HedgeId=%2% Adj=%3% OrderId=%4%") % contract._definition._name % hedgeAck._hedgeId % hedgeAck._size % (uint32_t) hedgeAck._orderId));
        Market& market = contract._externalMarket[hedgeAck._side];
        market.ackAdjustment(hedgeAck._hedgeId, hedgeAck._size, hedgeAck._orderId);
    }

    void handleFill(FillMessage& fill) {
        RvContract& contract = _contracts[fill._contractId];
        _logger.logInfo(boost::str(boost::format("RV got filled. Contract=%1% Side=%2% Price=%3% Qty=%4%") % contract._definition._name % GetSideString(fill._side) % ToDollars(fill._price) % fill._qty));
        const hf::qty_t fillSize = fill._side == BID ? fill._qty : -fill._qty;
        auto hedgeIt = _hedgeRequests.find(fill._contractId);
        if (hedgeIt != _hedgeRequests.end()) {
            hedgeIt->second += fillSize;
        } else {
            _hedgeRequests.insert({fill._contractId, fillSize});
        }
    }

    bool processGridChange(GridUpdate& update) {
        return _updateSelector.processUpdate(update);
    }

    bool validateRvVectorParametersUpdate(GridUpdate& update) {
        return true;
    }

    bool onRvVectorParametersUpdate(GridUpdate& update) {
        bool valid = validateRvVectorParametersUpdate(update);

        if (!valid) {
            return false;
        }

        bool foundation = false;
        bool longTermFairs = false;
        bool shortTermFairs = false;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            VectorParametersColumns columnType = static_cast<VectorParametersColumns>(gridCell.col());
            std::string& vectorName = _vectorNames[gridCell.row()];
            switch (columnType) {
                case VectorParametersColumns::Range: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _vectorsRange.setValue(gridCell.row(), value);
                    foundation = true;
                    shortTermFairs = true;
                    _logger.logInfo(boost::str(boost::format("RV range changed. Vector=%1% Value=%2%") % vectorName % value));
                    break;
                }
                case VectorParametersColumns::MaxPos: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _vectorsMaxPosition.setValue(gridCell.row(), value);
                    foundation = true;
                    shortTermFairs = true;
                    _logger.logInfo(boost::str(boost::format("RV max_pos changed. Vector=%1% Value=%2%") % vectorName % value));
                    break;
                }
                case VectorParametersColumns::EMA_ON: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _useEMAInVector.setValue(gridCell.row(), value);
                    longTermFairs = true;
                    shortTermFairs = true;
                    _logger.logInfo(boost::str(boost::format("RV ema_on changed. Vector=%1% Value=%2%") % vectorName % value));
                    break;
                }
                case VectorParametersColumns::EMA_Time: {
                    uint32_t value = boost::lexical_cast<uint32_t>(gridCell.value());
                    uint64_t periodInCycles = calculatePeriodInCycles(value);
                    _emaPeriodInVector.setValue(gridCell.row(), value);
                    longTermFairs = true;
                    shortTermFairs = true;
                    _logger.logInfo(boost::str(boost::format("RV ema_time changed. Vector=%1% Value=%2%") % vectorName % value));
                    break;
                }
                case VectorParametersColumns::LTF: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _longTermFairsInVector.setValue(gridCell.row(), value);
                    shortTermFairs = true;
                    _logger.logInfo(boost::str(boost::format("RV ltf changed. Vector=%1% Value=%2%") % vectorName % value));
                    break;
                }
                case VectorParametersColumns::SetToMarket: {
                    bool value = 1 == boost::lexical_cast<uint32_t>(gridCell.value());
                    _setToMarket.setValue(gridCell.row(), value);
                    shortTermFairs = true;
                    _logger.logInfo(boost::str(boost::format("RV set_to_market changed. Vector=%1% Value=%2%") % vectorName % value));
                    break;
                }
                default:
                    break;
            }
        }

        if (foundation) {
            calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
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

    uint64_t calculatePeriodInCycles(uint32_t seconds) {
        return 0;
    }

    bool onRvContractParametersUpdate(GridUpdate& update) {
        bool valid = validateRvContractParametersUpdate(update);

        if (!valid) {
            return false;
        }

        bool foundation = false;

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(gridCell.col());
            RvContract& contract = _contracts[gridCell.row()];
            switch (columnType) {
                case ContractParameterRVColumns::QuoteExternal: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _quoteExternal.setValue(gridCell.row(), value);
                    _logger.logInfo(boost::str(boost::format("RV quote external changed. Contract=%1% Value=%2%") % contract._definition._name % value));
                    break;
                }
                case ContractParameterRVColumns::Commission: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _commissionsInput.setValue(gridCell.row(), value);
                    foundation = true;
                    _logger.logInfo(boost::str(boost::format("RV commission changed. Contract=%1% Value=%2%") % contract._definition._name % value));
                    break;
                }
                case ContractParameterRVColumns::Width: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _contractWidths.setValue(gridCell.row(), ToRhonePrice(value));
                    foundation = true;
                    _logger.logInfo(boost::str(boost::format("RV width changed. Contract=%1% Value=%2%") % contract._definition._name % value));
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

        return true;
    }

    bool validateRvContractParametersUpdate(GridUpdate& update) {
        return true;
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
            _logger.logInfo(boost::str(boost::format("RV lean_matrix changed. Contract=%1% Contract=%2% Value=%3%") % contractOne._definition._name % contractTwo._definition._name % value));
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
                    _logger.logInfo(boost::str(boost::format("RV use_weighted changed. Contract=%1% Value=%2%") % contract._definition._name % value));
                    break;
                }
                case PricingConfigsColumns::Weight: {
                    double value = boost::lexical_cast<double>(gridCell.value());
                    _weights.setValue(gridCell.row(), value);
                    calculateOverdeterminedMatrix();
                    _logger.logInfo(boost::str(boost::format("RV weight changed. Contract=%1% Value=%2%") % contract._definition._name % value));
                    break;
                }
                case PricingConfigsColumns::EstablishedLevel: {
                    hf::qty_t value = boost::lexical_cast<hf::qty_t>(gridCell.value());
                    contract._establishedLevel = value;
                    updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], gridCell.row(), contract._establishedLevel);
                    _logger.logInfo(boost::str(boost::format("RV established_level changed. Contract=%1% Value=%2%") % contract._definition._name % value));
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
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            ManualFillsEntryColumns columnType = static_cast<ManualFillsEntryColumns>(gridCell.col());
            ManualFill& fill = _manualFills._manualFills[gridCell.row()];

            switch (columnType) {
                case ManualFillsEntryColumns::Contract: {
                    fill._contract = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Size: {
                    fill._size = atoi(gridCell.value().c_str());
                    break;
                }
                case ManualFillsEntryColumns::Strategy: {
                    fill._size = atoi(gridCell.value().c_str());
                    break;
                }
            }
        }

        bool valid = _manualFills.validate();

        if (!valid) {
            return false;
        }

        for (auto& fill : _manualFills._manualFills) {
            if (fill._strategyId == _strategyId) {
                RvContract& contract = _contracts[fill._contract];
                uint32_t size = std::abs(fill._size);
                uint32_t side = fill._size > 0 ? BID : ASK;
                addPosition(contract, size, side, true);
            }
        }

        calculateShortTermFairs();

        return true;
    }

    bool onMarketMakerParametersUpdate(GridUpdate& update) { return true; }
    bool onStackerParametersUpdate(GridUpdate& update) { return true; }
    bool onPnlParametersUpdate(GridUpdate& update) { return true; }
    bool onGlobalConfigsUpdate(GridUpdate& update) { return true; }

    void addMarketDataQueue(md::queue::MDQueue* q) {
        _marketDataQueue = q;
    }

    bool drainAllMarketData() {
        bool handMarketData = false;
        while (true) {
            const auto idx = _marketDataQueue->getNext();

            if (BOOST_UNLIKELY(!idx))
                break;

            _marketDataQueue->freeNext();

            auto& batch = _marketDataQueue->getPoolP()->getBatch(idx);
            onMDBatch(batch);

            {
                _marketDataQueue->poolBytesRead(batch.size);
                uint64_t usage;
                if (BOOST_UNLIKELY(_marketDataQueue->poolThresholdExceeded(usage))) {
                    onPoolThresholdExceeded(_marketDataQueue->getQueueID(), usage);
                }
                if (BOOST_UNLIKELY(_marketDataQueue->queueThresholdExceeded(usage))) {
                    onQueueThresholdExceeded(_marketDataQueue->getQueueID(), usage);
                }
            }

            handMarketData = true;
        }

        return handMarketData;
    }

    void onPoolThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void onQueueThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void onMDBatch(const md::MDBatch &batch)  {
        _logger.logInfo(boost::str(boost::format("RV received MD batch. %1%") % batch.bid));
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
                    if (contract._definition._usedInPricing) {
                        std::string message = boost::str(boost::format("Contract %1% market data not working. Reason=%2%.") % contract._definition._name % toUInt(event.statusType));
                        // disableRelativeValue(message);
                    } else {
                        _logger.logInfo(boost::str(boost::format("Contract %1% market data not working. Reason=%2%. RV.") % contract._definition._name % toUInt(event.statusType)));
                    }
                    break;
                }
                case md::MDStatusEvent::TYPE::RECOVERED: {
                    std::string message = boost::str(boost::format("Contract %1% market data recovered. RV.") % contract._definition._name);
                    _logger.logInfo(message);
                    break;
                }
                case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                    switch (event.extStatus) {
                        case 'O': {
                            std::string message = boost::str(boost::format("Contract %1% market data - open. RV.") % contract._definition._name);
                            _logger.logInfo(message);
                            break;
                        }
                        case 'C': {
                            if (contract._definition._usedInPricing) {
                                std::string message = boost::str(boost::format("Contract %1% market data - closed. Used in pricing.") % contract._definition._name);
                                // disableRelativeValue(message);
                            } else {
                                _logger.logInfo(boost::str(boost::format("Contract %1% market closed. RV.") % contract._definition._name));
                            }
                        }
                        case 'E': {
                            if (contract._definition._usedInPricing) {
                                std::string message = boost::str(boost::format("Contract %1% market data - expired. Used in pricing.") % contract._definition._name);
                                // disableRelativeValue(message);
                            } else {
                                _logger.logInfo(boost::str(boost::format("Contract %1% market data - expired. RV.") % contract._definition._name));
                            }
                        }
                        case '1': {
                            std::string message = boost::str(boost::format("Contract %1% market data - entered pre-open. RV.") % contract._definition._name);
                            _logger.logInfo(message);
                            break;
                        }
                        default:
                            break;
                    }
                    break;
                }
                case md::MDStatusEvent::TYPE::UNKNOWN: {
                    std::string message = boost::str(boost::format("Contract %1% market data - received unknown md status. RV") % contract._definition._name);
                    sendErrorMessage(message, ErrorType::Critical);
                    _logger.logInfo(message);
                    break;
                }
            }
        }
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent &event) {
        disableRelativeValue("MD feed gapped.");
    }

    void handleFakDone(hf::VENUE venue, const md::MDFakDonenEvent& event) {
        _logger.logInfo(boost::str(boost::format("Received FAK done.")));
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
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            RvContract& contract = _contracts[internalId];

            const bool bidChanged = event.buy.updatedQtyNum == 1;
            if (bidChanged) {
                Market& bid = contract._externalMarket[BID];
                const md::top_t::level_t& level = event.buy.top.l[0];
                bid.update(level.pr, level.q);
            }

            const bool askChanged = event.sell.updatedQtyNum == 1;
            if (askChanged) {
                Market& ask = contract._externalMarket[ASK];
                const md::top_t::level_t& level = event.sell.top.l[0];
                ask.update(level.pr, level.q);
            }

            {
                if (bidChanged | askChanged) {
                    logCurrentExternalMarket(contract);
                    _updatedContracts.insert(internalId);
                    if (contract._definition._usedInPricing) {
                        const bool missingBook = (contract._externalMarket[BID].getPrice() == hf::NULL_PRICE) | (contract._externalMarket[ASK].getPrice() == hf::NULL_PRICE);
                        if (missingBook) {
                            _logger.logInfo(boost::str(boost::format("Lost book for contract. %1%") % contract._definition._name));
                            disableRelativeValue(boost::str(boost::format("Do not have market data for pricing. Contract=%1%") % contract._definition._name));
                        }
                    }
                }
            }
        } else {
            _logger.logError(boost::str(boost::format("Received book update for unrecognized SID='%1%'") % event.sid));
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
        std::string message = boost::str(boost::format("Disabling Relative Value. Reason=%1%") % reason);
        _logger.logInfo(message);
        sendErrorMessage(message, ErrorType::Critical);
        _systemState->_relativeValue = SystemState::Disabled;
        handleDisabled();
    }

    void processInternalMarketUpdate(MarketUpdate& update) {
        RvContract& contract = _contracts[update._instrumentIndex];
        Market& market = contract._internalMarkets[update._side].getMarket(update._marketId);
        market.update(update._price, update._size);
    }

    void updateMidPoint(Market& bid, Market& offer, uint32_t instumentIndex, hf::qty_t establishedLevel) {
        _logger.logInfo(boost::str(boost::format("Updating midpoint for %1%") % instumentIndex));
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
        _logger.logInfo("CalculateOverdeterminedPrices");
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
            double sum = 0;
            for (uint32_t allIndex = 0; allIndex < _numberOfAllContracts; ++allIndex) {
                sum += _ODM.getValue(outrightIndex, allIndex) * _midPoints.getValue(0, allIndex);
            }
            _overdeterminedPrices.setValue(0, outrightIndex, sum);
        }
    }

    void calculateVectorMarketPrices() {
        _logger.logInfo("calculateVectorMarketPrices");
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = 0;
            for (uint32_t outrightIndex = 0; outrightIndex < _numberOfPricingContracts; ++outrightIndex) {
                sum += _pricingMatrix.getValue(vectorIndex, outrightIndex) * _overdeterminedPrices.getValue(0, outrightIndex);
            }
            _newVectorMarketPrices.setValue(vectorIndex, sum);
        }
        sendOverdeterminedMarketToGUI();
    }

    void calculateLongTermFairsInVector() {
        _logger.logInfo("calculateLongTermFairsInVector");

        const uint64_t now = clockUtil::GetCycles();
        const uint64_t timeInCycles = now - _lastEmaTime;

        if (_hasVectorPrices) {
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                const bool useEma = _useEMAInVector.getValue(vectorIndex);
                if (useEma) {
                    const double oldMarketPrice = _oldVectorMarketPrices.getValue(vectorIndex);
                    const uint64_t period = _emaPeriodInVector.getValue(vectorIndex);
                    const double alpha = static_cast<double>(now) / period;
                    const double mu = std::exp(-alpha);
                    const double nu = (1 - mu) / alpha;
                    const double lastEma = _longTermFairsInVector.getValue(vectorIndex);
                    const double newEma = mu * lastEma + (nu - mu) * _oldVectorMarketPrices.getValue(vectorIndex) + (1 - nu) * _newVectorMarketPrices.getValue(vectorIndex);
                    _longTermFairsInVector.setValue(vectorIndex, newEma);
                }
            }
        }

        _oldVectorMarketPrices = _newVectorMarketPrices;
        _hasVectorPrices = true;

        sendLtfToGUI();
    }

    double calculateWeightedMidpoint(Market& bid, Market& offer, hf::qty_t establishedLevel) {
        const hf::qty_t offerSize = std::min(offer.getUnAdjustedSize(), establishedLevel);
        const hf::qty_t bidSize = std::min(bid.getUnAdjustedSize(), establishedLevel);
        return (ToDollars(offer.getPrice()) * bidSize + ToDollars(bid.getPrice()) * offerSize) / (bidSize + offerSize);
    }

    double calculateSimpleAverage(Market& bid, Market& offer) {
        return (ToDollars(bid.getPrice()) + ToDollars(offer.getPrice())) / 2;
    }

    void init(ViperConfig& viperConfig, ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromMM, ViperQueue* outToMM, double frequency, SharedSystemState& systemState) {
        initializeModel(viperConfig);
        initializeQueues(inFromFront, outToFront, inFromMM, outToMM);
        _frequency = frequency;
        _systemState = &systemState;
    }

    void initializeQueues(ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromMM, ViperQueue* outToMM) {
        _inFromFront = inFromFront;
        _outToFront = outToFront;
        _inFromMM = inFromMM;
        _outToMM = outToMM;
    }

    void initializeModel(ViperConfig& config) {
        _maxPercentOfWidth = boost::lexical_cast<double>(config.getGlobal("RVQuoteMaxPercentWidth"));
        initializeContracts(config);
        config.populatePricingMatrix(_pricingMatrix);
        _riskMatrix = _pricingMatrix.inverse().transpose();
        initializeVectorStructures(config._vectorConfigs);
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        initializeFromRelativeValue(config._relativeValueConfigs);
        config._indexOfFirstTradedContract = _indexOfFirstTradedContract;
        initializeLeanMatrix(config._leanMatrix);
        initializeFromPricingConfig(config._pricingConfigs);
        initializeStructures();
        calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
        _overdeterminedPrices.initialize(1, _numberOfPricingContracts);
        _midPoints.initialize(1, _numberOfAllContracts);
        calculateOverdeterminedMatrix();
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
            _useWeightedMid.setValue(index, pc._establishedLevel == 1);
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
        _vectorsRange.init(size);
        _vectorsMaxPosition.init(size);
        _fareMoveForOneUnitPosition.init(size);
        _oldVectorMarketPrices.init(size);
        _newVectorMarketPrices.init(size);
        _useEMAInVector.init(size);
        _setToMarket.init(size);
        _emaPeriodInVector.init(size);
        _longTermFairsInVector.init(size);
        _positionsInVector.init(size);

        for (uint32_t index = 0; index < size; ++index) {
            VectorConfig& vc = config[index];
            _vectorsRange.setValue(index, vc._range);
            _vectorsMaxPosition.setValue(index, vc._maxPos);
            _setToMarket.setValue(index, vc._setToMarket);
            _useEMAInVector.setValue(index, vc._useEMA);
            _emaPeriodInVector.setValue(index, vc._EMALength);
            _longTermFairsInVector.setValue(index, vc._longTermFair);
            _vectorNames.push_back(vc._vectorName);
            _positionsInVector.setValue(index, 0);
            _fareMoveForOneUnitPosition.setValue(index, 0);
            _oldVectorMarketPrices.setValue(index, 0);
            _newVectorMarketPrices.setValue(index, 0);
        }
    }

    void initializeStructures() {
        _hedgeRatios.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _quoteSizes.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _numberOfVectors = _riskMatrix._numberOfColumns;
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
                    addPosition(*contract, std::abs(rvConfig._position), side, true);
                }
                _contractPositions.setValue(contract->_internalId, rvConfig._position);
                _quoteExternal.setValue(contract->_internalId, rvConfig._quoteExternal);
                contract->_traded = true;
            } else {
                _logger.logErrorAndThrow("Got unknown contract when initializing rel value");
            }
        }
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
        }
        _numberOfAllContracts = config._allContracts.size();
        _numberOfPricingContracts = config._numberOfPricingContracts;
    }

    void calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes() {
        for (uint32_t rowIndex = _indexOfFirstTradedContract; rowIndex < _numberOfAllContracts; ++rowIndex) {
            for (uint32_t columnIndex = _indexOfFirstTradedContract; columnIndex < _numberOfAllContracts; ++columnIndex) {
                double hedgeRatio = 0;
                if (rowIndex != columnIndex) {
                    hedgeRatio = calculateHedgeRatio(rowIndex, columnIndex);
                } else {
                    calculateVectorRiskExposureForContracts(rowIndex, columnIndex);
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

    double calculateHedgeRatio(uint32_t contractOneIndex, uint32_t contractTwoIndex) {
        calculateVectorRiskExposureForContracts(contractOneIndex, contractTwoIndex);
        calculatePossibleHedgeRatios();
        calculateVectorRisks();
        sumUpVectorRisks();
        return pickBestHedge();
    }

    void calculateVectorRiskExposureForContract(uint32_t tableColumn, ContractDefinition& contractDefinition) {
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double exposure = 0;
            if (contractDefinition._numberOfLegs > 1) {
                for (uint32_t legIndex = 0; legIndex < contractDefinition._numberOfLegs; ++legIndex) {
                    ContractDefinition::Leg& leg = contractDefinition._legs[legIndex];
                    double vectorRik = _riskMatrix.getValue(vectorIndex, leg._index);
                    exposure += vectorRik * leg._size;
                }
            } else {
                exposure = _riskMatrix.getValue(vectorIndex, contractDefinition._internalId);
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

    double pickBestHedge() {
        uint32_t minIndex = 0;
        double min = _hedgeVectorRiskExposuresSums.getValue(0);
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double sum = _hedgeVectorRiskExposuresSums.getValue(vectorIndex);
            if (sum < min) {
                min = sum;
                minIndex = vectorIndex;
            }
        }
        return _possibleHedgeRatios.getValue(minIndex, 2);
    }

    double calculateCommission(uint32_t contractOneIndex, uint32_t contractTwoIndex, double hedge) {
        const double commissionOne = _commissionsInput.getValue(contractOneIndex);
        const double commissionTwo = _commissionsInput.getValue(contractTwoIndex);
        const double normalizedValue = findNormalizer();
        return (commissionOne + std::abs(hedge) * commissionTwo) / normalizedValue;
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
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const double riskExposure = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractOneRisk)
                                        - _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractTwoRisk) * hedgeRatio;
            const double fairMove = _fareMoveForOneUnitPosition.getValue(vectorIndex);
            oneLotMoves.setValue(vectorIndex, riskExposure * fairMove);
        }
        Vector<double> fairMoveForContract;
        fairMoveForContract.init(_numberOfAllContracts);
        for (uint32_t contractIndex = 0; contractIndex < _numberOfPricingContracts; ++contractIndex) {
            double move = 0;
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                move += oneLotMoves.getValue(vectorIndex) * _riskMatrix.getValue(vectorIndex, contractIndex);
            }
            fairMoveForContract.setValue(contractIndex, move);
        }

        double maxMoveAsPercentage = std::numeric_limits<double>::min();

        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            double move = 0;
            RvContract& contract = _contracts[spreadIndex];
            for (uint32_t legIndex = 0; legIndex < contract._definition._numberOfLegs; ++legIndex) {
                auto& leg = contract._definition._legs[legIndex];
                const double legMove = fairMoveForContract.getValue(leg._index) * leg._size;
                move += legMove;
            }
            double moveAsPercentage = std::abs(move) / ToDollars(_contractWidths.getValue(spreadIndex));
            maxMoveAsPercentage = std::max(maxMoveAsPercentage, moveAsPercentage);
        }

        RvContract& quoteContract = _contracts[contractOneIndex];
        RvContract& leanContract = _contracts[contractTwoIndex];

        double quoteSize = _maxPercentOfWidth / maxMoveAsPercentage;

        _logger.logInfo(boost::str(boost::format("Calculated quote size. Contract=%1% Lean=%2% Size=%3%") % quoteContract._definition._name % leanContract._definition._name % quoteSize));

        return quoteSize;
    }

    void sendPositionUpdateToGUI(uint32_t contractIndex) {
        sendGenericMessageToFront(ViperMessageType::PositionUpdate, contractIndex);
    }

    void sendShortTermFairsToGUI() {
        sendGenericMessageToFront(ViperMessageType::STFUpdate);
    }

    void sendQuotesToGUI() {
        sendGenericMessageToFront(ViperMessageType::QuotesUpdate);
    }

    void sendOverdeterminedMarketToGUI() {
        sendGenericMessageToFront(ViperMessageType::OverdeterminedPricesUpdate);
    }

    void sendLtfToGUI() {
        sendGenericMessageToFront(ViperMessageType::LTFUpdate);
    }

    double sendHedges(RvContract& contract, double size, uint32_t side) {
        _logger.logInfo(boost::str(boost::format("Send hedges for fill. %1% Size=%2% Side=%3%") % contract._definition._name % size % GetSideString(side)));
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
                _logger.logInfo(boost::str(boost::format("Hedging %1% in %2% size %3% hedge ratio %4% need to hedge %5%")
                                           % contract._definition._name % leanContract._definition._name % needToHedge % hedgeRatio % needToHedge));
                hf::qty_t orderSize;
                Market& externalLeanMarket = leanContract._externalMarket[side];
                getOrderSize(hedgeRatio, orderSize, externalLeanMarket.getFakAdjustedSize(), needToHedge, lean._size);

                if (orderSize > 0) {
                    const uint32_t otherSide = GetOtherSide(side);

                    hf::price_t hedgePrice;
                    uint32_t hedgeId = getHedgeId();
                    if (lean._isExternal) {
                        hedgePrice = externalLeanMarket.getPrice();
                        externalLeanMarket.addAdjustment(hedgeId, orderSize);
                    } else {
                        hedgePrice = leanContract._internalMarkets[side].getMarket(lean._marketIndex).getPrice();
                    }

                    _logger.logInfo(boost::str(boost::format("Sending hedge. Contract=%1% Side=%2% Price=%3% Size=%4% External=%5%")
                                               % leanContract._definition._name % GetSideString(otherSide) % ToDollars(hedgePrice) % orderSize % (int) lean._isExternal));

                    sendHedgeOrder(leanContract._internalId, otherSide, hedgePrice, orderSize, lean._isExternal, hedgeId);
                    addPosition(leanContract, orderSize, otherSide, false);

                    sentHedge = true;
                }

                if (needToHedge <= 0) {
                    _logger.logInfo(boost::str(boost::format("Fully hedged. Contract=%1% Side=%2%.") % leanContract._definition._name % GetSideString(side)));
                    break;
                }
            } else {
                if (sentHedge) {
                    _logger.logInfo(boost::str(boost::format("Not Fully hedged but sent hedge so will recalculate. Contract=%1% Side=%2% NeedToHedge=%3%.") % leanContract._definition._name % GetSideString(side) % needToHedge));
                } else {
                    _logger.logInfo(boost::str(boost::format("Not Fully hedged and did not send hedge. Setting needToHedge=0. Contract=%1% Side=%2% NeedToHedge=%3%.") % leanContract._definition._name % GetSideString(side) % needToHedge));
                    needToHedge = 0;
                }
                break;
            }
        }
        return needToHedge;
    }

    uint32_t getHedgeId() {
        return ++_hedgeId;
    }

    void sendHedgeOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, bool external, uint32_t hedgeId) {
        auto &outMessage = _outToMM->pool()->getNextMessage();
        outMessage._type = ViperMessageType::HedgeOrder;

        HedgeOrderMessage& message = reinterpret_cast<HedgeOrderMessage &> (outMessage._data[0]);
        message._instrumentIndex = internalId;
        message._side = side;
        message._price = price;
        message._size = size;
        message._isExternal = external;
        message._hedgeId = hedgeId;

        const auto outIdx = _outToMM->pool()->append(sizeof(ViperMessageType) + sizeof(HedgeOrderMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void getOrderSize(double hedgeRatio, hf::qty_t& orderSize, hf::qty_t minAvailableLeanSize, double& needToHedge, double size) {
        double maxHedgeSize = size * hedgeRatio;
        double availableLeanSize = minAvailableLeanSize;
        if (availableLeanSize > maxHedgeSize) {
            availableLeanSize = maxHedgeSize;
        }
        double needToHedgeInHedgeTerms = needToHedge * hedgeRatio;
        _logger.logInfo(boost::str(boost::format("Need to hedge in terms of lean. %1% MinAvailable=%2% Cap=%3% Available=%4%")
                                                                % needToHedgeInHedgeTerms % minAvailableLeanSize % maxHedgeSize % availableLeanSize));
        if (needToHedgeInHedgeTerms >= availableLeanSize) {
            orderSize = static_cast<hf::qty_t>(std::round(availableLeanSize));
        } else {
            orderSize = static_cast<hf::qty_t>(std::round(needToHedgeInHedgeTerms));
        }
        needToHedge = (needToHedgeInHedgeTerms - orderSize) / hedgeRatio;

        _logger.logInfo(boost::str(boost::format("Hedge OrderSize=%1% Remaining NeedToHedge=%2%") % orderSize % needToHedge));
    }

    void addPosition(RvContract& contract, double size, uint32_t side, bool checkLimit) {
        _logger.logInfo(boost::str(boost::format("Adding position. Contract=%1% Side=%2% Size=%3%") % contract._definition._name % GetSideString(side) % size));
        const double positionChange = side == BID ? size : -size;
        addPositionToVector(contract, positionChange, checkLimit);
        _contractPositions.add(contract._definition._internalId, positionChange);
        logVectorPositions();
        logContractPositions();
        sendPositionUpdateToGUI(contract._definition._internalId);
    }

    void addPositionToVector(RvContract& contract, double positionChange, bool checkLimit) {
        if (contract._definition._numberOfLegs > 1) {
            for (uint32_t index = 0; index < contract._definition._numberOfLegs; ++index) {
                ContractDefinition::Leg& leg = contract._definition._legs[index];
                const double legPositionChange = positionChange * leg._size;
                for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                    const double changeInVector = _riskMatrix.getValue(vectorIndex, leg._index) * legPositionChange;
                    _positionsInVector.add(vectorIndex, changeInVector);
                }
            }
            if (checkLimit) {
                for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                    checkVectorPositionLimit(vectorIndex);
                }
            }
        } else {
            for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                const double changeInVector = _riskMatrix.getValue(vectorIndex, contract._definition._internalId) * positionChange;
                _positionsInVector.add(vectorIndex, changeInVector);
                if (checkLimit) {
                    checkVectorPositionLimit(vectorIndex);
                }
            }
        }
    }

    void checkVectorPositionLimit(uint32_t vectorIndex) {
        const double vectorPosition = _positionsInVector.getValue(vectorIndex);
        const double limit = _vectorsMaxPosition.getValue(vectorIndex);
        if (vectorPosition > limit) {
            const std::string message = boost::str(boost::format("Vector position max breached. Vector=%1% Position=%2% Limit=%3%") % _vectorNames.at(vectorIndex) % vectorPosition % limit);
            disableRelativeValue(message);
        }
    }

    void logContractPositions() {
        std::stringstream message;
        message << "Contract positions: ";
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            ContractDefinition& contractDefinition = _contracts[spreadIndex]._definition;
            double value = _contractPositions.getValue(spreadIndex);
            message << contractDefinition._name << "=" << value << ", ";
        }
        _logger.logInfo(message.str());
    }

    void logVectorPositions() {
        std::stringstream message;
        message << "Vector positions: ";
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double value = _positionsInVector.getValue(vectorIndex);
            message << _vectorNames.at(vectorIndex) << "=" << value << ", ";
        }
        _logger.logInfo(message.str());
    }

    void calculateShortTermFairs() {
        _logger.logInfo("calculateShortTermFairs");

        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const bool setToMarket = _setToMarket.getValue(vectorIndex);
            double value;
            if (!setToMarket) {
                value = _longTermFairsInVector.getValue(vectorIndex) - _positionsInVector.getValue(vectorIndex) * _fareMoveForOneUnitPosition.getValue(vectorIndex);
            } else {
                value = _newVectorMarketPrices.getValue(vectorIndex);
            }
            _shortTermFairsInVector.setValue(vectorIndex, value);
        }
        for (uint32_t contractIndex = 0; contractIndex < _numberOfPricingContracts; ++contractIndex) {
            double shortTermFair = 0;
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                const double value = _shortTermFairsInVector.getValue(vectorIndex) * _riskMatrix.getValue(vectorIndex, contractIndex);
                shortTermFair += value;
            }
            const hf::price_t rhonePrice = ToRhonePrice(shortTermFair);
            _shortTermFairsForContracts.setValue(contractIndex, rhonePrice);
        }
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            ContractDefinition& contractDefinition = _contracts[spreadIndex]._definition;
            hf::price_t shortTermFair = 0;
            for (ContractDefinition::Leg& leg : contractDefinition._legs) {
                hf::price_t value = _shortTermFairsForContracts.getValue(leg._index) * leg._size;
                shortTermFair += value;
            }
            const hf::price_t rhonePrice = shortTermFair;
            _shortTermFairsForContracts.setValue(spreadIndex, rhonePrice);
        }
        logShortTermFairsForVectors();
        logShortTermFairsForContracts();
        sendShortTermFairsToGUI();
    }

    void logShortTermFairsForVectors() {
        std::stringstream message;
        message << "Vector short term fairs: ";
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            double value = _shortTermFairsInVector.getValue(vectorIndex);
            message << _vectorNames.at(vectorIndex) << "=" << value << ", ";
        }
        _logger.logInfo(message.str());
    }

    void logShortTermFairsForContracts() {
        std::stringstream message;
        message << "Contract short term fairs: ";
        for (uint32_t spreadIndex = _indexOfFirstTradedContract; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            ContractDefinition& contractDefinition = _contracts[spreadIndex]._definition;
            double value = ToDollars(_shortTermFairsForContracts.getValue(spreadIndex));
            message << contractDefinition._name << "=" << value << ", ";
        }
        _logger.logInfo(message.str());
    }

    void calculateQuotes() {
        _logger.logInfo("CalculateQuotes");
        logCurrentExternalMarkets();
        for (uint32_t instrumentIndex = 0; instrumentIndex < _numberOfAllContracts; ++instrumentIndex) {
            RvContract& contract = _contracts[instrumentIndex];
            contract.clearLeans();
            if (contract._traded) {
                _logger.logInfo(boost::str(boost::format("Calculating quotes for %1%") % contract._definition._name));
                for (uint32_t leanIndex = 0; leanIndex < _numberOfAllContracts; ++leanIndex) {
                    const bool canLeanOn = _leanMatrix.getValue(instrumentIndex, leanIndex);
                    if (canLeanOn) {
                        const hf::price_t fair = _shortTermFairsForContracts.getValue(instrumentIndex);
                        const hf::price_t width = _contractWidths.getValue(instrumentIndex);
                        const hf::price_t leanFair = _shortTermFairsForContracts.getValue(leanIndex);
                        const double maxQuoteSize = _quoteSizes.getValue(contract._definition._internalId, leanIndex);

                        RvContract& leanContract = _contracts[leanIndex];

                        Markets& leanBids = leanContract._internalMarkets[BID];
                        for (uint32_t leanBidIndex = 0; leanBidIndex < leanBids._numberOfMarkets; ++leanBidIndex) {
                            Market& market = leanBids._markets[leanBidIndex];
                            calculateBidQuote(market, contract, leanIndex, leanFair, fair, width, maxQuoteSize, false);
                        }

                        if (_canQuoteExternally) {
                            Market& externalBid = leanContract._externalMarket[BID];
                            calculateBidQuote(externalBid, contract, leanIndex, leanFair, fair, width, maxQuoteSize, true);
                        }

                        Markets& leanOffers = leanContract._internalMarkets[ASK];
                        for (uint32_t leanBidIndex = 0; leanBidIndex < leanOffers._numberOfMarkets; ++leanBidIndex) {
                            Market& market = leanOffers._markets[leanBidIndex];
                            calculateAskQuote(market, contract, leanIndex, leanFair, fair, width, maxQuoteSize, false);
                        }

                        if (_canQuoteExternally) {
                            Market& externalAsk = leanContract._externalMarket[ASK];
                            calculateAskQuote(externalAsk, contract, leanIndex, leanFair, fair, width, maxQuoteSize, true);
                        }
                    }
                }

                const bool quoteExternal = _quoteExternal.getValue(instrumentIndex);

                Quote& bidQuote = contract._quotes[BID];
                bidQuote.createQuote(contract._definition._tickSize, BID, quoteExternal, _logger, _contracts, instrumentIndex);

                Quote& askQuote = contract._quotes[ASK];
                askQuote.createQuote(contract._definition._tickSize, ASK, quoteExternal, _logger, _contracts, instrumentIndex);

                sendQuoteOrder(contract);
            }
        }
        sendQuotesToGUI();
    }

    void sendQuoteOrder(RvContract& contract) {
        if (canExecute()) {
            auto& outMessage = _outToMM->pool()->getNextMessage();
            outMessage._type = ViperMessageType::QuoteOrder;

            QuoteOrderMessage& message = reinterpret_cast<QuoteOrderMessage&> (outMessage._data[0]);
            message._instrumentIndex = contract._definition._internalId;

            auto& messageBid = message._quotes[BID];
            Quote& bidQuote = contract._quotes[BID];
            messageBid._externalPrice = bidQuote._externalPrice;
            messageBid._externalSize = bidQuote._externalSize;
            messageBid._internalMarketPrice = bidQuote._bestPrice;
            messageBid._internalMarketSize = bidQuote._bestSize;

            auto& messageAsk = message._quotes[ASK];
            Quote& askQuote = contract._quotes[ASK];
            messageAsk._externalPrice = askQuote._externalPrice;
            messageAsk._externalSize = askQuote._externalSize;
            messageAsk._internalMarketPrice = askQuote._bestPrice;
            messageAsk._internalMarketSize = askQuote._bestSize;

            const auto outIdx = _outToMM->pool()->append(sizeof(ViperMessageType) + sizeof(QuoteOrderMessage));
            mb();
            _outToMM->appendNew(outIdx);
        } else {
            _logger.logInfo(boost::str(boost::format("Could not send RV quote for %1%. System Disabled.") % contract._definition._name));
        }
    }

    void calculateBidQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double maxQuoteSize, bool isExternal) {
        if (leanMarket.getPrice() != hf::NULL_PRICE) {
            const hf::price_t bidHedgingCost = leanFair - leanMarket.getPrice();
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t commission = _commissions.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t hedgingCost = calculateHedgingCost(width, bidHedgingCost, hedgeRatio);
            const hf::price_t bidPrice = fair - width - hedgingCost - commission;
            const double size = calculateSize(contract, ASK, hedgeRatio, leanMarket.getFakAdjustedSize(), maxQuoteSize);
            contract._quotes[BID]._leans.add(leanIndex, leanMarket.getId(), bidPrice, size, isExternal);
        }
    }

    void calculateAskQuote(Market& leanMarket, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, double maxQuoteSize,  bool isExternal) {
        if (leanMarket.getPrice() != hf::NULL_PRICE) {
            const hf::price_t askHedgingCost = leanMarket.getPrice() - leanFair;
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t hedgingCost = calculateHedgingCost(width, askHedgingCost, hedgeRatio);
            const hf::price_t askPrice = fair + width + hedgingCost + _commissions.getValue(contract._definition._internalId, leanIndex);
            const double size = calculateSize(contract, BID, hedgeRatio, leanMarket.getFakAdjustedSize(), maxQuoteSize);
            contract._quotes[ASK]._leans.add(leanIndex, leanMarket.getId(), askPrice, size, isExternal);
        }
    }

    hf::price_t calculateHedgingCost(hf::price_t width, hf::price_t cost, double hedgeRatio) {
        const hf::price_t hedgingCost = static_cast<hf::price_t>(cost * hedgeRatio);
        if (hedgingCost < 0) {
            const hf::price_t maxHedgingCost = static_cast<hf::price_t>(_maxPercentOfWidth * width);
            if (-hedgingCost > maxHedgingCost) {
                return -maxHedgingCost;
            }
        }
        return hedgingCost;
    }

    double calculateSize(RvContract& contract, uint32_t side, double hedgeRatio, double marketSize, double maxQuoteSize) {
        double size = 0;
        if (hedgeRatio > 0) {
            const double possibleSize = marketSize / hedgeRatio;
            if (possibleSize > maxQuoteSize) {
                size = maxQuoteSize;
            } else {
                size = possibleSize;
            }
            _logger.logInfo(boost::str(boost::format("Calculated lean size. Contract=%1% Side=%2% Market=%3% Max=%4% PossibleSize=%5% Size=%6% HedgeRatio=%7%")
                                       % contract._definition._name % GetSideString(side) % marketSize % maxQuoteSize % possibleSize % size % hedgeRatio));
        } else {
            _logger.logInfo(boost::str(boost::format("Calculated lean size. Contract=%1% Sid=%2% Market=%3% Max=%4% Size=%5% HedgeRatio in not positive. Ratio=%6%")
                                       % contract._definition._name % GetSideString(side) % marketSize % maxQuoteSize % size % hedgeRatio));
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
        std::string bidString;
        Market& bid = contract._externalMarket[BID];
        if (bid.getPrice() == hf::NULL_PRICE) {
            bidString = "missing";
        } else {
            bidString.append(std::to_string(bid.getUnAdjustedSize())).append("/").append(std::to_string(bid.getFakAdjustedSize())).append("@").append(std::to_string(ToDollars(bid.getPrice())));
        }

        std::string offerString;
        Market& offer = contract._externalMarket[ASK];
        if (offer.getPrice() == hf::NULL_PRICE) {
            offerString = "missing";
        } else {
            offerString.append(std::to_string(offer.getUnAdjustedSize())).append("/").append(std::to_string(offer.getFakAdjustedSize())).append("@").append(std::to_string(ToDollars(offer.getPrice())));
        }

        _logger.logInfo(boost::str(boost::format("Market: Contract=%1% Bid: %2% Offer: %3%") % contract._definition._name % bidString % offerString));
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

    SimpleMatrix<double> _hedgeRatios; // x,y - outrights, spreads - square
    Matrix _quoteSizes; // x,y - outrights, spreads - square

    std::vector<RvContract> _contracts; // outrights, spreads
    uint32_t _numberOfAllContracts;
    uint32_t _numberOfPricingContracts;
    uint32_t _indexOfFirstTradedContract;

    Vector<double> _contractPositions; // outrights
    Vector<double> _positionsInVector; // vectors

    std::vector<std::string> _vectorNames;

    SimpleMatrix<bool> _leanMatrix;
    Vector<bool> _quoteExternal;
    SimpleMatrix<hf::price_t> _commissions; // row, col - all contracts, all contracts
    Vector<double> _commissionsInput;

    Vector<hf::price_t> _shortTermFairsForContracts;
    Vector<double> _shortTermFairsInVector;
    Vector<double> _longTermFairsInVector;

    Matrix _ODM;
    Matrix _midPoints;
    Matrix _overdeterminedPrices;
    Vector<double> _oldVectorMarketPrices;
    Vector<double> _newVectorMarketPrices;
    bool _hasVectorPrices = true;

    Vector<bool> _useEMAInVector;
    Vector<bool> _setToMarket;
    Vector<uint64_t> _emaPeriodInVector;
    uint64_t _lastEmaTime;

    double _maxPercentOfWidth = 0;

    ViperQueue* _inFromFront;
    ViperQueue* _inFromMM;
    ViperQueue* _outToFront;
    ViperQueue* _outToMM;

    md::queue::MDQueue * _marketDataQueue;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;
    std::set<uint32_t> _updatedContracts;

    UpdateSelector<RelativeValue> _updateSelector;

    uint32_t _strategyId;

    SharedSystemState* _systemState;
    SystemState _oldSystemState = SystemState::LoggedOut;

    bool _canQuoteExternally = true;

    om::client_id_t _id;
    double _frequency;

    std::unordered_map<uint32_t, double> _hedgeRequests;
    uint32_t _hedgeId = 0;

    Logger _logger;

    // ********** TEMP working structures TEMP *********** //
    Matrix _possibleHedgeRatios; // row, col - vectors, 3
    Matrix _hedgeVectorRiskExposures; // row, col - vectors, vectors
    Vector<double> _hedgeVectorRiskExposuresSums; // vectors
    Matrix _normalizationMatrix; // row, col - outrights, 3
    ManualFillsCollection _manualFills;
    // *************************************************** //
};

}

#endif //VIPER_RELATIVE_VALUE_H
