#ifndef VIPER_FRONT_SIDE_H
#define VIPER_FRONT_SIDE_H

#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include <hf/ResourceManager.h>
#include "ClientAcceptor.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include "Executor.h"
#include <queue>
#include <md/sym/SymbolsResource.h>
#include "GUIThings.h"
#include "CommonHelper.h"
#include "Common.h"
#include "NegativeScalpChecker.h"
#include "PnlLimitsChecker.h"

namespace Python {

class FrontSide : public virtual hf::Service {
public:


    FrontSide() {
    }

    void onCreate(const libconfig::Setting& config) override {
        const auto host = hf::ConfigManager::getString(config, "host");
        const auto port = hf::ConfigManager::getLong(config, "port");
        _frequency = clockUtil::GetCpuFrequency();
        _clientAcceptor = new ClientsAcceptor(host, port, this, _frequency);
        _clientAcceptor->createMonitor();
        createGridUpdates();
    }

    void createGridUpdates() {
        std::vector<WindowIds> updatableGrids = {WindowIds::VectorParameters, WindowIds::ContractParametersRv, WindowIds::PricingConfig, WindowIds::ManualFillsEntry,
                                                 WindowIds::MarketMakerParameters, WindowIds::LeanMatrix, WindowIds::StackerParameters, WindowIds::ContractParametersRv,
                                                 WindowIds::PNL};
        for (auto id : updatableGrids) {
            _gridUpdates.insert({static_cast<uint32_t>(id), GridUpdate()});
        }
    }

    void onPostCreate(const libconfig::Setting& config) override {
        const auto symbolsName = hf::ConfigManager::getString(config, "symbols");
        _symbolsResource = dynamic_cast<md::sym::SymbolsResource *> (hf::ResourceManager::getResourceP(symbolsName));
        if (!_symbolsResource) {
           throw std::runtime_error("could not get symbols resource");
        }

        const auto name = hf::ConfigManager::getString(config, "config_file");
        const bool isConfigGood = _config.initialize(name, *_symbolsResource);
        if (!isConfigGood) {
            _logger.logErrorAndThrow("Config is no good.");
        }

        populateSymbolToInterId();

        _guiThings.init(&_config, &_symbolToInterId);

        const std::string relativeValueName = config["RelativeValue"];
        _relativeValue = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(relativeValueName));

        const std::string marketMakersName = config["MarketMakers"];
        _marketMakers = dynamic_cast<ExecutorT*> (hf::ResourceManager::getResourceP(marketMakersName));

        auto queueFromMMToFront = createQueue();
        auto queueFromFrontToMM = createQueue();
        auto queueFromRVToFront = createQueue();
        auto queueFromFrontToRV = createQueue();
        auto queueFromMMToRV = createQueue();
        auto queueFromRVToMM = createQueue();

        _inFromMM = queueFromMMToFront;
        _inFromRV = queueFromRVToFront;
        _outToMM = queueFromFrontToMM;
        _outToRV = queueFromFrontToRV;

        _relativeValue->init(_config, queueFromFrontToRV, queueFromRVToFront, queueFromMMToRV, queueFromRVToMM, _frequency, _systemState);
        _marketMakers->init(_config, queueFromFrontToMM, queueFromMMToFront, queueFromRVToMM, queueFromMMToRV, _frequency, _systemState);

        uint32_t clips = boost::lexical_cast<uint32_t>(_config.getGlobal(NEGATIVE_SCALP_CLIPS));
        hf::price_t dollars = ToRhonePrice(boost::lexical_cast<uint32_t>(_config.getGlobal(NEGATIVE_SCALP_DOLLARS)));
        uint32_t period = boost::lexical_cast<uint32_t>(_config.getGlobal(NEGATIVE_SCALP_PERIOD));
        uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
        _negativeScalpCheckers.initialize(_config._allContracts, periodInCycles, dollars, clips);

        _pnlCalculator.init(_config._allContracts, _negativeScalpCheckers);
        _pnlLimitsChecker.initialize(&_pnlCalculator, _config);

        _refreshInterval = GetCyclesFromSeconds(1, _frequency);
    }

     void populateSymbolToInterId() {
        for (uint32_t i = 0; i < _config._allContracts.size(); ++i) {
            auto& contract = _config._allContracts[i];
            _symbolToInterId.insert({contract._name, i});
        }
    }

