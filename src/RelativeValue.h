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
#include "Logger.h"

namespace Python {

struct Lean {
    uint32_t _instrumentIndex;
    uint32_t _marketIndex;
    bool _isExternal = false;
    hf::price_t _price;
    hf::qty_t _size;
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

    void add(uint32_t leanIndex, uint32_t marketIndex, hf::price_t price, hf::qty_t size, bool isExternal) {
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

    hf::price_t _bestPrice;
    hf::qty_t _bestSize;
    uint32_t _bestLeanIndex;

    hf::price_t _externalPrice;
    hf::qty_t _externalSize;

    template <typename ContractsT>
    void createQuote(hf::price_t tickSize, uint32_t side, bool quoteExternal, Logger& logger, ContractsT& contracts, uint32_t contractId) {
        if (side == BID) {
            _leans.sortBids();
        } else {
            _leans.sortAks();
        }

        Lean& bestLean = _leans._leans[0];
        _bestPrice = bestLean._price;
        _bestSize = bestLean._size;
        _bestLeanIndex = bestLean._instrumentIndex;

        hf::price_t roundedPrice = hf::NULL_PRICE;
        hf::qty_t size = 0;

        if (quoteExternal) {
            for (uint32_t leanIndex = 0; (leanIndex < 3) & (leanIndex < _leans.getNumberOfLeans()); ++leanIndex) {
                Lean& lean = _leans._leans[leanIndex];
                hf::price_t price = RoundToTickSize(lean._price, tickSize, side);
                if (roundedPrice == hf::NULL_PRICE) {
                    roundedPrice = price;
                    size = lean._size;
                    logger.logInfo(boost::str(boost::format("Adding quote for lean. Lean=%1% Size=%2% Price=%3%") % contracts[lean._instrumentIndex]._definition._name % lean._size % ToDollars(lean._price)));
                } else if (roundedPrice == price) {
                    size += lean._size;
                } else {
                    break;
                }
            }

            _externalPrice = roundedPrice;
            _externalSize = size;

            logger.logInfo(boost::str(boost::format("Quote. Contract=%1% Side=%2% Price=%3% TotalSize=%4%") % contracts[contractId]._definition._name % getSideString(side) % ToDollars(_externalPrice) % _externalSize));
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
            processMessagesFromFront();
            processMessagesFromMarketMaker();
            processMarketData();
            checkSystemState();
        }
    }

    void checkSystemState() {
        SystemState newState = _newSystemState;
        if (_systemState != newState) {
            onSysteStateChange(newState);
            _systemState = newState;
        }
    }

    void onSysteStateChange(SystemState newState) {
        switch (newState) {
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

    void handleEnabled() {
        _logger.logInfo("RV got enabled");
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled);
        logVectorPositions();
        logContractPositions();
        calculateAllMidPoints();
        calculateOverdeterminedPrices();
        calculateVectorMarketPrices();
        calculateLongTermFairsInVector();
        calculateShortTermFairs();
        calculateQuotes();
    }

    void simulateMarketData() {
        simulateMarketDataOne(0, 58.3, 7, 58.5, 2);
        simulateMarketDataOne(6, 0.31, 20, 0.33, 20);
        simulateMarketDataOne(7, 0.33, 20, 0.35, 20);
        simulateMarketDataOne(8, 0.36, 20, 0.38, 20);
        simulateMarketDataOne(9, 0.36, 20, 0.38, 20);
        simulateMarketDataOne(10, 0.37, 20, 0.39, 20);
        simulateMarketDataOne(11, 0.65, 20, 0.68, 20);
        simulateMarketDataOne(12, 0.7, 20, 0.73, 20);
        simulateMarketDataOne(13, 0.73, 20, 0.76, 20);
        simulateMarketDataOne(14, 0.73, 20, 0.76, 20);
        simulateMarketDataOne(15, 0.98, 20, 1.07, 20);
        simulateMarketDataOne(16, 0.99, 20, 1.12, 20);
        simulateMarketDataOne(17, 0.98, 20, 1.15, 20);
    }

    void simulateMarketDataOne(uint32_t id, double bidPrice, hf::qty_t bidSize, double offerPrice, hf::qty_t offerSize) {
        RvContract& contract = _contracts[id];
        Market& bid = contract._externalMarket[BID];
        bid._size = bidSize;
        bid._price = ToRhonePrice(bidPrice);
        Market& ask = contract._externalMarket[ASK];
        ask._size = offerSize;
        ask._price = ToRhonePrice(offerPrice);
    }

    void calculateAllMidPoints() {
        for (uint32_t i = 0; i < _numberOfAllContracts; ++i) {
            double weight = _weights.getValue(i);
            if (!IsZero(weight)) {
                RvContract& contract = _contracts[i];
                Market& bid = contract._externalMarket[BID];
                Market& ask = contract._externalMarket[ASK];
                if (bid._price != hf::NULL_PRICE && ask._price != hf::NULL_PRICE) {
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

    void subscribeToMarketData() {
        for (uint32_t internalId = 0; internalId < _numberOfAllContracts; ++internalId) {
            RvContract& contract = _contracts[internalId];
            subscribeToMarketData(contract);
        }
    }

    void subscribeToMarketData(RvContract& contract) {
        for (auto *const queuePtr : _marketDataQueues) {
            auto &si = queuePtr->getSI(contract._definition._sid);
            auto &vi = si.venues[unsigned(contract._definition._venue)];
            vi.level_num = 1;
            vi.level_qty = true;
            vi.trades = false;
        }
    }

    void processMessagesFromFront() {
        while (true) {
            uint64_t idx = _inFromFront->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromFront->freeNext();
            auto& message = _inFromFront->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::GridUpdate: {
                    GridUpdateMessage& gridChange = reinterpret_cast<GridUpdateMessage &> (message._data[0]);
                    bool accepted = processGridChange(*gridChange._gridUpdate);
                    ViperMessageType type = accepted ? ViperMessageType::GridUpdateAccepted : ViperMessageType::GridUpdateRejected;
                    sendGenericMessageToFront(type, gridChange._gridUpdate->_update.gridid());
                    break;
                }
                case ViperMessageType::SwitchChange: {
                    GenericMessage& value = reinterpret_cast<GenericMessage &> (message._data[0]);
                    handleSwitchChange(value);
                    break;
                }
                case ViperMessageType::Login: {
                    bool in = handleLogin();
                    sendGenericMessageToFront(ViperMessageType::LoginReply, in);
                    break;
                }
                case ViperMessageType::Logout: {
                    bool out = handleLogout();
                    sendGenericMessageToFront(ViperMessageType::LogoutReply, out);
                    break;
                }
                case ViperMessageType::ClientDown: {
                    handleClientDown();
                    break;
                }
                default:
                    break;
            }
        }
    }

    void handleClientDown() {
        _logger.logInfo("RV handling client down.");
        _systemState = _newSystemState = SystemState::Disabled;
    }

    bool handleLogin() {
        _logger.logInfo("RV logging in.");
        _systemState = _newSystemState = SystemState::LoggedIn;
        return true;
    }

    bool handleLogout() {
        _logger.logInfo("RV logging out.");
        _systemState = _newSystemState = SystemState::Disabled;
        return true;
    }

    void handleSwitchChange(GenericMessage& value) {
        MainControlAction action = static_cast<MainControlAction>(value._number_1);
        bool enabled = 1 == value._number_2;
        if (action == MainControlAction::Enabled) {
            if (enabled) {
                _systemState = _newSystemState = SystemState::Enabled;
                handleEnabled();
            } else {
                _systemState = _newSystemState = SystemState::Disabled;
                handleDisabled();
            }
        } else if (action == MainControlAction::Quoting) {
            if (enabled) {
                _canQuote = true;
                handleQuotingEnabled();
            } else {
                _canQuote = false;
                handleQuotingDisabled();
            }
        }
    }

    void processMessagesFromMarketMaker() {
        while (true) {
            uint64_t idx = _inFromMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromFront->freeNext();
            auto& message = _inFromMM->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::MarketUpdate: {
                    MarketUpdate& marketUpdate = reinterpret_cast<MarketUpdate &> (message._data[0]);
                    processMarketUpdate(marketUpdate);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    onFill(fill._contractId, fill._side, fill._qty, fill._price);
                    break;
                }
                default:
                    break;
            }
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
            switch (columnType) {
                case VectorParametersColumns::Step: {
                    double value = atof(gridCell.value().c_str());
                    _stepSize.setValue(gridCell.row(), value);
                    foundation = true;
                    break;
                }
                case VectorParametersColumns::Range: {
                    double value = atof(gridCell.value().c_str());
                    _vectorsRange.setValue(gridCell.row(), value);
                    foundation = true;
                    shortTermFairs = true;
                    break;
                }
                case VectorParametersColumns::MaxPos: {
                    double value = atof(gridCell.value().c_str());
                    _vectorsMaxPosition.setValue(gridCell.row(), value);
                    foundation = true;
                    shortTermFairs = true;
                    break;
                }
                case VectorParametersColumns::EMA_ON: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _useEMAInVector.setValue(gridCell.row(), value);
                    longTermFairs = true;
                    shortTermFairs = true;
                    break;
                }
                case VectorParametersColumns::EMA_Time: {
                    uint32_t value = atoi(gridCell.value().c_str());
                    uint64_t periodInCycles = calculatePeriodInCycles(value);
                    _emaPeriodInVector.setValue(gridCell.row(), value);
                    longTermFairs = true;
                    shortTermFairs = true;
                    break;
                }
                case VectorParametersColumns::LTF: {
                    double value = atof(gridCell.value().c_str());
                    _longTermFairsInVector.setValue(gridCell.row(), value);
                    shortTermFairs = true;
                    break;
                }
                case VectorParametersColumns::SetToMarket: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _setToMarket.setValue(gridCell.row(), value);
                    shortTermFairs = true;
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

            switch (columnType) {
                case ContractParameterRVColumns::QuoteExternal: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _quoteExternal.setValue(gridCell.row(), value);
                    break;
                }
                case ContractParameterRVColumns::Commission: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _commissionsInput.setValue(gridCell.row(), value);
                    foundation = true;
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
            bool value = 1 == atoi(gridCell.value().c_str());
            _leanMatrix.setValue(gridCell.row(), gridCell.col(), value);
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
            switch (columnType) {
                case PricingConfigsColumns::UseWeighted: {
                    bool value = 1 == atoi(gridCell.value().c_str());
                    _useWeightedMid.setValue(gridCell.row(), value);
                    RvContract& contract = _contracts[gridCell.row()];
                    updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], gridCell.row(), contract._establishedLevel);
                    break;
                }
                case PricingConfigsColumns::Weight: {
                    double value = atof(gridCell.value().c_str());
                    _weights.setValue(gridCell.row(), value);
                    calculateOverdeterminedMatrix();
                    break;
                }
                default:
                    break;
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
                addPosition(contract, size, side);
            }
        }

        calculateShortTermFairs();
        calculateQuotes();

        return true;
    }

    bool onMarketMakerParametersUpdate(GridUpdate& update) { return true; }
    bool onStackerParametersUpdate(GridUpdate& update) { return true; }
    bool onExecutionParametersUpdate(GridUpdate& update) { return true; }

    void addMarketDataQueue(md::queue::MDQueue* q) {
        _marketDataQueues.emplace_back(q);
    }

    void processMarketData() {
        for (auto *const qp : _marketDataQueues) {
            const auto idx = qp->getNext();

            if (BOOST_UNLIKELY(!idx))
                continue;

            qp->freeNext();

            auto &batch = qp->getPoolP()->getBatch(idx);
            onMDBatch(batch);

            {
                qp->poolBytesRead(batch.size);
                uint64_t usage;
                if (BOOST_UNLIKELY(qp->poolThresholdExceeded(usage))) {
                    onPoolThresholdExceeded(qp->getQueueID(), usage);
                }
                if (BOOST_UNLIKELY(qp->queueThresholdExceeded(usage))) {
                    onQueueThresholdExceeded(qp->getQueueID(), usage);
                }
            }
        }
    }

    void onPoolThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void onQueueThresholdExceeded(const std::string& queueName, uint64_t usage) {
    }

    void onMDBatch(const md::MDBatch &batch)  {
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
                default:
                    break;
            }
        }
        onEndOfBatch();
    }

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent &event) {
    }

    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent &event) {
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent &event) {
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
                const hf::qty_t othersSize = level.q;
                bid._price = level.pr;
                bid._size = othersSize;
            }

            const bool askChanged = event.sell.updatedQtyNum == 1;
            if (askChanged) {
                Market& ask = contract._externalMarket[ASK];
                const md::top_t::level_t& level = event.sell.top.l[0];
                const hf::qty_t othersSize = level.q;
                ask._price = level.pr;
                ask._size = othersSize;
            }

            {
                if (bidChanged | askChanged) {
                    logCurrentExternalMarket(contract);
                    _hadBookUpdates = true;
                    _updatedContracts.insert(internalId);
                }
            }
        } else {
            _logger.logError(boost::str(boost::format("Received book update for unrecognized SID='%1%'") % event.sid));
        }
    }

    void onEndOfBatch() {
        if (_hadBookUpdates) {
            if (_systemState == SystemState::Enabled) {
                for (uint32_t id : _updatedContracts) {
                    RvContract& contract = _contracts[id];
                    updateMidPoint(contract._externalMarket[BID], contract._externalMarket[ASK], id, contract._establishedLevel);
                }
                calculateOverdeterminedPrices();
                calculateVectorMarketPrices();
                calculateLongTermFairsInVector();
                calculateShortTermFairs();
                calculateQuotes();
            }
            _hadBookUpdates = false;
            _updatedContracts.clear();
        }
    }

    void processMarketUpdate(MarketUpdate& update) {
        RvContract& contract = _contracts[update._instrumentIndex];
        Market& market = contract._internalMarkets[update._side].getMarket(update._marketId);
        market._price = update._price;
        market._size = update._size;
        calculateQuotes();
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
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
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
            for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
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
        const hf::qty_t offerSize = std::min(offer._size, establishedLevel);
        const hf::qty_t bidSize = std::min(bid._size, establishedLevel);
        return (ToDollars(offer._price) * bidSize + ToDollars(bid._price) * offerSize) / (bidSize + offerSize);
    }

    double calculateSimpleAverage(Market& bid, Market& offer) {
        return (ToDollars(bid._price) + ToDollars(offer._price)) / 2;
    }

    void init(ViperConfig& viperConfig, ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromMM, ViperQueue* outToMM) {
        initializeModel(viperConfig);
        initializeQueues(inFromFront, outToFront, inFromMM, outToMM);
    }

    void initializeQueues(ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromMM, ViperQueue* outToMM) {
        _inFromFront = inFromFront;
        _outToFront = outToFront;
        _inFromMM = inFromMM;
        _outToMM = outToMM;
    }

    void initializeModel(ViperConfig& config) {
        initializeContracts(config);
        config.populatePricingMatrix(_pricingMatrix);
        _riskMatrix = _pricingMatrix.inverse().transpose();
        initializeVectorStructures(config._vectorConfigs);
        _fareMoveForOneUnitPosition = _vectorsRange.divide(_vectorsMaxPosition);
        initializeFromRelativeValue(config._relativeValueConfigs);
        initializeLeanMatrix(config._leanMatrix);
        initializeFromPricingConfig(config._pricingConfigs);
        initializeStructures();
        calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes();
        _overdeterminedPrices.initialize(1, _numberOfOutrights);
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
            _weights.setValue(index, pc._weight);
            _useWeightedMid.setValue(index, pc._establishedLevel == 1);
        }
    }

    void calculateOverdeterminedMatrix() {
        Matrix A;
        A.initialize(_numberOfAllContracts, _numberOfOutrights);
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            RvContract& contract = _contracts[row];
            for (uint32_t col = 0; col < _numberOfOutrights; ++col) {
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
        _stepSize.init(size);
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
            _stepSize.setValue(index, vc._step);
            _longTermFairsInVector.setValue(index, vc._longTermFair);
            _vectorNames.push_back(vc._vectorName);
        }
    }

    void initializeStructures() {
        _hedgeRatios.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _quoteSizes.initialize(_numberOfAllContracts, _numberOfAllContracts);
        _maxQuoteSizes.init(_numberOfAllContracts);
        _numberOfVectors = _riskMatrix._numberOfColumns;
        _shortTermFairsInVector.init(_numberOfVectors);
        _possibleHedgeRatios.initialize(_numberOfVectors, 3);
        _hedgeVectorRiskExposures.initialize(_numberOfVectors, _numberOfVectors);
        _hedgeVectorRiskExposuresSums.init(_numberOfVectors);
        _normalizationMatrix.initialize(_numberOfOutrights, 3);
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
            if (contract != nullptr) {
                _commissionsInput.setValue(contract->_internalId, rvConfig._commission);
                _contractWidths.setValue(contract->_internalId, ToRhonePrice(rvConfig._width));
                if (rvConfig._position != 0) {
                    uint32_t side = rvConfig._position > 0 ? BID : ASK;
                    addPosition(*contract, std::abs(rvConfig._position), side);
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
        _numberOfOutrights = config._numberOfOutrights;
        _numberOfAllContracts = config._allContracts.size();
    }

    void calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes() {
        for (uint32_t rowIndex = 0; rowIndex < _hedgeRatios._numberOfRows; ++rowIndex) {
            for (uint32_t columnIndex = 0; columnIndex < _hedgeRatios._numberOfColumns; ++columnIndex) {
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
        calculateMaxQuoteSizes();
    }

    void calculateMaxQuoteSizes() {
        for (uint32_t row = 0; row < _numberOfAllContracts; ++row) {
            double maxQuoteSize = std::numeric_limits<double>::min();
            for (uint32_t col = 0; col < _numberOfAllContracts; ++col) {
                const double quoteSize = _quoteSizes.getValue(row, col);
                maxQuoteSize = std::max(maxQuoteSize, quoteSize);
            }
            hf::qty_t finalSize = static_cast<hf::qty_t>(maxQuoteSize);
            _maxQuoteSizes.setValue(row, finalSize);
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
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
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
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
            double value = _normalizationMatrix.getValue(outrightIndex, NormalizationMatrixColumns::ContractOneRatio) - _normalizationMatrix.getValue(outrightIndex, NormalizationMatrixColumns::ContractTwoRatio);
            _normalizationMatrix.setValue(outrightIndex, NormalizationMatrixColumns::Total, value);
        }
    }

    double findNormalizer() {
        double min = std::numeric_limits<double>::max();
        for (uint32_t outrightIndex = 0; outrightIndex < _numberOfOutrights; ++outrightIndex) {
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
        double minQuoteSize = std::numeric_limits<double>::max();
        for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
            const double riskExposure = _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractOneRisk)
                                        - _possibleHedgeRatios.getValue(vectorIndex, PossibleHedgeRatiosColumns::ContractTwoRisk) * hedgeRatio;
            if (!IsZero(riskExposure)) {
                const double fairMove = _fareMoveForOneUnitPosition.getValue(vectorIndex);
                const double stepSize = _stepSize.getValue(vectorIndex);
                const double oneLotMove = std::abs(riskExposure) * fairMove;
                const double quoteSize = stepSize / oneLotMove;
                if (quoteSize < minQuoteSize) {
                    minQuoteSize = quoteSize;
                }
            }
        }
        return minQuoteSize;
    }

    void onFill(uint32_t contractIndex, uint32_t side, hf::qty_t size, hf::price_t price) {
        RvContract& contract = _contracts[contractIndex];
        _logger.logInfo(boost::str(boost::format("RV got filled. Contract=%1% Side=%2% Price=%3% Qty=%4%") % contract._definition._name % getSideString(side) % ToDollars(price) % size));
        sendHedges(contract, size, side);
        addPosition(contract, size, side);
        calculateShortTermFairs();
        calculateQuotes();
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

    void sendHedges(RvContract& contract, hf::qty_t size, uint32_t side) {
        _logger.logInfo("RV sending hedges.");
        Quote& quote = contract._quotes[side];
        double needToHedge = size;
        hf::price_t roundedPrice = hf::NULL_PRICE;
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
                hf::qty_t orderSize;
                Market& leanMarket = leanContract._externalMarket[side];
                const bool done = getOrderSize(hedgeRatio, orderSize, leanMarket._size, needToHedge);
                const uint32_t otherSide = GetOtherSide(side);

                hf::price_t hedgePrice;
                if (lean._isExternal) {
                    hedgePrice = leanContract._externalMarket[side]._price;
                } else {
                    hedgePrice = leanContract._internalMarkets[side].getMarket(lean._marketIndex)._price;
                }

                _logger.logInfo(boost::str(boost::format("Sending hedge. Contract=%1% Side=%2% Price=%3% Size=%4% External=%5%")
                                           % leanContract._definition._name % getSideString(otherSide) % ToDollars(hedgePrice) % orderSize % (int) lean._isExternal));

                sendHedgeOrder(leanContract._internalId, otherSide, hedgePrice, orderSize, lean._isExternal);
                addPosition(leanContract, orderSize, otherSide);
                if (done) {
                    return;
                }
            } else {
                return;
            }
        }
    }

    void sendHedgeOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, bool external) {
        auto &outMessage = _outToMM->pool()->getNextMessage();
        outMessage._type = ViperMessageType::HedgeOrder;

        HedgeOrderMessage& message = reinterpret_cast<HedgeOrderMessage &> (outMessage._data[0]);
        message._instrumentIndex = internalId;
        message._side = side;
        message._price = price;
        message._size = size;
        message._isExternal = external;

        const auto outIdx = _outToMM->pool()->append(sizeof(ViperMessageType) + sizeof(HedgeOrderMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    bool getOrderSize(double hedgeRatio, hf::qty_t& orderSize, hf::qty_t availableLeanSize, double& needToHedge) {
        bool done = false;
        double leanSizeInQuoteTerms = availableLeanSize * hedgeRatio;
        if (leanSizeInQuoteTerms < needToHedge) {
            orderSize = availableLeanSize;
            needToHedge -= leanSizeInQuoteTerms;
        } else {
            orderSize = static_cast<hf::qty_t>(needToHedge * hedgeRatio);
            done = true;
        }
        return done;
    }

    void addPosition(RvContract& contract, hf::qty_t size, uint32_t side) {
        const hf::qty_t positionChange = side == BID ? size : -size;
        ContractDefinition& contractDefinition = contract._definition;
        if (contractDefinition._numberOfLegs > 1) {
            for (uint32_t index = 0; index < contractDefinition._numberOfLegs; ++index) {
                ContractDefinition::Leg& leg = contractDefinition._legs[index];
                const hf::qty_t legPositionChange = positionChange * leg._size;
                _contractPositions.add(leg._index, legPositionChange);
                for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                    const double changeInVector = _riskMatrix.getValue(vectorIndex, leg._index) * legPositionChange;
                    _positionsInVector.add(vectorIndex, changeInVector);
                }
            }
        } else {
            _contractPositions.add(contract._definition._internalId, positionChange);
            for (uint32_t vectorIndex = 0; vectorIndex < _riskMatrix._numberOfColumns; ++vectorIndex) {
                const double changeInVector = _riskMatrix.getValue(vectorIndex, contract._definition._internalId) * positionChange;
                _positionsInVector.add(vectorIndex, changeInVector);
            }
        }
        logVectorPositions();
        logContractPositions();
        sendPositionUpdateToGUI(contract._definition._internalId);
    }

    void logContractPositions() {
        std::stringstream message;
        message << "Contract positions: ";
        for (uint32_t spreadIndex = _numberOfOutrights; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
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
        for (uint32_t contractIndex = 0; contractIndex < _numberOfOutrights; ++contractIndex) {
            double shortTermFair = 0;
            for (uint32_t vectorIndex = 0; vectorIndex < _numberOfVectors; ++vectorIndex) {
                const double value = _shortTermFairsInVector.getValue(vectorIndex) * _riskMatrix.getValue(vectorIndex, contractIndex);
                shortTermFair += value;
            }
            const hf::price_t rhonePrice = ToRhonePrice(shortTermFair);
            _shortTermFairsForContracts.setValue(contractIndex, rhonePrice);
        }
        for (uint32_t spreadIndex = _numberOfOutrights; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
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
        for (uint32_t spreadIndex = _numberOfOutrights; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
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
                        const hf::price_t width = _contractWidths.getValue(instrumentIndex) + _contractWidths.getValue(leanIndex);
                        const hf::price_t leanFair = _shortTermFairsForContracts.getValue(leanIndex);
                        const hf::qty_t maxQuoteSize = static_cast<hf::qty_t>(_quoteSizes.getValue(contract._definition._internalId, leanIndex));

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
                bidQuote.createQuote(contract._definition._tickSize, BID, quoteExternal, _logger, _contracts, instrumentIndex);
                sendQuoteOrder(contract, BID);

                Quote& askQuote = contract._quotes[ASK];
                askQuote.createQuote(contract._definition._tickSize, ASK, quoteExternal, _logger, _contracts, instrumentIndex);
                sendQuoteOrder(contract, ASK);
            }
        }
        sendQuotesToGUI();
    }

    void sendQuoteOrder(RvContract& contract, uint32_t side) {
        Quote& quote = contract._quotes[side];

        auto &outMessage = _outToMM->pool()->getNextMessage();
        outMessage._type = ViperMessageType::QuoteOrder;

        QuoteOrderMessage& message = reinterpret_cast<QuoteOrderMessage &> (outMessage._data[0]);
        message._instrumentIndex = contract._definition._internalId;
        message._side = side;

        message._externalPrice = quote._externalPrice;
        message._externalSize = quote._externalSize;

        message._internalMarketPrice = quote._bestPrice;
        message._internalMarketSize = quote._bestSize;

        const auto outIdx = _outToMM->pool()->append(sizeof(ViperMessageType) + sizeof(QuoteOrderMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void calculateBidQuote(Market& market, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, hf::qty_t maxQuoteSize, bool isExternal) {
        if (market._price != hf::NULL_PRICE) {
            const hf::price_t bidHedgingCost = leanFair - market._price;
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t commission = _commissions.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t bidPrice = fair - width - static_cast<hf::price_t>(bidHedgingCost * hedgeRatio) - commission;
            const hf::qty_t size = calculateSize(hedgeRatio, market._size, maxQuoteSize);
            contract._quotes[BID]._leans.add(leanIndex, market._id, bidPrice, size, isExternal);
        }
    }

    void calculateAskQuote(Market& market, RvContract& contract, uint32_t leanIndex, hf::price_t leanFair, hf::price_t fair, hf::price_t width, hf::qty_t maxQuoteSize,  bool isExternal) {
        if (market._price != hf::NULL_PRICE) {
            const hf::price_t askHedgingCost = market._price - leanFair;
            const double hedgeRatio = _hedgeRatios.getValue(contract._definition._internalId, leanIndex);
            const hf::price_t askPrice = fair + width + static_cast<hf::price_t>(askHedgingCost * hedgeRatio) + _commissions.getValue(contract._definition._internalId, leanIndex);
            const hf::qty_t size = calculateSize(hedgeRatio, market._size, maxQuoteSize);
            contract._quotes[ASK]._leans.add(leanIndex, market._id, askPrice, size, isExternal);
        }
    }

    hf::qty_t calculateSize(double hedgeRatio, hf::qty_t marketSize, hf::qty_t maxQuoteSize) {
        hf::qty_t size = maxQuoteSize;
        if (!IsZero(hedgeRatio)) {
            const hf::qty_t possibleSize = static_cast<hf::qty_t>(marketSize / hedgeRatio);
            if (possibleSize > maxQuoteSize) {
                size = maxQuoteSize;
            }
        }
        return size;
    }

    void logCurrentExternalMarkets() {
        for (uint32_t spreadIndex = _numberOfOutrights; spreadIndex < _numberOfAllContracts; ++spreadIndex) {
            RvContract& contract = _contracts[spreadIndex];
            logCurrentExternalMarket(contract);
        }
    }

    void logCurrentExternalMarket(RvContract& contract) {
        std::string bidString;
        Market& bid = contract._externalMarket[BID];
        if (bid._price == hf::NULL_PRICE) {
            bidString = "missing";
        } else {
            bidString.append(std::to_string(bid._size)).append("@").append(std::to_string(ToDollars(bid._price)));
        }

        std::string offerString;
        Market& offer = contract._externalMarket[ASK];
        if (offer._price == hf::NULL_PRICE) {
            offerString = "missing";
        } else {
            offerString.append(std::to_string(offer._size)).append("@").append(std::to_string(ToDollars(offer._price)));
        }

        _logger.logInfo(boost::str(boost::format("Market: Contract=%1% Bid: %2% Offer: %3%") % contract._definition._name % bidString % offerString));
    }

    Matrix _pricingMatrix; // x,y - vectors - square
    Matrix _riskMatrix; // x,y - vectors - square
    uint32_t _numberOfVectors;

    Vector<double> _vectorsRange;
    Vector<double> _vectorsMaxPosition;
    Vector<double> _stepSize;
    Vector<double> _fareMoveForOneUnitPosition;
    Vector<bool> _useWeightedMid;
    Vector<double> _weights;
    Vector<hf::price_t> _contractWidths;

    SimpleMatrix<double> _hedgeRatios; // x,y - outrights, spreads - square
    Matrix _quoteSizes; // x,y - outrights, spreads - square
    Vector<hf::qty_t> _maxQuoteSizes;

    std::vector<RvContract> _contracts; // outrights, spreads
    uint32_t _numberOfOutrights;
    uint32_t _numberOfAllContracts;

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

    ViperQueue* _inFromFront;
    ViperQueue* _inFromMM;
    ViperQueue* _outToFront;
    ViperQueue* _outToMM;

    std::vector<md::queue::MDQueue *> _marketDataQueues;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;
    bool _hadBookUpdates = false;
    std::set<uint32_t> _updatedContracts;

    UpdateSelector<RelativeValue> _updateSelector;

    uint32_t _strategyId;

    SystemState _systemState = SystemState::LoggedOut;
    SystemState _newSystemState = SystemState::LoggedOut;
    bool _canQuote = false;

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
