#ifndef VIPER_FRONT_SIDE_H
#define VIPER_FRONT_SIDE_H

#include <hf/Service.h>
#include <hf/ConfigManager.h>
#include <hf/ResourceManager.h>
#include "ClientAcceptor.h"
#include "ViperConfig.h"
#include "RelativeValue.h"
#include "MarketMakers.h"
#include <queue>
#include <md/sym/SymbolsResource.h>
#include "GUIThings.h"
#include "CommonHelper.h"
#include "PnlCalculator.h"
#include "Common.h"

namespace Python {

class FrontSide : public virtual hf::Service {
public:

    enum StrategyLimitIndex {StrategyLimitIndexMM, StrategyLimitIndexRV};

    FrontSide() {
    }

    void onCreate(const libconfig::Setting& config) override {
        const auto host = hf::ConfigManager::getString(config, "host");
        const auto port = hf::ConfigManager::getLong(config, "port");
        _clientAcceptor = new ClientsAcceptor(host, port, this);
        _clientAcceptor->createMonitor();
        createGridUpdates();
        _strategyPnlLimits[StrategyLimitIndexRV] = 0;
        _strategyPnlLimits[StrategyLimitIndexMM] = 0;
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
        _marketMakers = dynamic_cast<MarketMakersT*> (hf::ResourceManager::getResourceP(marketMakersName));

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

        _relativeValue->init(_config, queueFromFrontToRV, queueFromRVToFront, queueFromMMToRV, queueFromRVToMM);
        _marketMakers->init(_config, queueFromFrontToMM, queueFromMMToFront, queueFromRVToMM, queueFromMMToRV);

        _pnlCalculator.init(_config._allContracts.size());

        double frequency = clockUtil::GetCpuFrequency();

        _refreshInterval = static_cast<uint64_t>(frequency * 2 * 1000000000.0);
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
        return;
        const uint64_t now = clockUtil::GetCycles();
        if (now > _nextRefreshTime) {
            if (_systemState > SystemState::LoggedOut) {
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
                    _pnlCalculator.recalculateOpenPnl(contractIndex, ToRhonePrice(price));
                    sendPnlToGui(_pnlCalculator.getAggregatedPnl(), StrategyId::RelativeValue);
                    sendPnlDetailToGui(_pnlCalculator.getRelativeValuePnlDetail(contractIndex), StrategyId::RelativeValue, contractIndex);
                    sendPnlToGui(_pnlCalculator.getAggregatedPnl(), StrategyId::MarketMaker);
                    sendPnlDetailToGui(_pnlCalculator.getMarketMakerPnlDetail(contractIndex), StrategyId::MarketMaker, contractIndex);
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
                case ViperMessageType::LoginReply: {
                    handleLoginReply();
                    break;
                }
                case ViperMessageType::LogoutReply: {
                    handleLogoutReply();
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

    void handleLoginReply() {
        ++_numberOFStateReplies;
        if (_numberOFStateReplies == 2) {
            if (_relativeValue->_systemState == SystemState::LoggedIn && _marketMakers->_systemState == SystemState::LoggedIn) {
                proto::GUIInitialization m;
                _guiThings.populateGUIInitialization(m, _relativeValue->_pricingMatrix, _relativeValue->_riskMatrix);
                _clientAcceptor->handleLoginAccepted(m);
                _systemState = SystemState::LoggedIn;
            } else {
                _clientAcceptor->handleLoginRejected();
            }
            _numberOFStateReplies = 0;
        }
    }

    void handleLogoutReply() {
        ++_numberOFStateReplies;
        if (_numberOFStateReplies == 2) {
            _clientAcceptor->handleLoggedOut();
            if (_relativeValue->_systemState != SystemState::Disabled || _marketMakers->_systemState != SystemState::Disabled) {
                _logger.logError("Logout appears to not work doing hard shutdown");
                _relativeValue->_newSystemState = SystemState::HardShutdown;
                _marketMakers->_newSystemState = SystemState::HardShutdown;
                _systemState = SystemState::HardShutdown;
            }
            _numberOFStateReplies = 0;
        }
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
    }

    void sendOverdeterminedPricesUpdate() {
        proto::GridUpdate message;
        _guiThings.populateVectorPrices(message, _relativeValue);
        _clientAcceptor->sendGridUpdate(message);
        proto::GridUpdate contractMessage;
        _guiThings.populateContractPrices(contractMessage, _relativeValue);
        _clientAcceptor->sendGridUpdate(contractMessage);
    }

    void sendQuotesToGUI() {
        proto::GridUpdate message;
        _guiThings.populateContractQuotes(message, _relativeValue);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendStfToGui() {
        proto::GridUpdate vectorMessage;
        _guiThings.populateVectorStf(vectorMessage, _relativeValue);
        _clientAcceptor->sendGridUpdate(vectorMessage);
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
                case ViperMessageType::LoginReply: {
                    handleLoginReply();
                    break;
                }
                case ViperMessageType::LogoutReply: {
                    handleLogoutReply();
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
                case ViperMessageType::CriticalMessage: {
                    CriticalMessage& m = reinterpret_cast<CriticalMessage&> (message._data[0]);
                    sendCriticalMessage(m);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    sendFillToGUI(fill);
                    sendPlaySound(SoundId::Fill);
                    doPnl(fill);
                    _logger.logInfo(boost::str(boost::format("Fill. Strategy=%1% Type=%2% Contract=%3% Side=%4% Price=%5% Size=%6%")
                                        % getStrategyString(fill._strategy) % getFillTypeString(fill._type) % _config._allContracts[fill._contractId]._name
                                        % getSideString(fill._side) % ToDollars(fill._price) % fill._qty));
                    break;
                }
                default:
                    break;
            }
        }
    }

    void doPnl(FillMessage& fill) {
        switch (fill._strategy) {
            case StrategyId::RelativeValue: {
                _pnlCalculator.onFillFromRelativeValue(fill);
                AggregatedPnl& pnl = _pnlCalculator.getAggregatedPnl();
                checkPnlLimit(pnl);
                checkRvPnlLimit(pnl);
                sendPnlToGui(pnl, StrategyId::RelativeValue);
                sendPnlDetailToGui(_pnlCalculator.getRelativeValuePnlDetail(fill._contractId), StrategyId::RelativeValue, fill._contractId);
                break;
            }
            case StrategyId::MarketMaker: {
                _pnlCalculator.onFillFromMarketMaker(fill);
                AggregatedPnl& pnl = _pnlCalculator.getAggregatedPnl();
                checkPnlLimit(pnl);
                checkMMPnlLimit(pnl);
                sendPnlToGui(pnl, StrategyId::MarketMaker);
                sendPnlDetailToGui(_pnlCalculator.getMarketMakerPnlDetail(fill._contractId), StrategyId::MarketMaker, fill._contractId);
                break;
            }
            case StrategyId::Execution: {
                // TODO
                break;
            }
        }
    }

    void checkPnlLimit(AggregatedPnl& pnl) {
        const hf::price_t totalPnl = pnl._total._open + pnl._total._closed;
        if (totalPnl < _pnlLimit) {
            _logger.logInfo(boost::str(boost::format("Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(_pnlLimit) % ToDollars(totalPnl)));
            onSystemStateChange(proto::SystemState::SystemStateDisabled);
        }
    }

    void checkRvPnlLimit(AggregatedPnl& pnl) {
        const hf::price_t totalPnl = pnl._total._open + pnl._total._closed;
        if (totalPnl < _strategyPnlLimits[StrategyLimitIndexRV]) {
            _logger.logInfo(boost::str(boost::format("RV Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(_pnlLimit) % ToDollars(totalPnl)));
            sendGenericMessageToRV(ViperMessageType::SwitchChange, static_cast<int32_t>(MainControlAction::Enabled), 0);
        }
    }

    void checkMMPnlLimit(AggregatedPnl& pnl) {
        const hf::price_t totalPnl = pnl._total._open + pnl._total._closed;
        if (totalPnl < _strategyPnlLimits[StrategyLimitIndexMM]) {
            _logger.logInfo(boost::str(boost::format("MM Pnl limit breached. Limit=%1% Pnl=%2%") % ToDollars(_pnlLimit) % ToDollars(totalPnl)));
            sendGenericMessageToMM(ViperMessageType::SwitchChange, static_cast<int32_t>(MainControlAction::Enabled), 0);
        }
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

    void sendCriticalMessage(CriticalMessage& m) {
        proto::GridUpdate message;
        _guiThings.populateCriticalMessage(message, m);
        _clientAcceptor->sendGridUpdate(message);
    }

    void onGridUpdate(proto::GridUpdate& update) {
        auto it = _gridUpdates.find(update.gridid());
        if (it != _gridUpdates.end()) {
            GridUpdate& gridUpdate = it->second;
            if (gridUpdate._ready) {
                gridUpdate._update = update;
                const WindowIds windowId = static_cast<WindowIds>(update.gridid());
                if (windowId == WindowIds::PNL) {
                    handlePnlLimitChange(gridUpdate);
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

    void handlePnlLimitChange(GridUpdate& update) {
        for (auto& one : update._update.cells()) {
            PNLRows type = static_cast<PNLRows>(one.row());
            hf::price_t limit = ToRhonePrice(boost::lexical_cast<int64_t>(one.value()));
            switch (type) {
                case PNLRows::MarketMakers: {
                    _strategyPnlLimits[StrategyLimitIndexMM] = limit;
                    AggregatedPnl& pnl = _pnlCalculator.getAggregatedPnl();
                    checkMMPnlLimit(pnl);
                    break;
                }
                case PNLRows::RelativeValue: {
                    _strategyPnlLimits[StrategyLimitIndexRV] = limit;
                    AggregatedPnl& pnl = _pnlCalculator.getAggregatedPnl();
                    checkRvPnlLimit(pnl);
                    break;
                }
                case PNLRows::Total: {
                    _pnlLimit = limit;
                    AggregatedPnl& pnl = _pnlCalculator.getAggregatedPnl();
                    checkPnlLimit(pnl);
                    break;
                }
                default:
                    break;
            }
        }
    }

    bool isUpdateForMarketMaker(WindowIds id) {
        return id == WindowIds::MarketMakerParameters || id == WindowIds::StackerParameters || id == WindowIds::ExecutionParameters;
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
                sendGenericMessageToRV(ViperMessageType::Login);
                sendGenericMessageToMM(ViperMessageType::Login);
                break;
            }
            case ViperMessageType::Logout: {
                sendGenericMessageToRV(ViperMessageType::Logout);
                sendGenericMessageToMM(ViperMessageType::Logout);
                break;
            }
            case ViperMessageType::ClientDown: {
                sendGenericMessageToRV(ViperMessageType::ClientDown);
                sendGenericMessageToMM(ViperMessageType::ClientDown);
                break;
            }
            default:
                break;
        }
    }

    void onGuiInitializationAccepted() {
        _guiInitialized = true;
    }

    void onGuiInitializationRejected() {
        _logger.logErrorAndThrow("GUI initialization failed;");
    }

    void onSystemStateChange(proto::SystemState ns) {
        SystemState newState = toHf(ns);
        if (newState != _systemState) {
            _logger.logError("New State: " + (int) newState);
            _relativeValue->_newSystemState = newState;
            _marketMakers->_newSystemState = newState;
            _systemState = newState;
        }
    }

    void onSwitchChange(proto::SwitchChange& change) {
        auto& cell = change.cell();

        MainControlAction action = static_cast<MainControlAction>(cell.col());

        if (action != MainControlAction::Enabled && action != MainControlAction::Quoting) {
            //TODO - send current state
            _clientAcceptor->sendSwitchChangeReply(cell.row(), cell.col(), cell.value(), toUInt(WindowIds::MainControl), false);
            return;
        }

        int32_t value = atoi(cell.value().c_str());

        StrategyId strategyId = static_cast<StrategyId>(cell.row());

        switch (strategyId) {
            case StrategyId::MarketMaker: {
                sendGenericMessageToMM(ViperMessageType::SwitchChange, cell.col(), value);
                break;
            }
            case StrategyId::RelativeValue: {
                sendGenericMessageToRV(ViperMessageType::SwitchChange, cell.col(), value);
                break;
            }
            default: {
                //TODO send current state
                _clientAcceptor->sendSwitchChangeReply(cell.row(), cell.col(), cell.value(), toUInt(WindowIds::MainControl), false);
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
    MarketMakersT* _marketMakers;

    GUIThings _guiThings;
    bool _guiInitialized = false;

    SystemState _systemState = SystemState::LoggedOut;
    uint32_t _numberOFStateReplies = 0;

    PnlCalculator _pnlCalculator;
    uint64_t _nextRefreshTime = 0;
    uint64_t _refreshInterval;

    hf::price_t _pnlLimit = 0;
    hf::price_t _strategyPnlLimits[2];

    Logger _logger;

};

}

#endif //VIPER_FRONT_SIDE_H