    ViperQueue* createQueue() {
        auto pool = new ViperPool();
        pool->create();
        auto queue = new ViperQueue();
        queue->create();
        queue->set_pool(pool);
        return queue;
    }

    void onRun() override {
        _inFromMM->resetR();
        _inFromRV->resetR();
        _outToMM->resetW();
        _outToRV->resetW();

        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            _clientAcceptor->runIteration();
            processOutgoingMessagesFromRelativeValue();
            processOutgoingMessagesFromMarketMaker();
            refreshThings();
        }
    }

    void refreshThings() {
        const uint64_t now = clockUtil::GetCycles();
        if (now > _nextRefreshTime) {
            if (_marketMakers->isLoggedIn()) {
                for (uint32_t contractIndex = _config._numberOfOutrights - 1; contractIndex < _config._allContracts.size(); ++contractIndex) {
                    ConfigContract& contract = _config._allContracts[contractIndex];
                    hf::price_t price;
                    if (contract._legs.size() > 1) {
                        double sum = 0;
                        for (auto& leg : contract._legs) {
                            sum += _relativeValue->_overdeterminedPrices.getValue(0, leg._index) * leg._ratio;
                        }
                        price = ToRhonePrice(sum);
                    } else {
                        price = ToRhonePrice(_relativeValue->_overdeterminedPrices.getValue(0, contractIndex));
                    }

                    if (_pnlCalculator.hasDifferentMarkPrice(contractIndex, price)) {
                        _pnlCalculator.recalculateOpenPnl(contractIndex, price);
                        sendPnlDataToGui(contractIndex, StrategyId::RelativeValue);
                        sendPnlDataToGui(contractIndex, StrategyId::MarketMaker);
                        if (_pnlLimitsChecker.checkLimits(StrategyId::RelativeValue)) {
                            onSystemStateChange(SystemState::Disabled, "PNL limits breached");
                        }
                        if (_pnlLimitsChecker.checkLimits(StrategyId::MarketMaker)) {
                            onSystemStateChange(SystemState::Disabled, "PNL limits breached");
                        }
                    }

                    sendMessageCountsToGui();
                }
            }
            _nextRefreshTime = now + _refreshInterval;
        }
    }

    void processOutgoingMessagesFromRelativeValue() {
        while (true) {
            uint64_t idx = _inFromRV->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromRV->freeNext();

            auto& message = _inFromRV->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::SwitchChangeReply: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    _clientAcceptor->sendSwitchChangeReply(toUInt(StrategyId::RelativeValue), toUInt(reply._number_1), std::to_string(reply._number_2), toUInt(WindowIds::MainControl), true);
                    break;
                }
                case ViperMessageType::PositionUpdate: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    const uint32_t contractIndex = reply._number_1;
                    sendPositionsToGUI(contractIndex);
                    break;
                }
                case ViperMessageType::STFUpdate: {
                    sendStfToGui();
                    break;
                }
                case ViperMessageType::QuotesUpdate: {
                    sendQuotesToGUI();
                    break;
                }
                case ViperMessageType::OverdeterminedPricesUpdate: {
                    sendOverdeterminedPricesUpdate();
                    break;
                }
                case ViperMessageType::LTFUpdate: {
                    sendLtfToGui();
                    break;
                }
                case ViperMessageType::GridUpdateAccepted: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    onGridUpdateAccepted(reply._number_1);
                    break;
                }
                case ViperMessageType::GridUpdateRejected: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    onGridUpdateRejected(reply._number_1);
                    break;
                }
                case ViperMessageType::ErrorMessage: {
                    ErrorMessage& m = reinterpret_cast<ErrorMessage&> (message._data[0]);
                    sendErrorMessage(m);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void sendPlaySound(SoundId id) {
        _clientAcceptor->sendPlaySound(static_cast<uint32_t>(id));
    }

    void sendGuiInitialized() {
        sendGenericMessageToMM(ViperMessageType::GuiInitialized);
        sendGenericMessageToRV(ViperMessageType::GuiInitialized);
    }

    void sendFillToGUI(FillMessage& fill) {
        proto::GridUpdate message;
        _guiThings.populateFillMessage(fill, message);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendLtfToGui() {
        proto::GridUpdate message;
        _guiThings.populateLTFmessage(message, _relativeValue);
        _clientAcceptor->sendGridUpdate(message);
        proto::DataSeriesPointSet pointsSet;
        _guiThings.populateLTFChartDataPoints(pointsSet, _relativeValue);
        _clientAcceptor->sendChartDataPoints(pointsSet);
    }

    void sendOverdeterminedPricesUpdate() {
        proto::GridUpdate message;
        _guiThings.populateVectorPrices(message, _relativeValue);
        _clientAcceptor->sendGridUpdate(message);
        proto::DataSeriesPointSet pointsSet;
        _guiThings.populateVectorPricesForChart(pointsSet, _relativeValue);
        _clientAcceptor->sendChartDataPoints(pointsSet);
        proto::GridUpdate contractMessage;
        _guiThings.populateContractPrices(contractMessage, _relativeValue);
        _clientAcceptor->sendGridUpdate(contractMessage);
    }

    void sendQuotesToGUI() {
        bool checkForCross = _systemState._relativeValue == SystemState::CheckQuotes;
        proto::GridUpdate message;
        _guiThings.populateContractQuotes(message, _relativeValue, checkForCross);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendStfToGui() {
        proto::GridUpdate vectorMessage;
        _guiThings.populateVectorStf(vectorMessage, _relativeValue);
        _clientAcceptor->sendGridUpdate(vectorMessage);
        proto::DataSeriesPointSet pointSet;
        _guiThings.populateVectorStfForChart(pointSet, _relativeValue);
        _clientAcceptor->sendChartDataPoints(pointSet);
        proto::GridUpdate message;
        _guiThings.populateContractStf(message, _relativeValue);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendPositionsToGUI(uint32_t contractIndex) {
        proto::GridUpdate message;
        _guiThings.populateContractPosition(contractIndex, message, _relativeValue);
        _clientAcceptor->sendGridUpdate(message);
        proto::GridUpdate vectorMessage;
        _guiThings.populateVectorPosition(vectorMessage, _relativeValue);
        _clientAcceptor->sendGridUpdate(vectorMessage);
    }

    void processOutgoingMessagesFromMarketMaker() {
        while (true) {
            uint64_t idx = _inFromMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromMM->freeNext();

            auto& message = _inFromMM->pool()->getMessage(idx);

            switch (message._type) {
                case ViperMessageType::GridUpdateAccepted: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    onGridUpdateAccepted(reply._number_1);
                    break;
                }
                case ViperMessageType::GridUpdateRejected: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    onGridUpdateRejected(reply._number_1);
                    break;
                }
                case ViperMessageType::SwitchChangeReply: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    _clientAcceptor->sendSwitchChangeReply(toUInt(StrategyId::MarketMaker), toUInt(reply._number_1), std::to_string(reply._number_2), toUInt(WindowIds::MainControl), true);
                    break;
                }
                case ViperMessageType::MarketMakerEnabled: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    uint32_t id = reply._number_1;
                    bool enabled = reply._number_2 == 1;
                    sendMarketMakerEnabled(id, enabled);
                    break;
                }
                case ViperMessageType::ErrorMessage: {
                    ErrorMessage& m = reinterpret_cast<ErrorMessage&> (message._data[0]);
                    sendErrorMessage(m);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    sendFillToGUI(fill);
                    sendPlaySound(SoundId::Fill);
                    doPnl(fill);
                    _logger.logInfo(boost::str(boost::format("Fill. Strategy=%1% Type=%2% Contract=%3% Side=%4% Price=%5% Size=%6%")
                                        % GetStrategyString(fill._strategy) % GetFillTypeString(fill._type) % _config._allContracts[fill._contractId]._name
                                        % GetSideString(fill._side) % ToDollars(fill._price) % fill._qty));
                    break;
                }
                case ViperMessageType::GatewayStatus: {
                    GatewayStatusMessage& status = reinterpret_cast<GatewayStatusMessage &> (message._data[0]);

                    std::string m = status._message;
                    ErrorMessage em;
                    memcpy(em._message, m.c_str(), m.size());
                    em._size = m.size();
                    em._type = status._active ? ErrorType::Normal : ErrorType::Critical;
                    sendErrorMessage(em);

                    proto::GridUpdate update;
                    _guiThings.populateGatewayStatusUpdate(update, status._active);
                    _clientAcceptor->sendGridUpdate(update);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void doPnl(FillMessage& fill) {
        _pnlCalculator.onFill(fill);
        if (!_pnlLimitsChecker.checkLimits(fill._strategy)) {
            onSystemStateChange(SystemState::Disabled, "PNL limits breached.");
        }
        sendPnlDataToGui(fill._contractId, fill._strategy);
        if (_negativeScalpCheckers.getChecker(fill._contractId).isOverLimit()) {
            onSystemStateChange(SystemState::Disabled, "PNL limits breached.");
        }
    }

    void sendPnlDataToGui(uint32_t contractId, StrategyId strategyId) {
        sendPnlToGui(_pnlCalculator.getAggregatedPnl(), strategyId);
        sendPnlDetailToGui(_pnlCalculator.getPnlDetail(contractId, strategyId), strategyId, contractId);
    }

    void sendPnlToGui(AggregatedPnl& pnl, StrategyId id) {
        proto::GridUpdate pnlMessage;
        _guiThings.populatePnlUpdate(pnlMessage, pnl, id);
        _clientAcceptor->sendGridUpdate(pnlMessage);
    }

    void sendPnlDetailToGui(Pnl& pnl, StrategyId id, uint32_t contractIndex) {
        proto::GridUpdate pnlDetailMessage;
        _guiThings.populatePnlDetailUpdate(pnlDetailMessage, pnl, id, contractIndex);
        _clientAcceptor->sendGridUpdate(pnlDetailMessage);
    }

    void sendMarketMakerEnabled(uint32_t id, bool enabled) {
        proto::GridUpdate message;
        _guiThings.populateMarketMakerEnabled(message, id, enabled);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendMessageCountsToGui() {
        proto::GridUpdate message;
        _guiThings.populateMessageCountsUpdate(message, _marketMakers);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendErrorMessage(ErrorMessage& m) {
        proto::GridUpdate message;
        _guiThings.populateErrorMessage(message, m);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendErrorMessage(const std::string& message, ErrorType type) {
        ErrorMessage em;
        strcpy(em._message, message.c_str());
        em._size = message.size();
        em._type = type;
        sendErrorMessage(em);
    }

    void onGridUpdate(proto::GridUpdate& update) {
        auto it = _gridUpdates.find(update.gridid());
        if (it != _gridUpdates.end()) {
            GridUpdate& gridUpdate = it->second;
            if (gridUpdate._ready) {
                gridUpdate._update = update;
                const WindowIds windowId = static_cast<WindowIds>(update.gridid());
                if (windowId == WindowIds::PNL) {
                    const bool limitsBreach = _pnlLimitsChecker.handlePnlLimitChange(gridUpdate);
                    if (limitsBreach) {
                        onSystemStateChange(SystemState::Disabled, "PNL limits breached.");
                    }
                } else {
                    const bool isUpdateForMM = isUpdateForMarketMaker(windowId);
                    if (isUpdateForMM) {
                        sendGridUpdateToMM(gridUpdate);
                    } else {
                        sendGridUpdateToRv(gridUpdate);
                    }
                }
            } else {
                _logger.logError("received grid update but another already in flight");
            }
        } else {
            _logger.logError("received grid update for unknown grid " + update.gridid());
        }
    }

    bool isUpdateForMarketMaker(WindowIds id) {
        return id == WindowIds::MarketMakerParameters || id == WindowIds::StackerParameters || id == WindowIds::GlobalConfigs;
    }

    void sendGridUpdateToRv(GridUpdate& update) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = ViperMessageType::GridUpdate;

        GridUpdateMessage& message = reinterpret_cast<GridUpdateMessage &> (outMessage._data[0]);
        message._gridUpdate = &update;

        const auto outIdx = _outToRV->pool()->append(sizeof(GridUpdateMessage) + sizeof(ViperMessageType));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGridUpdateToMM(GridUpdate& update) {
        auto &outMessage = _outToMM->pool()->getNextMessage();
        outMessage._type = ViperMessageType::GridUpdate;

        GridUpdateMessage& reply = reinterpret_cast<GridUpdateMessage &> (outMessage._data[0]);
        reply._gridUpdate = &update;

        const auto outIdx = _outToMM->pool()->append(sizeof(GridUpdateMessage) + sizeof(ViperMessageType));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void sendGenericMessageToRV(ViperMessageType type, int32_t action = 0, int32_t value = 0) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = type;

        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = action;
        message._number_2 = value;

        const auto outIdx = _outToRV->pool()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGenericMessageToMM(ViperMessageType type, int32_t action = 0, int32_t value = 0) {
        auto &outMessage = _outToMM->pool()->getNextMessage();
        outMessage._type = type;

        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = action;
        message._number_2 = value;

        const auto outIdx = _outToMM->pool()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void onGridUpdateAccepted(uint32_t gridId) {
        auto it = _gridUpdates.find(gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            _config.applyGridUpdate(gu);
            gu.clear();
            _clientAcceptor->sendGridChangeReply(gridId, true);
        } else {
            _logger.logError("Accepted grid update which cannot be found");
        }
    }

    void onGridUpdateRejected(uint32_t gridId) {
        auto it = _gridUpdates.find(gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            gu.clear();
            _clientAcceptor->sendGridChangeReply(gridId, false);
        } else {
            _logger.logError("Rejected grid update which cannot be found");
        }
    }

    void onOtherRequest(ViperMessageType mt)  {
        switch (mt) {
            case ViperMessageType::Login: {
                onSystemStateChange(SystemState::LoggedIn, "GUI Requested");
                break;
            }
            case ViperMessageType::Logout: {
                onSystemStateChange(SystemState::LoggedOut, "GUI Requested");
                break;
            }
            case ViperMessageType::ClientDown: {
                _logger.logError("Client down - doing hard shutdown");
                onSystemStateChange(SystemState::HardShutdown, "GUI Requested");
                break;
            }
            default:
                break;
        }
    }

    void onGuiInitializationAccepted() {
        sendGuiInitialized();
    }

    void onGuiInitializationRejected() {
        _logger.logErrorAndThrow("GUI initialization failed;");
    }

    void onSystemStateChange(SystemState newState, const std::string& reason) {
        if (newState != SystemState::CheckQuotes) {
            onStrategySystemStateChange(newState, _systemState._marketMaker, reason, "MarketMaker");
        }
        onStrategySystemStateChange(newState, _systemState._relativeValue, reason, "RelativeValue");
    }

    void onStrategySystemStateChange(SystemState newState, SystemState& currentState, const std::string& reason, const std::string& strategy) {
        if (newState != currentState) {
            std::string message = boost::str(boost::format("Request State Change: Strategy=%1% State=%2% Reason=%3%") % strategy % GetSystemStateString(newState) % reason);
            _logger.logError(message);
            if (newState == SystemState::Enabled) {
                sendPlaySound(SoundId::Enabled);
            }
            sendErrorMessage(message, ErrorType::Critical);
            currentState = newState;
            if (currentState == SystemState::HardShutdown) {
                hf::Service::exitAll();
            }
        }
    }

    void onSwitchChange(proto::SwitchChange& change) {
        auto& cell = change.cell();

        MainControlAction action = static_cast<MainControlAction>(cell.col());

        if (action != MainControlAction::Enabled && action != MainControlAction::Quoting) {
            return;
        }

        SystemState state = ToSystemStateFromProto((proto::SystemState)atoi(cell.value().c_str()));
        StrategyId strategyId = static_cast<StrategyId>(cell.row());

        switch (strategyId) {
            case StrategyId::MarketMaker: {
                if (MainControlAction::Enabled == action) {
                    onStrategySystemStateChange(state, _systemState._marketMaker, "GUI requested", "MarketMaker");
                }
                break;
            }
            case StrategyId::RelativeValue: {
                if (MainControlAction::Enabled == action) {
                    onStrategySystemStateChange(state, _systemState._relativeValue, "GUI requested", "RelativeValue");
                }
                break;
            }
            default: {
                break;
            }
        }
    }

private:
    ClientsAcceptor* _clientAcceptor;

    ViperQueue* _inFromRV;
    ViperQueue* _inFromMM;
    ViperQueue* _outToRV;
    ViperQueue* _outToMM;

    ViperConfig _config;
    std::unordered_map<uint32_t, GridUpdate> _gridUpdates;

    md::sym::SymbolsResource* _symbolsResource;

    std::unordered_map<std::string, uint32_t> _symbolToInterId;

    RelativeValue* _relativeValue;
    ExecutorT* _marketMakers;

    GUIThings _guiThings;

    SharedSystemState _systemState;

    PnlCalculator _pnlCalculator;
    NegativeScalpCheckers _negativeScalpCheckers;
    PnlLimitsChecker _pnlLimitsChecker;

    uint64_t _nextRefreshTime = 0;
    uint64_t _refreshInterval;

    double _frequency;


    Logger _logger;

};

}

#endif //VIPER_FRONT_SIDE_H

