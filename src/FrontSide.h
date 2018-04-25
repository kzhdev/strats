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
#include <functional>
#include <md/sym/SymbolsResource.h>
#include <hf/queue/DebugQueueResource.h>
#include <hf/queue/DebugQueueLogger.h>
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
                                                 WindowIds::PNL, WindowIds::GlobalConfigs};
        for (auto id : updatableGrids) {
            _gridUpdates.insert({static_cast<uint32_t>(id), GridUpdate()});
        }
    }

    void onPostCreate(const libconfig::Setting& config) override {
        setUpQueues(config);

        const auto symbolsName = hf::ConfigManager::getString(config, "symbols");
        _symbolsResource = dynamic_cast<md::sym::SymbolsResource *> (hf::ResourceManager::getResourceP(symbolsName));
        if (!_symbolsResource) {
           throw std::runtime_error("could not get symbols resource");
        }

        const auto name = hf::ConfigManager::getString(config, "config_file");
        const bool isConfigGood = _config.initialize(name, *_symbolsResource);
        if (!isConfigGood) {
            GetLogger().logErrorAndThrow("Config is no good.");
        }

        populateSymbolToInterId();

        _guiThings.init(&_config, &_symbolToInterId);

        const std::string relativeValueName = config["RelativeValue"];
        _relativeValue = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(relativeValueName));

        const std::string marketMakersName = config["MarketMakers"];
        _marketMakers = dynamic_cast<ExecutorT*> (hf::ResourceManager::getResourceP(marketMakersName));

        _relativeValue->init(_config, _frequency, _systemState, std::bind(&FrontSide::isPnlWithinLimits, this, std::placeholders::_1));
        _marketMakers->init(_config, _frequency, _systemState, std::bind(&FrontSide::isPnlWithinLimits, this, std::placeholders::_1));

        uint32_t clips = boost::lexical_cast<uint32_t>(_config.getGlobal(NEGATIVE_SCALP_CLIPS));
        hf::price_t dollars = ToRhonePrice(boost::lexical_cast<uint32_t>(_config.getGlobal(NEGATIVE_SCALP_DOLLARS)));
        uint32_t period = boost::lexical_cast<uint32_t>(_config.getGlobal(NEGATIVE_SCALP_PERIOD));
        uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
        _negativeScalpChecker.initialize(periodInCycles, dollars, clips);
        _pnlCalculatorForNegativeScalpCHecker.init(_config._allContracts, &_negativeScalpChecker);

        _pnlCalculator.init(_config._allContracts);
        _pnlLimitsChecker.initialize(&_pnlCalculator, _config);

        _refreshInterval = GetCyclesFromSeconds(1, _frequency);
        _chartRefreshInterval = GetCyclesFromMilliSeconds(100, _frequency);

        const std::string loggerName = config["kdb_logger"];
        _kdbLogger = &(dynamic_cast<hf::DebugQueueLogger*> (hf::ResourceManager::getResourceP(loggerName)))->queue();
    }

    bool isPnlWithinLimits(uint32_t id) {
        return !_pnlLimitsChecker.checkLimits(static_cast<StrategyId>(id));
    }

    void setUpQueues(const libconfig::Setting& config) {
        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_to_market_maker");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _outToMM = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_outToMM) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_from_market_maker");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _inFromMM = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_inFromMM) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_to_relative_value");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _outToRV = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_outToRV) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_from_relative_value");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _inFromRV = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_inFromRV) {
                GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        _inFromMM->resetR();
        _inFromRV->resetR();
        _outToMM->resetW();
        _outToRV->resetW();
    }

     void populateSymbolToInterId() {
        for (uint32_t i = 0; i < _config._allContracts.size(); ++i) {
            auto& contract = _config._allContracts[i];
            _symbolToInterId.insert({contract._name, i});
        }
    }

    void onRun() override {
        setKdbLogger(_kdbLogger);
        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            _clientAcceptor->runIteration();
            processOutgoingMessagesFromRelativeValue();
            processOutgoingMessagesFromMarketMaker();
            refreshThings();
        }
    }

    bool isLoggedIn() {
        return _marketMakers->isLoggedIn();
    }

    void refreshThings() {
        if (isLoggedIn()) {
            const uint64_t now = clockUtil::GetCycles();
            if (now > _nextRefreshTime) {
                sendPnlStateToGui();
                sendMessageCountsToGui();
                sendMarketMakerStateToGui();
                sendRelativeValueGridStateToGui();
                sendVectorGridStateToGui();
                _nextRefreshTime = now + _refreshInterval;
            }
            if (now > _nextChartRefreshTime) {
                sendChartsUpdateToGui();
                _nextChartRefreshTime = now + _chartRefreshInterval;
            }
        }
    }

    void sendRelativeValueGridStateToGui() {
        proto::GridUpdate message;
        bool checkForCross = _systemState._relativeValue == SystemState::CheckQuotes;
        _guiThings.populateRelativeValueGridUpdate(message, _relativeValue, checkForCross);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendVectorGridStateToGui() {
        proto::GridUpdate message;
        _guiThings.populateVectorGridUpdate(message, _relativeValue);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendPnlStateToGui() {
        for (uint32_t contractIndex = _config._indexOfFirstTradedContract; contractIndex < _config._allContracts.size(); ++contractIndex) {
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
                    onSystemStateChange(SystemState::Disabled, "PNL limits breached - refreshing MD");
                }
                if (_pnlLimitsChecker.checkLimits(StrategyId::MarketMaker)) {
                    onSystemStateChange(SystemState::Disabled, "PNL limits breached - refreshing MD");
                }
            }
        }
    }

    void sendChartsUpdateToGui() {
        proto::DataSeriesPointSet pointsSet;
        _guiThings.populateChartDataPoints(pointsSet, _relativeValue);
        _clientAcceptor->sendChartDataPoints(pointsSet);
    }

    void processOutgoingMessagesFromRelativeValue() {
        while (true) {
            uint64_t idx = _inFromRV->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromRV->freeNext();

            auto& message = *static_cast<ViperMessage*>(_inFromRV->getPoolP()->getData(idx));

            switch (message._type) {
                case ViperMessageType::SwitchChangeReply: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage &> (message._data[0]);
                    _clientAcceptor->sendSwitchChangeReply(toUInt(StrategyId::RelativeValue), toUInt(reply._number_1), std::to_string(reply._number_2), toUInt(WindowIds::MainControl), true);
                    sendSwitchSound(reply._number_2);
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
                    sendPlaySound(SoundId::Error);
                    break;
                }
                case ViperMessageType::QuoteSizesUpdate: {
                    sendQuoteSizesToGUI();
                    break;
                }
                case ViperMessageType::AcceptedManualFill: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    uint32_t id = reply._number_1;
                    hf::qty_t size = reply._number_2;
                    _config.addPosition(id, size);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    handleFill(fill);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void sendSwitchSound(uint32_t state) {
        switch (state) {
            case proto::SystemState::SystemStateEnabled:
                sendPlaySound(SoundId::Enabled);
                break;
            case proto::SystemState::SystemStateDisabled:
                sendPlaySound(SoundId::Disable);
                break;
            case proto::SystemState::SystemStateHardShutdown:
                sendPlaySound(SoundId::Shutdown);
                break;
            default:
                break;
        }
    }

    void sendQuoteSizesToGUI() {
        proto::GridUpdate message;
        _guiThings.populateQuoteSizes(message, _relativeValue->_quoteSizes);
        _clientAcceptor->sendGridUpdate(message);
        proto::GridUpdate hedgeRatios;
        _guiThings.updateHedgeRatios(hedgeRatios, _relativeValue->_hedgeRatios);
        _clientAcceptor->sendGridUpdate(hedgeRatios);
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

    void processOutgoingMessagesFromMarketMaker() {
        while (true) {
            uint64_t idx = _inFromMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromMM->freeNext();

            auto& message = *static_cast<ViperMessage*>(_inFromMM->getPoolP()->getData(idx));

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
                    sendSwitchSound(reply._number_2);
                    break;
                }
                case ViperMessageType::MarketMakerEnabled: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    uint32_t id = reply._number_1;
                    bool enabled = reply._number_2 == 1;
                    sendMarketMakerEnabled(id, enabled);
                    if (!enabled) {
                        sendPlaySound(SoundId::Disable);
                    }
                    break;
                }
                case ViperMessageType::ErrorMessage: {
                    ErrorMessage& m = reinterpret_cast<ErrorMessage&> (message._data[0]);
                    sendErrorMessage(m);
                    sendPlaySound(SoundId::Error);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    handleFill(fill);
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
                case ViperMessageType::AcceptedManualFill: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    uint32_t id = reply._number_1;
                    hf::qty_t size = reply._number_2;
                    _config.addPosition(id, size);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void handleFill(FillMessage& fill) {
        sendFillToGUI(fill);
        sendPlaySound(SoundId::Fill);
        doPnl(fill);
        updateConfigPosition(fill);
        LOG(0, "Fill. Strategy={i1} Type={i2} Contract={C[i3]} Side={i4} Price={d1} Size={i5}", toUInt(fill._strategy), toUInt(fill._type), getContractName(fill._contractId), fill._side, ToDollars(fill._price),fill._qty)
    }

    void updateConfigPosition(FillMessage& fill) {
        hf::qty_t qty = fill._side == BID ? fill._qty : -fill._qty;
        _config.addPosition(fill._contractId, qty);
    }

    void doPnl(FillMessage& fill) {
        _pnlCalculator.onFill(fill);
        if (_pnlLimitsChecker.checkLimits(fill._strategy)) {
            _pnlLimitsChecker.logState();
            onSystemStateChange(SystemState::Disabled, "PNL limits breached - on fill.");
        }
        sendPnlDataToGui(fill._contractId, fill._strategy);
        _negativeScalpChecker.expireEntries(clockUtil::GetCycles());
        _pnlCalculatorForNegativeScalpCHecker.onFill(fill);
        if (_negativeScalpChecker.isOverLimit()) {
            onSystemStateChange(SystemState::Disabled, "Negative scalp checker breached - on fill.");
        }
    }

    void sendPnlDataToGui(uint32_t contractId, StrategyId strategyId) {
        sendPnlToGui(_pnlCalculator.getAggregatedPnl(), strategyId);
        sendPnlDetailToGui(_pnlCalculator.getPnlDetail(contractId, StrategyId::RelativeValue), _pnlCalculator.getPnlDetail(contractId, StrategyId::MarketMaker), strategyId, contractId);
    }

    void sendPnlToGui(AggregatedPnl& pnl, StrategyId id) {
        proto::GridUpdate pnlMessage;
        _guiThings.populatePnlUpdate(pnlMessage, pnl, id);
        _clientAcceptor->sendGridUpdate(pnlMessage);
    }

    void sendPnlDetailToGui(Pnl& rvPnl, Pnl& mmPnl, StrategyId id, uint32_t contractIndex) {
        proto::GridUpdate pnlDetailMessage;
        _guiThings.populatePnlDetailUpdate(pnlDetailMessage, rvPnl, mmPnl, id, contractIndex);
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

    void sendMarketMakerStateToGui() {
        proto::GridUpdate message;
        _guiThings.populateMarketMakerState(message, _marketMakers);
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
                    onGridUpdateAccepted(toUInt(WindowIds::PNL));
                    if (limitsBreach) {
                        onSystemStateChange(SystemState::Disabled, "PNL limits breached - on param update.");
                    }
                } else if (windowId == WindowIds::GlobalConfigs) {
                    handleGlobalConfigChange(gridUpdate);
                } else if (windowId == WindowIds::ManualFillsEntry) {
                    sendGridUpdateToMM(gridUpdate);
                    sendGridUpdateToRv(gridUpdate);
                }
                else {
                    const bool isUpdateForMM = isUpdateForMarketMaker(windowId);
                    if (isUpdateForMM) {
                        sendGridUpdateToMM(gridUpdate);
                    } else {
                        sendGridUpdateToRv(gridUpdate);
                    }
                }
            } else {
                GetLogger().logError("received grid update but another already in flight");
            }
        } else {
            GetLogger().logError("received grid update for unknown grid " + update.gridid());
        }
    }

    void handleGlobalConfigChange(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _config._globalConfigs[gridCell.row()];
            if (config._key == NEGATIVE_SCALP_CLIPS) {
                uint32_t clips = boost::lexical_cast<uint32_t>(gridCell.value());
                _negativeScalpChecker.updateCountLimit(clips);
                LOG(0, "Negative Scalper Checkers. Updated count limit. Value={i1}", clips)
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if (config._key == NEGATIVE_SCALP_DOLLARS) {
                uint32_t value = boost::lexical_cast<uint32_t>(gridCell.value());
                _negativeScalpChecker.updatePnlLimit(ToRhonePrice(value));
                LOG(0, "Negative Scalper Checkers. Updated dollar limit. Value={i1}", value)
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if (config._key == NEGATIVE_SCALP_PERIOD) {
                uint32_t period = boost::lexical_cast<uint32_t>(gridCell.value());
                uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                _negativeScalpChecker.updatePeriodInCycles(periodInCycles);
                LOG(0, "Negative Scalper Checkers. Updated period. Period={i2}", period)
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if (config._key == RV_QUOTE_MAX_PERCENT_WIDTH) {
                sendGridUpdateToRv(update);
            }
            else if (config._key == QUOTE_BANNER_PERIOD) {
                sendGridUpdateToMM(update);
            }
            else if (config._key == FILL_BANNER_PERIOD) {
                sendGridUpdateToMM(update);
            }
            else if (config._key == CHECK_HEDGE_QUOTE_PERIOD) {
                sendGridUpdateToMM(update);
            }
        }
    }

    bool isUpdateForMarketMaker(WindowIds id) {
        return id == WindowIds::MarketMakerParameters || id == WindowIds::StackerParameters;
    }

    void sendGridUpdateToRv(GridUpdate& update) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::GridUpdate;
        GridUpdateMessage& message = reinterpret_cast<GridUpdateMessage &> (outMessage._data[0]);
        message._gridUpdate = &update;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(GridUpdateMessage) + sizeof(ViperMessageType));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGridUpdateToMM(GridUpdate& update) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToMM->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::GridUpdate;
        GridUpdateMessage& reply = reinterpret_cast<GridUpdateMessage &> (outMessage._data[0]);
        reply._gridUpdate = &update;
        const auto outIdx = _outToMM->getPoolP()->append(sizeof(GridUpdateMessage) + sizeof(ViperMessageType));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void sendGenericMessageToRV(ViperMessageType type, int32_t action = 0, int32_t value = 0) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = type;
        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = action;
        message._number_2 = value;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendGenericMessageToMM(ViperMessageType type, int32_t action = 0, int32_t value = 0) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToMM->getPoolP()->getNextP());
        outMessage._type = type;
        GenericMessage& message = reinterpret_cast<GenericMessage &> (outMessage._data[0]);
        message._number_1 = action;
        message._number_2 = value;
        const auto outIdx = _outToMM->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToMM->appendNew(outIdx);
    }

    void onGridUpdateAccepted(uint32_t gridId) {
        auto it = _gridUpdates.find(gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            _config.applyGridUpdate(gu);
            gu.clear();
            proto::GridUpdate message;
            message.set_gridid(gridId);
            message.set_messageid(0);
            _clientAcceptor->sendGridChangeReply(message);
        } else {
            GetLogger().logError("Accepted grid update which cannot be found");
        }
    }

    void onGridUpdateRejected(uint32_t gridId) {
        auto it = _gridUpdates.find(gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            gu.clear();
            proto::GridUpdate message;
            message.set_gridid(gridId);
            message.set_messageid(0);
            _clientAcceptor->sendGridChangeReply(message);
        } else {
            GetLogger().logError("Rejected grid update which cannot be found");
        }
    }

    void onOtherRequest(ViperMessageType mt)  {
        switch (mt) {
            case ViperMessageType::Login: {
                onSystemStateChange(SystemState::LoggedIn, "GUI Requested");
                proto::GUIInitialization m;
                _guiThings.populateGUIInitialization(m, _relativeValue->_pricingMatrix, _relativeValue->_riskMatrix, _relativeValue->_hedgeRatios, _relativeValue->_quoteSizes);
                _clientAcceptor->handleLoginAccepted(m);
                break;
            }
            case ViperMessageType::Logout: {
                onSystemStateChange(SystemState::LoggedOut, "GUI Requested");
                break;
            }
            case ViperMessageType::ClientDown: {
                GetLogger().logError("Client down - doing hard shutdown");
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
        GetLogger().logErrorAndThrow("GUI initialization failed;");
    }

    void onSystemStateChange(SystemState newState, const std::string& reason) {
        if (newState != SystemState::CheckQuotes) {
            onStrategySystemStateChange(newState, _systemState._marketMaker, reason, "MarketMaker");
        }
        if (newState == SystemState::CheckQuotes) {
            if (_systemState._relativeValue == SystemState::Disabled || _systemState._relativeValue == SystemState::LoggedIn) {
                onStrategySystemStateChange(newState, _systemState._relativeValue, reason, "RelativeValue");
            }
        } else {
            onStrategySystemStateChange(newState, _systemState._relativeValue, reason, "RelativeValue");
        }
    }

    void onStrategySystemStateChange(SystemState newState, SystemState& currentState, const std::string& reason, const std::string& strategy) {
        if (currentState == SystemState::HardShutdown) {
            hf::Service::exitAll();
        } else {
            if (newState != currentState) {
                std::string message = boost::str(boost::format("Request State Change: Strategy=%1% State=%2% Reason=%3%") % strategy % GetSystemStateString(newState) % reason);
                GetLogger().logInfo(message);
                sendErrorMessage(message, ErrorType::Critical);
                currentState = newState;
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
                if (MainControlAction::Quoting == action) {
                    sendGenericMessageToMM(ViperMessageType::Quoting, toUInt(state));
                }
                break;
            }
            case StrategyId::RelativeValue: {
                if (MainControlAction::Enabled == action) {
                    onStrategySystemStateChange(state, _systemState._relativeValue, "GUI requested", "RelativeValue");
                }
                if (MainControlAction::Quoting == action) {
                    sendGenericMessageToRV(ViperMessageType::Quoting, toUInt(state));
                }
                break;
            }
            default: {
                break;
            }
        }
    }


#ifdef IS_DEV
        std::string getContractName(uint32_t id) {
            return _config._allContracts[id]._name;
        }
#else
        uint32_t getContractName(uint32_t id) {
            return id;
        }
#endif

    private:
    ClientsAcceptor* _clientAcceptor;

    md::queue::MDQueueResource* _inFromRV;
    md::queue::MDQueueResource* _inFromMM;
    md::queue::MDQueueResource* _outToRV;
    md::queue::MDQueueResource* _outToMM;

    ViperConfig _config;
    std::unordered_map<uint32_t, GridUpdate> _gridUpdates;

    md::sym::SymbolsResource* _symbolsResource;

    std::unordered_map<std::string, uint32_t> _symbolToInterId;

    RelativeValue* _relativeValue;
    ExecutorT* _marketMakers;

    GUIThings _guiThings;

    SharedSystemState _systemState;

    PnlCalculator _pnlCalculator;
    PnlLimitsChecker _pnlLimitsChecker;

    PnlCalculatorsForNegativeScalpChecker _pnlCalculatorForNegativeScalpCHecker;
    NegativeScalpChecker _negativeScalpChecker;

    uint64_t _nextRefreshTime = 0;
    uint64_t _refreshInterval;

    uint64_t _nextChartRefreshTime = 0;
    uint64_t _chartRefreshInterval;

    double _frequency;

    hf::queue::DebugQueue* _kdbLogger;
};

}

#endif //VIPER_FRONT_SIDE_H

