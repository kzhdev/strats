#ifndef VIPER_FRONT_SIDE_H
#define VIPER_FRONT_SIDE_H

#include <experimental/filesystem>
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
#include <sys/types.h>
#include <signal.h>
#include <ven/VenueUtils.h>

namespace Python {

class FrontSide : public ViperResource, public virtual hf::Service {
public:
    static constexpr const char* const NAME = "FrontSide";

    FrontSide() : _guiThings(GUIThings::get()) {
    }

    ResourceId id() const noexcept override { return ResourceId::FrontSide; }

    void onCreate(const libconfig::Setting& config) override;

    void createGridUpdates() {
        std::vector<WindowIds> updatableGrids = {WindowIds::VectorParameters, WindowIds::ContractParametersRv, WindowIds::PricingConfig, WindowIds::ManualFillsEntry,
                                                 WindowIds::MarketMakerParameters, WindowIds::LeanMatrix, WindowIds::StackerParameters, WindowIds::ContractParametersRv,
                                                 WindowIds::PNL, WindowIds::GlobalConfigs, WindowIds::TraderSwitch, WindowIds::GTCManagement, WindowIds::StatusDebug};
        for (auto id : updatableGrids) {
            _gridUpdates.insert({static_cast<uint32_t>(id), GridUpdate()});
        }
    }

    void onPostCreate(const libconfig::Setting& config) override {
        ViperResource::_onPostCreate(config);

        const auto symbolsName = hf::ConfigManager::getString(config, "symbols");
        _symbolsResource = dynamic_cast<md::sym::SymbolsResource *> (hf::ResourceManager::getResourceP(symbolsName));
        if (!_symbolsResource) {
           throw std::runtime_error("could not get symbols resource");
        }

        _viperConfig._iceCancelResting = hf::ConfigManager::getBool(config, "ice_cancel_resting", true);
        _viperConfig._allowGTCs = hf::ConfigManager::getBool(config, "allowGTCs", false);
        _viperConfig._allowStatusGrid = hf::ConfigManager::getBool(config, "allowStatusGrid", false);
        const auto modelCsv = static_cast<std::string>(std::experimental::filesystem::canonical(hf::ConfigManager::getString(config, "config_file")));
        const auto rvPosCsv = static_cast<std::string>(std::experimental::filesystem::canonical(hf::ConfigManager::getString(config, "rv_pos_file")));
        const auto mmLastFillPriceCsv = static_cast<std::string>(std::experimental::filesystem::canonical(hf::ConfigManager::getString(config, "mm_last_fill_price_file")));
        const bool isConfigGood = _viperConfig.initialize(modelCsv, rvPosCsv, mmLastFillPriceCsv, *_symbolsResource);
        if (!isConfigGood) {
            logging::GetLogger().logErrorAndThrow("Config is no good.");
        }

        _guiThings.init(&_viperConfig);

        const auto internalHeartbeat = hf::ConfigManager::getULong(config, "internal_heartbeat");
        uint64_t internalHeartbeatPeriod = GetCyclesFromMilliSeconds(internalHeartbeat, _frequency);

        auto guiHeartbeatInMs = DEFAULT_GUI_HEARTBEAT_INTERVAL_IN_MS;
        if(config.exists("send_gui_heartbeat_interval_in_ms")) {
            guiHeartbeatInMs = hf::ConfigManager::getULong(config, "send_gui_heartbeat_interval_in_ms", -1);
            if(guiHeartbeatInMs == -1) {
                logging::GetLogger().logErrorAndThrow("Could not parse value for send_gui_heartbeat_interval_in_ms");
            }
            else if(guiHeartbeatInMs <= 0) {
                logging::GetLogger().logErrorAndThrow("send_gui_heartbeat_interval_in_ms must be positive");
            }
            logging::GetLogger().logInfo(std::string("GUI heartbeat interval override.  value=") + std::to_string(guiHeartbeatInMs) + "ms");
        }
        else {
            logging::GetLogger().logInfo(std::string("GUI heartbeat interval=") + std::to_string(guiHeartbeatInMs) + "ms");
        }
        _guiHeartbeatInterval = GetCyclesFromMilliSeconds(guiHeartbeatInMs, _frequency);

        if (!config.exists("transaction_capacity")) {
            logging::GetLogger().logErrorAndThrow("Not configured: transaction_capacity");
        }

        const libconfig::Setting& tcsConfig = config["transaction_capacity"];
        std::map<hf::VENUE, uint32_t> transactionCapacity;
        for (int i = 0; i < tcsConfig.getLength(); ++i) {
            const libconfig::Setting& tcConfig = tcsConfig[i];
            const auto venueString = hf::ConfigManager::getString(tcConfig, "venue");
            const hf::VENUE venue = ven::VenueUtils::getID(venueString);
            uint32_t tranCapacity = hf::ConfigManager::getLong(tcConfig, "value");
            transactionCapacity.insert({venue, tranCapacity});
        }

        if (!config.exists("iid")) {
            logging::GetLogger().logErrorAndThrow("Not configured: iid");
        }
        const auto iid = hf::ConfigManager::getULong(config, "iid");

        _internalHeartbeatTimestamps.resize(toUInt(StrategyId::MAX));
        for (auto strategy : _strategies) {
            strategy->init(_frequency, internalHeartbeatPeriod, std::bind(&FrontSide::isPnlWithinLimits, this, std::placeholders::_1));
            _internalHeartbeatTimestamps[toUInt(strategy->strategyId())] = 0;
        }
        _internalHeartbeatTimestamps[toUInt(StrategyId::Execution)] = 0;

        _executor = dynamic_cast<Executor*>(hf::ResourceManager::getResourceP("Viper"));
        _executor->init(_frequency, internalHeartbeatPeriod, transactionCapacity, iid);

        _maxInternalHeartbeatTime = GetCyclesFromMilliSeconds(internalHeartbeat * 2, _frequency);

        uint32_t clips = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(NEGATIVE_SCALP_CLIPS));
        hf::price_t dollars = ToRhonePrice(boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(NEGATIVE_SCALP_DOLLARS)));
        uint32_t period = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(NEGATIVE_SCALP_PERIOD));
        uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
        _negativeScalpChecker.initialize(periodInCycles, dollars, clips);
        _pnlCalculatorForNegativeScalpChecker.init(_viperConfig._allContracts, &_negativeScalpChecker);

        uint64_t fcPeriod = GetCyclesFromMilliSeconds(boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(FILL_RATE_PERIOD)), _frequency);
        uint32_t fcClips = boost::lexical_cast<uint32_t>(_viperConfig.getGlobal(FILL_RATE_CLIPS));
        uint64_t fcSumTotalLimit = boost::lexical_cast<uint64_t>(_viperConfig.getGlobal(FILL_RATE_SUM_TOTAL));
        _fillRateChecker.initialize(fcClips, fcPeriod, fcSumTotalLimit);

        _pnlCalculator.init(_viperConfig._allContracts);
        _pnlLimitsChecker.initialize(&_pnlCalculator, _viperConfig);

        _refreshInterval = GetCyclesFromSeconds(1, _frequency);
        _chartRefreshInterval = GetCyclesFromMilliSeconds(200, _frequency);
        _ltfStoreInterval = GetCyclesFromSeconds(15, _frequency);
        _versionSendingTime = GetCyclesFromSeconds(20, _frequency);

        for(unsigned i = 0; i < _viperConfig._marketMakerConfigs.size(); ++i) {
            const auto& sym = _viperConfig._marketMakerConfigs[i]._contract;
            _internalIdToMarketMakerConfigOffset[_viperConfig._symbolToInterId[sym]] = i;
        }
    }

    bool isPnlWithinLimits(uint32_t id) {
        return !_pnlLimitsChecker.checkLimits(static_cast<StrategyId>(id));
    }

    void onRun() override {
        if (_kdbLogger) {
            logging::GetLogger().logInfo(boost::str(boost::format("FS kdb logger %1%") % _kdbLogger));
            _kdbLogger->resetW();
            logging::setKdbLogger(_kdbLogger);
        }
        while (BOOST_LIKELY(!hf::Service::isExitAll())) {
            _clientAcceptor->runIteration();
            processOutgoingMessages();
            refreshThings();
            uint64_t heartbeatTimestamp = clockUtil::GetCycles();
            checkInternalHeartbeats(heartbeatTimestamp);
            sendGUIHeartbeat(heartbeatTimestamp);
        }
    }

    void checkInternalHeartbeats(uint64_t now_in_cycles) {
        bool isBad = false;
        for (uint32_t i = 0; i < (toUInt(StrategyId::MAX)); ++i) {
            isBad |= checkInternalHeartbeat((StrategyId)(i), now_in_cycles);
        }
        if (isBad) {
            killProcess();
        }
    }

    void sendGUIHeartbeat(uint64_t now_in_cycles) {
        if(now_in_cycles > _guiHeartbeatTime) {
            _clientAcceptor->sendGuiHeartbeat(hf::util::TimeUtils::clock_gettime());
            _guiHeartbeatTime = now_in_cycles + _guiHeartbeatInterval;
        }
    }

    void killProcess() {
#define VIPER_GENERATE_CORE_ON_KILL
#ifdef VIPER_GENERATE_CORE_ON_KILL
        ::raise(SIGABRT);
#endif
        kill(getpid(), SIGKILL);
    }

    bool checkInternalHeartbeat(StrategyId id, uint64_t now) {
        uint64_t last = _internalHeartbeatTimestamps[toUInt(id)];
        bool breach = false;
        if (last > 0) {
            uint64_t elapsed = now - last;
            breach = elapsed > _maxInternalHeartbeatTime;
            if (breach) {
                std::string message = boost::str(boost::format("%1% not responsive.") % toString(id));
                sendErrorMessage(message, ErrorType::Critical);
                logging::GetLogger().logError(message);
            }
        }
        return breach;
    }

    bool isLoggedIn() {
        return _strategies[toUInt(StrategyId::MarketMaker)]->state() != SystemState::LoggedOut;
    }

    void refreshThings() {
        if (isLoggedIn()) {
            const uint64_t now = clockUtil::GetCycles();
            if (now > _nextRefreshTime) {
                sendPnlStateToGui();
                sendMessageCountsToGui();
                for (auto strategy : _strategies) {
                    proto::GridUpdate message;
                    strategy->sendStateToGUI([this](proto::GridUpdate& message) {
                        _clientAcceptor->sendGridUpdate(message);
                    });

                }
                sendPositionsToGui();
                _nextRefreshTime = now + _refreshInterval;
            }
            if (now > _nextChartRefreshTime) {
                sendChartsUpdateToGui();
                _nextChartRefreshTime = now + _chartRefreshInterval;
            }
            if (now > _nextLtfStoreTime) {
                storeLtfs();
                _nextLtfStoreTime = now + _ltfStoreInterval;
            }
            if (_loginTime > 0 && (now - _loginTime) >= _versionSendingTime) {
                sendViperVersion();
                for(const auto& position : _viperConfig._unallocatedRVPositionsFromFile) {
                    sendErrorMessage("Unallocated RV position loaded from file.  Contract=" + position.first + " Position=" + std::to_string(position.second), ErrorType::Critical);
                }
                _loginTime = 0;
            }
        }
    }

    void sendViperVersion() {
        boost::format versionString{"ViperVersion=%1% ViperGitHash=%2% RhonePublicGitHash=%3% ICECancelResting=%4%"};
        sendErrorMessage(boost::str(versionString % Python::ViperVersion % Python::ViperGitHash % Python::RhonePublicGitHash % _viperConfig._iceCancelResting), ErrorType::Normal);
    }

    void storeLtfs() {
        bool needToWrite = false;
        auto relativeValue = reinterpret_cast<RelativeValue*>(_strategies[toUInt(StrategyId::RelativeValue)]);
        for (uint32_t vectorIndex = 0; vectorIndex < relativeValue->_numberOfVectors; ++vectorIndex) {
            const bool useEma = relativeValue->_useEMAInVector.getValue(vectorIndex);
            if (useEma) {
                double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
                _viperConfig.updateVectorLtf(vectorIndex, ltf);
                needToWrite = true;
            }
        }
        if (needToWrite) {
            _viperConfig.writeToFile();
        }
    }

    void storeLtf(uint32_t vectorIndex) {
        auto relativeValue = reinterpret_cast<RelativeValue*>(_strategies[toUInt(StrategyId::RelativeValue)]);
        double ltf = relativeValue->_longTermFairsInVector.getValue(vectorIndex);
        _viperConfig.updateVectorLtf(vectorIndex, ltf);
        _viperConfig.writeToFile();
    }

    void sendPositionsToGui() {
        proto::GridUpdate message;
        _guiThings.updatePositions(message);
        _clientAcceptor->sendGridUpdate(message);
    }

    bool checkAndActOnPnlForStrategy(StrategyId strategyID, bool onFill, bool onParamUpdate, bool checkTotalLimitsRequired) {
        bool violationExists = false;
        auto* strategy = _strategies[toUInt(strategyID)];
        if(strategyID == StrategyId::MarketMaker) {
            //MM (pnl or drawdown breach) -> disable MM external quoting (MM stays enabled)
            if(_pnlLimitsChecker.checkStrategyLimits(StrategyId::MarketMaker)) {
                bool loggingRequired = strategy->canQuoteExternally();
                strategy->disableQuoteExternal();
                if (loggingRequired) {
                    sendErrorMessage("MM PnL breach.  OnFill=" + std::to_string(onFill) + "OnParamUpdate=" + std::to_string(onParamUpdate) + " Disable MM quote external", ErrorType::Critical);
                    LOG(0, "MM PnL breach.  Disable MM quote external. onFill={i1} onParamUpdate={i2}", onFill, onParamUpdate)
                }
                violationExists = true;
            }
        }
        else {
            //RV (and future strategies) (pnl or drawdown breach) -> disable strategy, disable strategy’s external quoting
            bool loggingRequired = strategy->isEnabled() | strategy->canQuoteExternally();
            if(_pnlLimitsChecker.checkStrategyLimits(strategy->strategyId())) {
                strategy->disable();
                strategy->disableQuoteExternal();
                if(loggingRequired) {
                    sendErrorMessage("RV PnL breach.  OnFill=" + std::to_string(onFill) + " OnParamUpdate=" + std::to_string(onParamUpdate) + " Disable RV and Disable RV quote external",ErrorType::Critical);
                    LOG(0, "PnL breach.  StrategyId={i1} Disable Strategy, Disable Quote External onFill={i2} onParamUpdate={i2}", toUInt(strategy->strategyId()), onFill, onParamUpdate);
                }
                violationExists = true;
            }
        }

        if (checkTotalLimitsRequired && _pnlLimitsChecker.checkTotalLimits()) {
            //Total( pnl or drawdown breach) -> disable external quoting on all exchanges, disable external quoting on all strategies, disable all strategies except MM
            bool loggingRequired = [&]() {
                return std::any_of(_strategies.begin(), _strategies.end(), [](const auto* strategy) {
                    if(strategy->strategyId() != StrategyId::MarketMaker && strategy->isEnabled()) {
                        return true;
                    }
                    return strategy->canQuoteExternally();
                });
            }();
            if(loggingRequired) {
                sendErrorMessage("Total PnL breach.  OnFill=" + std::to_string(onFill) + " OnParamUpdate=" + std::to_string(onParamUpdate) + " Disable External Quote on all Strategies.  Disable external quote on all exchanges.  Disable all strategies but MM", ErrorType::Critical);
            }
            DisableExternalQuotingOnAllMMInstrumentsKernel(s_systemState);
            sendStrategyDisableExternalQuoting();
            for(auto* strategy : _strategies) {
                if (strategy->strategyId() != StrategyId::MarketMaker) {
                    onStrategySystemStateChange(SystemState::Disabled, strategy->_state, s_systemState.strategyEnableBlocked(strategy->strategyId()),
                                                "PnL limits breached - Total PnL - OnFill=" + std::to_string(onFill) + " OnParamUpdate=" + std::to_string(onParamUpdate), strategyFullName(strategy->strategyId()));
                }
            }
            violationExists = true;
        }
        return violationExists;
    }

    //returns if any limits violated
    bool checkAndActOnPnl(bool onParamChange) {
        bool violationExists = false;
        for(int i = 0; i < toInt(StrategyId::Execution); ++i) {
            violationExists |= checkAndActOnPnlForStrategy(static_cast<StrategyId>(i), false, onParamChange, i == 0);
        }
        return violationExists;
    }

    void sendPnlStateToGui() {
        bool globalUpdatesNeeded = _canSendPnl;
        bool sendPnlPending = !_canSendPnl;
        auto relativeValue = reinterpret_cast<RelativeValue*>(_strategies[toUInt(StrategyId::RelativeValue)]);
        for (uint32_t contractIndex = _viperConfig._indexOfFirstTradedContract; contractIndex < _viperConfig._allContracts.size(); ++contractIndex) {
            ConfigContract& contract = _viperConfig._allContracts[contractIndex];
            hf::price_t price;
            if (contract._legs.size() > 1) {
                double sum = 0;
                for (auto& leg : contract._legs) {
                    sum += relativeValue->_overdeterminedPrices.getValue(0, leg._index) * leg._ratio;
                }
                price = ToRhonePrice(sum);
            } else {
                price = ToRhonePrice(relativeValue->_overdeterminedPrices.getValue(0, contractIndex));
            }

            if (_pnlCalculator.hasDifferentMarkPrice(contractIndex, price)) {
                _pnlCalculator.recalculateOpenPnl(contractIndex, price);
                for (auto* strategy : _strategies) {
                    sendPnlDataToGui(contractIndex, strategy->strategyId());
                }
                checkAndActOnPnl(false);
                _canSendPnl = true;
                globalUpdatesNeeded = false;
            }
        }

        if(globalUpdatesNeeded | (sendPnlPending & _canSendPnl)) {
            for (auto* strategy : _strategies) {
                sendAggregatePnLDataToGui(strategy->strategyId());
            }
        }
    }

    void sendChartsUpdateToGui() {
        auto relativeValue = reinterpret_cast<RelativeValue*>(_strategies[toUInt(StrategyId::RelativeValue)]);
        if (relativeValue->_hasDataForCharts) {
            proto::DataSeriesPointSet pointsSet;
            _guiThings.populateChartDataPoints(pointsSet, relativeValue);
            _clientAcceptor->sendChartDataPoints(pointsSet);
        }
    }

    void processOutgoingMessages() {
        while (_msgCursor != _msgQueue.end()) {
            auto& message = *reinterpret_cast<ViperMessage*>(&_msgQueue[_msgCursor].data[0]);
            _msgCursor += message._size;
            switch (message._type) {
                case ViperMessageType::SwitchChangeReply: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    auto id = toUInt(reply._number_3);
                    if (id < toUInt(StrategyId::Execution) * 2) {
                        _clientAcceptor->sendSwitchChangeReply(id, toUInt(reply._number_1), std::to_string(reply._number_2), toUInt(WindowIds::MainControl), true);
                    } else {
                        auto venue = id - toUInt(StrategyId::Execution) * 2;
                        _clientAcceptor->sendSwitchChangeReply(_guiThings.venueToMainControlRowIndex(venue), toUInt(reply._number_1), std::to_string(reply._number_2), toUInt(WindowIds::MainControl), true);
                    }
                    sendSwitchSound(reply._number_2);
                    break;
                }
                case ViperMessageType::GridUpdateAccepted: {
                    auto& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    onGridUpdateAccepted(reply._number_1);
                    break;
                }
                case ViperMessageType::ErrorMessage: {
                    ErrorMessage& m = reinterpret_cast<ErrorMessage&> (message._data[0]);
                    sendErrorMessage(m);
                    sendPlaySound((m._type == ErrorType::Critical ? SoundId::CriticalMessage : SoundId::NormalMessage));
                    break;
                }
                case ViperMessageType::QuoteSizesUpdate: {
                    sendQuoteSizesToGUI();
                    break;
                }
                case ViperMessageType::AcceptedManualFill: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&>(message._data[0]);
                    uint32_t id = reply._number_1;
                    hf::qty_t size = reply._number_2;
                    bool isInternal = reply._number_3 == 0;
                    _viperConfig.addPosition(id, size, (StrategyId)message._source, isInternal);
                    _viperConfig.writeToFile();
                    sendManualFillToAudit(id, size, (StrategyId)message._source);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage&> (message._data[0]);
                    handleFill(fill);
                    break;
                }
                case ViperMessageType::StoreLtf: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    storeLtf(reply._number_1);
                    break;
                }
                case ViperMessageType::InternalHeartbeat: {
                    _internalHeartbeatTimestamps[toUInt(message._source)] = clockUtil::GetCycles();
                    break;
                }

                case ViperMessageType::MarketMakerEnabled: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&>(message._data[0]);
                    uint32_t id = reply._number_1;
                    bool enabled = reply._number_2 == 1;
                    sendMarketMakerEnabled(id, enabled);
                    if (!enabled) {
                        disableMMInViperConfig(id);
                        sendPlaySound(SoundId::Disable);
                    }
                    break;
                }

                case ViperMessageType::GatewayStatus: {
                    GatewayStatusMessage& status = reinterpret_cast<GatewayStatusMessage &> (message._data[0]);

                    std::string m = status._message;
                    ErrorMessage em;
                    memcpy(em._message, m.c_str(), m.size());
                    em._size = m.size();
                    em._type = ErrorType::Critical;
                    sendErrorMessage(em);

                    proto::GridUpdate update;
                    _guiThings.populateGatewayStatusUpdate(update, status._active);
                    _clientAcceptor->sendGridUpdate(update);
                    break;
                }

                case ViperMessageType::QuoteExternallyOff: {
                    GenericMessage& reply = reinterpret_cast<GenericMessage&> (message._data[0]);
                    uint32_t internalId = reply._number_1;
                    uint32_t gridId = reply._number_2;
                    uint32_t columnId = reply._number_3;
                    if (gridId == toUInt(WindowIds::MarketMakerParameters)) {
                        _viperConfig._marketMakerConfigs[_internalIdToMarketMakerConfigOffset[internalId]]._quoteExternal = false;
                    }
                    sendQuoteExternallyOffGridUpdate(internalId, gridId, columnId);
                    break;
                }
                case ViperMessageType::DisableMarketMakerInstrument: {
                    GenericMessage& gm = reinterpret_cast<GenericMessage&> (message._data[0]);
                    auto& internal_id = gm._number_1;
                    disableMMInViperConfig(internal_id);
                    sendMarketMarketDisabledForInstrument(internal_id);
                    break;
                }
                case ViperMessageType::IOPUpdate: {
                    IOPUpdate& iopUpdate = reinterpret_cast<IOPUpdate &>(message._data[0]);
                    proto::GridUpdate gridUpdate;
                    if(_guiThings.tryPopulateIOPUpdate(iopUpdate, gridUpdate)) {
                        _clientAcceptor->sendGridUpdate(gridUpdate);
                    }
                    break;
                }
                case ViperMessageType::EndIOPTracking: {
                    proto::GridUpdate gridUpdate;
                    if(_guiThings.tryPopulateResetGTCGrid(gridUpdate)) {
                        _clientAcceptor->sendGridUpdate(gridUpdate);
                    }
                    break;
                }
                case ViperMessageType::AuctionMDUpdate: {
                    const AuctionMDUpdate& auctionMDUpdate = reinterpret_cast<AuctionMDUpdate &>(message._data[0]);
                    proto::GridUpdate gridUpdate;
                    if(_guiThings.tryPopulateAuctionMDUpdate(auctionMDUpdate, gridUpdate)) {
                        _clientAcceptor->sendGridUpdate(gridUpdate);
                    }
                    break;
                }
                case ViperMessageType::NumGTCUpdate: {
                    const GenericMessage& numGTCs = reinterpret_cast<GenericMessage &>(message._data[0]);
                    proto::GridUpdate gridUpdate;
                    if(_guiThings.tryPopulateNumGTCUpdate(numGTCs, gridUpdate)) {
                        _clientAcceptor->sendGridUpdate(gridUpdate);
                    }
                    break;
                }
                case ViperMessageType::DisableUseGTC: {
                    GenericMessage& gm = reinterpret_cast<GenericMessage&> (message._data[0]);
                    auto& internal_id = gm._number_1;
                    disableUseGTCInViperConfig(internal_id);
                    sendStatckerUseGTCDisabledForInstrument(internal_id);
                    break;
                }
                case ViperMessageType::MMWidthPercentageUpdate: {
                    sendMMWidthPercentageUpdateToGUI(toUInt(_viperConfig._currentShift));
                    break;
                }
                case ViperMessageType::EmaOnOff: {
                    auto& gm = reinterpret_cast<GenericMessage&> (message._data[0]);
                    uint32_t vectorId = gm._number_1;
                    bool isOn = gm._number_2;
                    _viperConfig._vectorConfigs[vectorId]._useEMA = isOn;
                    sendEMAOnUpdate(vectorId, isOn);
                    break;
                }

                default:
                    break;
            }
        }
    }

    void sendQuoteExternallyOffGridUpdate(uint32_t internalId, uint32_t gridId, uint32_t columnId) {
        proto::GridUpdate message;
        message.set_updatetype(proto::GridUpdateType::CellsType);
        message.set_gridid(gridId);
        auto cells = message.mutable_cells();
        auto cell = cells->Add();
        cell->set_row(internalId);
        cell->set_col(columnId);
        cell->set_value(std::to_string(false));
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendMarketMarketDisabledForInstrument(uint32_t internal_id) {
        proto::GridUpdate message;
        message.set_updatetype(proto::GridUpdateType::CellsType);
        message.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        auto cells = message.mutable_cells();
        auto cell = cells->Add();
        cell->set_row(internal_id);
        cell->set_col(toUInt(MarketMakerParametersColumns::Enabled));
        cell->set_value(std::to_string(false));
        _clientAcceptor->sendGridUpdate(message);
        sendPlaySound(SoundId::Disable);
    }

    void sendStatckerUseGTCDisabledForInstrument(uint32_t internal_id) {
        proto::GridUpdate message;
        message.set_updatetype(proto::GridUpdateType::CellsType);
        message.set_gridid(toUInt(WindowIds::StackerParameters));
        auto cells = message.mutable_cells();
        auto cell = cells->Add();
        cell->set_row(internal_id);
        cell->set_col(toUInt(StackerParametersColumns::UseGTC));
        cell->set_value(std::to_string(false));
        _clientAcceptor->sendGridUpdate(message);
        sendPlaySound(SoundId::Disable);
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
        auto relativeValue = reinterpret_cast<RelativeValue*>(_strategies[toUInt(StrategyId::RelativeValue)]);
        _guiThings.populateQuoteSizes(_viperConfig, message, relativeValue->_quoteSizes);
        _clientAcceptor->sendGridUpdate(message);
        proto::GridUpdate hedgeRatios;
        _guiThings.updateHedgeRatios(hedgeRatios, relativeValue->_hedgeRatios);
        _clientAcceptor->sendGridUpdate(hedgeRatios);
        proto::GridUpdate factorHedged;
        _guiThings.updateFactorHedged(factorHedged, relativeValue->_factorHedgedIndexes);
        _clientAcceptor->sendGridUpdate(factorHedged);
        proto::GridUpdate extraContractWidth;
        _guiThings.updateExtraContractWidth(extraContractWidth, relativeValue);
        _clientAcceptor->sendGridUpdate(extraContractWidth);
        proto::GridUpdate pctStfMove;
        _guiThings.updatePctStfMove(pctStfMove, relativeValue);
        _clientAcceptor->sendGridUpdate(pctStfMove);
        proto::GridUpdate maxTicksThroughMarket;
        _guiThings.updateMaxTicksThroughMarket(maxTicksThroughMarket, relativeValue);
        _clientAcceptor->sendGridUpdate(maxTicksThroughMarket);
    }

    void sendPlaySound(SoundId id) {
        _clientAcceptor->sendPlaySound(static_cast<uint32_t>(id));
    }

    void sendGuiInitialized() {
        sendGenericMessageToExecutor(ViperMessageType::GuiInitialized);
        for (auto strategy : _strategies) {
            strategy->sendGenericMessage(ViperMessageType::GuiInitialized);
        }
    }

    void disableAll_changeState(ViperStrategyBase* strategy, const std::string& message) {
        onStrategySystemStateChange(SystemState::Disabled, strategy->_state,
                                    s_systemState.strategyEnableBlocked(strategy->strategyId()),
                                    message, strategyFullName(strategy->strategyId()));
    }

    void sendDisableAllStrategies(const std::string& message) {
        for(auto* strategy : _strategies) {
            disableAll_changeState(strategy, message);
        }
    }

    void sendDisableAllStrategiesWithAnExclusion(StrategyId exception, const std::string& message) {
        for(auto* strategy : _strategies) {
            if(strategy->strategyId() != exception) {
                disableAll_changeState(strategy, message);
            }
        }
    }

    void sendStrategyDisableExternalQuoting() {
        for(auto* strategy : _strategies) {
            strategy->disableQuoteExternal();
        }
    }

    void sendFillToGUI(FillMessage& fill) {
        proto::GridUpdate message;
        auto relativeValue = reinterpret_cast<RelativeValue*>(_strategies[toUInt(StrategyId::RelativeValue)]);
        auto marketMaker = reinterpret_cast<MarketMaker*>(_strategies[toUInt(StrategyId::MarketMaker)]);
        _guiThings.populateFillMessage(fill, message, relativeValue, marketMaker);
        _clientAcceptor->sendGridUpdate(message);
    }

    void disableMMInViperConfig(int64_t internal_id) {
        BOOST_ASSERT(_internalIdToMarketMakerConfigOffset.count(internal_id) == 1);
        const auto& offset = _internalIdToMarketMakerConfigOffset.find(internal_id);
        if(offset != std::end(_internalIdToMarketMakerConfigOffset)) {
            _viperConfig._marketMakerConfigs[offset->second]._enabled = false;
        }
    }

    void disableUseGTCInViperConfig(int64_t internal_id) {
        BOOST_ASSERT(_internalIdToMarketMakerConfigOffset.count(internal_id) == 1);
        const auto& offset = _internalIdToMarketMakerConfigOffset.find(internal_id);
        if(offset != std::end(_internalIdToMarketMakerConfigOffset)) {
            _viperConfig._stackerConfigs[offset->second]._useGtc = false;
        }
    }

    void handleFill(FillMessage& fill) {
        auto realFill = fill._type != FillType::Phantom;
        if (BOOST_LIKELY(realFill)) {
            sendFillToGUI(fill);
            sendPlaySound(SoundId::Fill);
        }
        doPnl(fill);
        if (BOOST_LIKELY(realFill)) {
            updateConfigPosition(fill);
            LOG(0, "Fill. Strategy={i1} Type={i2} Contract={C[i3]} Side={i4} Price={d1} Size={i5} InternalAllocation={i6} ExternalAllocation={i7}", strategyName(fill._strategy),
                toUInt(fill._type), getContractName(fill._contractId), fill._side, ToDollars(fill._price), fill._qty,
                fill._internalAllocation, fill._externalAllocation)
        }
    }

    void updateConfigPosition(FillMessage& fill) {
        if (fill._strategy == StrategyId::MarketMaker) {
            int32_t multiplier = 1;
            if (fill._source != FillSource::Hedge && fill._side == ASK) {
                multiplier = -1;
            }
            _viperConfig.addPosition(fill._contractId, fill._internalAllocation * multiplier, fill._strategy, true);
            _viperConfig.addPosition(fill._contractId, fill._externalAllocation * multiplier, fill._strategy, false);
            _viperConfig.recordMMLastFillPrice(fill._contractId, fill._price);
        } else {
            hf::qty_t qty = fill._side == BID ? fill._qty : -fill._qty;
            _viperConfig.addPosition(fill._contractId, qty, fill._strategy, fill._type == FillType::Internal);
        }
        _viperConfig.writeToFile();
    }

    void doPnl(FillMessage& fill) {
        _pnlCalculator.onFill(fill);

        if (BOOST_UNLIKELY(checkAndActOnPnlForStrategy(fill._strategy, true, false, true))) {
            _pnlLimitsChecker.logState();
        }

        if (BOOST_LIKELY(fill._type != FillType::Phantom)) {
            sendPnlDataToGui(fill._contractId, fill._strategy);
        }
        uint64_t now = clockUtil::GetCycles();
        _negativeScalpChecker.expireEntries(now);
        _fillRateChecker.expireEntries(now);
        _pnlCalculatorForNegativeScalpChecker.onFill(fill);
        _fillRateChecker.onFill(now, fill._qty, fill._type, fill._source);
        std::string message;
        bool triggered = false;
        bool disableMM = false;
        if (_negativeScalpChecker.isOverLimit(message)) {
            LOG(0, "NegativeScalpChecker is over limit.  Disabling RV & MM, disable ALL external quoting {i1}", 0);
            triggered = disableMM = true;
        }
        else if(_fillRateChecker.isOverLimit()) {
            if(message.empty()) {
                message = std::string(
                        "FillRateChecker is over limit.  Disabling RV + RV/MM/Exchange ext quote. Window(ms)=") + std::to_string(GetMillisFromCycles(_fillRateChecker.throttleWindow(), _frequency)) +
                          " NumClips=" + std::to_string(_fillRateChecker.clipCount()) + "/" + std::to_string(_fillRateChecker.maxClips()) +
                          " SumTotalLimit=" + std::to_string(_fillRateChecker.sumTotal()) + "/" + std::to_string(_fillRateChecker.sumTotalLimit());
            }
            LOG(0, "FillRateChecker is over limit.  Disabling RV, sending signal to Executor to disable External Quoting NumClips={i1}/{i2} SumTotal={i3}/{i4}", _fillRateChecker.clipCount(),
                    _fillRateChecker.maxClips(), _fillRateChecker.sumTotal(), _fillRateChecker.sumTotalLimit());
            triggered = true;
        }

        if(triggered) {
            if(disableMM) {
                sendDisableAllStrategies(message);
            }
            else {
                sendDisableAllStrategiesWithAnExclusion(StrategyId::MarketMaker, message);
            }
            sendStrategyDisableExternalQuoting();
            DisableExternalQuotingOnAllMMInstrumentsKernel(s_systemState);
        }
    }

    void sendAggregatePnLDataToGui(StrategyId strategyId) {
        sendPnlToGui(strategyId, _pnlCalculator.getAggregatedPnl(strategyId), _pnlCalculator._pnlTotal);
    }

    void sendPnlDataToGui(uint32_t contractId, StrategyId strategyId) {
        sendAggregatePnLDataToGui(strategyId);
        sendPnlDetailToGui(_pnlCalculator.getPnlDetail(contractId, StrategyId::RelativeValue), _pnlCalculator.getPnlDetail(contractId, StrategyId::MarketMaker), strategyId, contractId);
    }

    void sendPnlToGui(StrategyId strategy, const Pnl& pnl, const Pnl& totalPnl) {
        proto::GridUpdate pnlMessage;
        _guiThings.populatePnlUpdate(pnlMessage, strategy, pnl, totalPnl);
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
        _guiThings.populateMessageCountsUpdate(message, _executor);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendErrorMessage(ErrorMessage& m) {
        proto::GridUpdate message;
        _guiThings.populateErrorMessage(message, m);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendErrorMessage(const std::string& message, ErrorType type) {
        BOOST_ASSERT(message.size() < CRITICAL_MESSAGE_MAX_SIZE);
        ErrorMessage em;
        auto strlen = std::min<decltype(CRITICAL_MESSAGE_MAX_SIZE)>(message.size(), CRITICAL_MESSAGE_MAX_SIZE);
        memcpy(em._message, message.c_str(), strlen);
        em._size = strlen;
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
                    _pnlLimitsChecker.handlePnlLimitChange(gridUpdate);
                    onGridUpdateAccepted(toUInt(WindowIds::PNL));
                    checkAndActOnPnl(true);
                } else if (windowId == WindowIds::GlobalConfigs) {
                    handleGlobalConfigChange(gridUpdate);
                } else if (windowId == WindowIds::ManualFillsEntry) {
                    for (auto strategy : _strategies) {
                        strategy->sendGridUpdate(gridUpdate);
                    }
                }
                else if(windowId == WindowIds::GTCManagement || windowId == WindowIds::StackerParameters ||
                        windowId == WindowIds::TraderSwitch) {
                    sendGridUpdateToExecutor(gridUpdate);
                }
                else if(windowId == WindowIds::StatusDebug) {
                    sendStatusDebugUpdates(gridUpdate);
                    onGridUpdateAccepted(gridUpdate._update.gridid());
                }
                else {
                    _strategies[toUInt(getUpdateStrategy(windowId))]->sendGridUpdate(gridUpdate);
                }
            } else {
                logging::GetLogger().logError("received grid update but another already in flight");
            }
        } else {
            logging::GetLogger().logError("received grid update for unknown grid " + std::to_string(update.gridid()));
        }
    }

    void handleGlobalConfigChange(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _viperConfig._globalConfigs[gridCell.row()];
            if (config._key == NEGATIVE_SCALP_CLIPS) {
                uint32_t clips = boost::lexical_cast<uint32_t>(gridCell.value());
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                if (_viperConfig._currentShift == shift) {
                    _negativeScalpChecker.updateCountLimit(clips);
                }
                LOG(0, "Negative Scalper Checkers. Updated count limit. Value={i1} Shift={i2}", clips, toUInt(shift))
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if (config._key == NEGATIVE_SCALP_DOLLARS) {
                uint32_t value = boost::lexical_cast<uint32_t>(gridCell.value());
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                if (_viperConfig._currentShift == shift) {
                    _negativeScalpChecker.updatePnlLimit(ToRhonePrice(value));
                }
                LOG(0, "Negative Scalper Checkers. Updated dollar limit. Value={i1} Shift={i2}", value, toUInt(shift))
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if (config._key == NEGATIVE_SCALP_PERIOD) {
                uint32_t period = boost::lexical_cast<uint32_t>(gridCell.value());
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                if (_viperConfig._currentShift == shift) {
                    uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                    _negativeScalpChecker.updatePeriodInCycles(periodInCycles);
                }
                LOG(0, "Negative Scalper Checkers. Updated period. Period={i1} Shift={i2}", period, toUInt(shift))
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if(config._key == FILL_RATE_CLIPS) {
                uint32_t clips = boost::lexical_cast<uint32_t>(gridCell.value());
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                if (_viperConfig._currentShift == shift) {
                    _fillRateChecker.changeMaxClips(clips, clockUtil::GetCycles());
                }
                LOG(0, "Fill Rate Checker.  Update max clips={i1} Shift={i1}", clips, toUInt(shift));
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if(config._key == FILL_RATE_SUM_TOTAL) {
                hf::qty_t fillRateSumTotal = boost::lexical_cast<hf::qty_t>(gridCell.value());
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                if (_viperConfig._currentShift == shift) {
                    _fillRateChecker.changeSumTotal(fillRateSumTotal, clockUtil::GetCycles());
                }
                LOG(0, "Fill Rate Checker.  Update sumTotal={i1} Shift={i2}", fillRateSumTotal, toUInt(shift));
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if(config._key == FILL_RATE_PERIOD) {
                uint32_t period = boost::lexical_cast<uint32_t>(gridCell.value());
                uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                auto shift = static_cast<Shifts>(gridCell.col() - 1);
                if (_viperConfig._currentShift == shift) {
                    _fillRateChecker.changeThrottleWindowSize(periodInCycles, clockUtil::GetCycles());
                }
                LOG(0, "Fill Rate Checker.  Updated window size={i1}ms ({i2} cycles) Shift={i3}", period, periodInCycles, toUInt(shift));
                onGridUpdateAccepted(toUInt(WindowIds::GlobalConfigs));
            }
            else if (config._key == RV_WIDTH_MULTIPLIER) {
                sendGridUpdateToRv(update);
            }
            else if (config._key == QUOTE_BANNER_PERIOD || config._key == FILL_BANNER_PERIOD ||
                     config._key == HEDGE_BANNER_MICS || config._key == PERCENT_MSG_HEDGES ||
                     config._key == PERCENT_MSG_REDUCE_QUOTE || config._key == MSG_HEDGES_INTERVAL_MICS ||
                     config._key == MSG_REDUCE_QUOTE_MICS || config._key == SELF_MATCH_BANNER_MICS ||
                     config._key == ORDER_RATE_CLIPS || config._key == ORDER_RATE_SUM_TOTAL || config._key == ORDER_RATE_PERIOD) {
                sendGridUpdateToExecutor(update);
            }
        }
    }

    StrategyId getUpdateStrategy(WindowIds id) {
        return (id == WindowIds::MarketMakerParameters) ? StrategyId::MarketMaker : StrategyId::RelativeValue;
    }

    void sendGridUpdateToRv(GridUpdate& update) {
        _strategies[toUInt(StrategyId::RelativeValue)]->sendMessage<GridUpdateMessage>(
                ViperMessageType::GridUpdate,
                [&](GridUpdateMessage& message) {
                    message._gridUpdate = &update;
                });
    }

    void sendGridUpdateToMM(GridUpdate& update) {
        _strategies[toUInt(StrategyId::MarketMaker)]->sendMessage<GridUpdateMessage>(
                ViperMessageType::GridUpdate,
                [&](GridUpdateMessage& message) {
                    message._gridUpdate = &update;
                });
    }

    void sendGridUpdateToExecutor(GridUpdate& update) {
        sendMessageToExecutor<GridUpdateMessage>(
                ViperMessageType::GridUpdate,
                [&](GridUpdateMessage& message) {
                    message._gridUpdate = &update;
                });
    }

    void sendChangeToAudit(uint32_t gridId, GridUpdate& gu) {
        proto::GridUpdate message;
        _guiThings.populateAuditMessage(gridId, message, gu, _viperConfig._currentTrader);
        _clientAcceptor->sendGridUpdate(message);
    }

    void sendManualFillToAudit(uint32_t contractId, hf::qty_t size, StrategyId strategy) {
        proto::GridUpdate message;
        _guiThings.populateManualFilAuditMessage(message, contractId, size, _viperConfig._currentTrader, strategy);
        _clientAcceptor->sendGridUpdate(message);
    }

    void onGridUpdateAccepted(uint32_t gridId) {
        //Note that even if the update was rejected, there is zero checking here.  We rely on the GridUpdate itself being modified
        //to what is going to be persisted
        auto it = _gridUpdates.find(gridId);
        if (it != _gridUpdates.end()) {
            GridUpdate& gu = it->second;
            _viperConfig.applyGridUpdate(gu);
            if (gridId != toUInt(WindowIds::ManualFillsEntry)) {
                sendChangeToAudit(gridId, gu);
                if (gridId == toUInt(WindowIds::TraderSwitch)) {
                    onTraderSwitchUpdate(gu);
                } else if (gridId == toUInt(WindowIds::GlobalConfigs)) {
                    sendGlobalConfigValueUpdateToGUI(gu);
                }
            }
            gu.clear();
            if (gridId == toUInt(WindowIds::ManualFillsEntry)) {
                proto::GridUpdate update;
                update.set_updatetype(proto::GridUpdateType::CellsType);
                update.set_gridid(gridId);
                update.set_messageid(0);
                _clientAcceptor->sendGridChangeReply(update);
            } else {
                _clientAcceptor->sendGridChangeReply(gu._update);
            }
        } else {
            logging::GetLogger().logError("Accepted grid update which cannot be found");
        }
    }

    void onTraderSwitchUpdate(GridUpdate& update) {
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            TraderSwitchColumns columnType = static_cast<TraderSwitchColumns>(gridCell.col());
            if (columnType == TraderSwitchColumns::Shift) {
                auto shift = atoi(gridCell.value().c_str());
                switchGlobalConfigAndSendUpdateToGUI(shift);
                break;
            }
        }
    }

    void sendMMWidthPercentageUpdateToGUI(uint32_t shift) {
        GridUpdate gu;
        gu._update.set_gridid(toUInt(WindowIds::MarketMakerParameters));
        gu._update.set_updatetype(proto::GridUpdateType::CellsType);
        auto cells = gu._update.mutable_cells();

        for (uint32_t j = 0; j < _viperConfig._marketMakerConfigs.size(); ++j) {
            auto& config = _viperConfig._marketMakerConfigs[j];
            proto::GridCell cell;
            cell.set_row(j + _viperConfig._indexOfFirstTradedContract);
            cell.set_col(toUInt(MarketMakerParametersColumns::Width));
            cell.set_value(std::to_string(config._widthPercentage[toUInt(Shifts::__current_marker__)]));
            gu._oldValues.emplace_back(std::move(cell));
            config._widthPercentage[toUInt(Shifts::__current_marker__)] = config._widthPercentage[shift];
            _guiThings.setCell(cells, j + _viperConfig._indexOfFirstTradedContract, MarketMakerParametersColumns::Width, std::to_string(config._widthPercentage[shift]));
        }

        sendChangeToAudit(toUInt(WindowIds::MarketMakerParameters), gu);

        gu._update.set_messageid(proto::GUI_MESSAGE_TYPE_GRID_CHANGE);
        _clientAcceptor->sendGridChangeReply(gu._update);
    }

    void switchGlobalConfigAndSendUpdateToGUI(uint32_t shift) {
        GridUpdate gu;
        gu._update.set_gridid(toUInt(WindowIds::GlobalConfigs));
        gu._update.set_updatetype(proto::GridUpdateType::CellsType);
        uint32_t i = 0;
        for (auto& config : _viperConfig._globalConfigs) {
            config._value = config._shifts[shift];
            if (config._key == NEGATIVE_SCALP_CLIPS) {
                uint32_t clips = boost::lexical_cast<uint32_t>(config._value);
                _negativeScalpChecker.updateCountLimit(clips);
                LOG(0, "Negative Scalper Checkers. Updated count limit. Value={i1} Shift={i2}", clips, toUInt(shift))
            }
            else if (config._key == NEGATIVE_SCALP_DOLLARS) {
                uint32_t value = boost::lexical_cast<uint32_t>(config._value);
                _negativeScalpChecker.updatePnlLimit(ToRhonePrice(value));
                LOG(0, "Negative Scalper Checkers. Updated dollar limit. Value={i1} Shift={i2}", value, toUInt(shift))
            }
            else if (config._key == NEGATIVE_SCALP_PERIOD) {
                uint32_t period = boost::lexical_cast<uint32_t>(config._value);
                uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                _negativeScalpChecker.updatePeriodInCycles(periodInCycles);
                LOG(0, "Negative Scalper Checkers. Updated period. Period={i1} Shift={i2}", period, toUInt(shift))
            }
            else if(config._key == FILL_RATE_CLIPS) {
                uint32_t clips = boost::lexical_cast<uint32_t>(config._value);
                _fillRateChecker.changeMaxClips(clips, clockUtil::GetCycles());
                LOG(0, "Fill Rate Checker.  Update max clips={i1} Shift={i1}", clips, toUInt(shift));
            }
            else if(config._key == FILL_RATE_SUM_TOTAL) {
                hf::qty_t fillRateSumTotal = boost::lexical_cast<hf::qty_t>(config._value);
                _fillRateChecker.changeSumTotal(fillRateSumTotal, clockUtil::GetCycles());
                LOG(0, "Fill Rate Checker.  Update sumTotal={i1} Shift={i2}", fillRateSumTotal, toUInt(shift));
            }
            else if(config._key == FILL_RATE_PERIOD) {
                uint32_t period = boost::lexical_cast<uint32_t>(config._value);
                uint64_t periodInCycles = GetCyclesFromMilliSeconds(period, _frequency);
                _fillRateChecker.changeThrottleWindowSize(periodInCycles, clockUtil::GetCycles());
                LOG(0, "Fill Rate Checker.  Updated window size={i1}ms ({i2} cycles) Shift={i3}", period, periodInCycles, toUInt(shift));
            }

            auto cell = gu._update.add_cells();
            cell->set_row(i++);
            cell->set_col(0);
            cell->set_value(config._value);
        }
        sendChangeToAudit(toUInt(WindowIds::MarketMakerParameters), gu);
        gu._update.set_messageid(proto::GUI_MESSAGE_TYPE_GRID_CHANGE);
        _clientAcceptor->sendGridChangeReply(gu._update);
    }

    void sendGlobalConfigValueUpdateToGUI(GridUpdate& update) {
        // can modify update directly, it might be accessed by other components
        GridUpdate gu;
        gu._update.set_gridid(toUInt(WindowIds::GlobalConfigs));
        gu._update.set_updatetype(proto::GridUpdateType::CellsType);

        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            auto& config = _viperConfig._globalConfigs[gridCell.row()];
            auto shift = static_cast<Shifts>(gridCell.col() - 1);
            if (_viperConfig._currentShift == shift) {
                auto cell = gu._update.add_cells();
                cell->set_row(gridCell.row());
                cell->set_col(0);
                cell->set_value(config._value);
            }
        }
        gu._update.set_messageid(proto::GUI_MESSAGE_TYPE_GRID_CHANGE);
        _clientAcceptor->sendGridChangeReply(gu._update);
    }

    void onOtherRequest(ViperMessageType mt)  {
        switch (mt) {
            case ViperMessageType::Login: {
                onSystemStateChange(SystemState::LoggedIn, "GUI Requested");
                proto::GUIInitialization m;
                _guiThings.populateGUIInitialization(m, _strategies, _executor);
                _clientAcceptor->handleLoginAccepted(m, GetMillisFromCycles(_guiHeartbeatInterval, _frequency));
                _loginTime = clockUtil::GetCycles();

                // set shift dependent values correctly
                auto shift = toUInt(_viperConfig._currentShift);
                sendGenericMessageToExecutor(ViperMessageType::ShiftUpdate, shift);
                switchGlobalConfigAndSendUpdateToGUI(shift);
                break;
            }
            case ViperMessageType::Logout: {
                onSystemStateChange(SystemState::LoggedOut, "GUI Requested");
                break;
            }
            case ViperMessageType::ClientDown: {
                logging::GetLogger().logError("Client down - doing hard shutdown");
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
        logging::GetLogger().logErrorAndThrow("GUI initialization failed;");
    }

    void onSystemStateChange(SystemState newState, const std::string& reason) {
        for (auto strategy : _strategies) {
            if (newState == SystemState::CheckQuotes) {
                if (strategy->strategyId() == StrategyId::RelativeValue) {
                    SystemState rvState = strategy->state();
                    if (rvState == SystemState::Disabled || rvState == SystemState::LoggedIn || rvState == SystemState::CancelGtcAndDisable) {
                        onStrategySystemStateChange(newState, strategy->_state,
                                                    s_systemState.strategyEnableBlocked(strategy->strategyId()), reason,
                                                    toString(strategy->strategyId()));
                    }
                }
            } else {
                onStrategySystemStateChange(newState, strategy->_state, s_systemState.strategyEnableBlocked(strategy->strategyId()), reason, toString(strategy->strategyId()));
            }
        }
    }

    void onStrategySystemStateChange(SystemState newState, std::atomic<SystemState>& currentState, bool gated, const std::string& reason, const std::string& strategy) {
        if (currentState.load(std::memory_order_relaxed) == SystemState::HardShutdown) {
            hf::Service::exitAll();
        }
        else if(newState == SystemState::Enabled && gated) {
            std::string message = boost::str(boost::format("Refusing to change state -- state is gated shut.  Requested State Change: Strategy=%1% State=%2% Reason=%3%") % strategy % GetSystemStateString(newState) % reason);
            logging::GetLogger().logInfo(message);
            sendErrorMessage(message, ErrorType::Critical);
        }
        else {
            if (newState != currentState.load(std::memory_order_relaxed)) {
                std::string message = boost::str(boost::format("Request State Change: Strategy=%1% State=%2% Reason=%3%") % strategy % GetSystemStateString(newState) % reason);
                logging::GetLogger().logInfo(message);
                sendErrorMessage(message, ErrorType::Critical);
                currentState.store(newState, std::memory_order_release);
            }
        }
    }

    void onSwitchChange(proto::SwitchChange& change) {
        auto& cell = change.cell();

        MainControlAction action = static_cast<MainControlAction>(cell.col());

        if (action != MainControlAction::Enabled) {
            return;
        }

        SystemState state = ToSystemStateFromProto((proto::SystemState)atoi(cell.value().c_str()));
        auto row = cell.row();
        //The intent here is:
        // Strategy : MM can always enable.  Other strategies can only enable if not in breech of strategy/total PnL
        // Strategy QuoteExternal : Can only enable if not in breach of strategy/total PnL
        // Exchange QuoteExternal : Allow if total pnl is not breached
        if (row < toUInt(StrategyId::Execution)) {
            auto strategy = _strategies[row];
            std::string message;
            if(state == SystemState::Enabled && strategy->strategyId() != StrategyId::MarketMaker && checkAndActOnPnlForStrategy(strategy->strategyId(), false, false, true)) {
                sendErrorMessage(std::string("Refusing to enable Strategy=") + toString(strategy->strategyId()) + " -- pnl currently breached", ErrorType::Critical);
            }
            else {
                onStrategySystemStateChange(state, strategy->_state, s_systemState.strategyEnableBlocked(strategy->strategyId()),"GUI requested",toString(strategy->strategyId()));
            }
        } else if (row < toUInt(StrategyId::Execution) * 2) {
            auto strategy = _strategies[row - toUInt(StrategyId::Execution)];
            std::string message;
            if(state == SystemState::Enabled && checkAndActOnPnlForStrategy(strategy->strategyId(), false, false, true)) {
                message = std::string("Refusing to enable QuoteExternal for Strategy=") + toString(strategy->strategyId()) + " -- pnl currently breached";
                logging::GetLogger().logInfo(message);
            }
            else {
                message = boost::str(boost::format("Request %1% QuoteExternal Change: State=%2% Reason=requested") % toString(strategy->strategyId()) % GetSystemStateString(state));
                logging::GetLogger().logInfo(message);
                strategy->_quoteExternal.store(state, std::memory_order_release);
            }
            sendErrorMessage(message, ErrorType::Critical);
        } else {
            auto venue = _guiThings.getVenueFromMainControlRowIndex(cell.row());
            if(state == SystemState::Enabled && _pnlLimitsChecker.checkTotalLimits()) {
                auto message = std::string("Refusing to enable External Quote on venue=") + hf::VENUE_NAMES.at(venue) + " due to Total PnL breech";
                logging::GetLogger().logInfo(message);
                sendErrorMessage(message, ErrorType::Critical);
            }
            else {
                onExchangeQuoteExtChange(state, venue);
            }
        }
    }

    void onExchangeQuoteExtChange(SystemState newState, uint8_t venue) {
        if (!venue) {
            return;
        }

        bool inMDGap = s_systemState._inMDGap.test(static_cast<hf::VENUE>(venue));
        boost::format fmt;

        if((!inMDGap) || newState != SystemState::Enabled) {
            changeExchangeExternalQuoting(static_cast<hf::VENUE>(venue), newState);
            fmt = boost::format("Request ExchangeQuoteExternal Change: Venue=%1% State=%2% Reason=requested");
        }
        else {
            fmt = boost::format{"Request QuoteExternal Change REJECTED.  MD Gap. Venue=%1% State=%2% Reason=requested"};
        }
        std::string message = boost::str(fmt % ven::VenueUtils::getName(static_cast<hf::VENUE>(venue)) % GetSystemStateString(newState));
        logging::GetLogger().logInfo(message);
        sendErrorMessage(message, ErrorType::Critical);
    }


    COLD void sendStatusDebugUpdates(const GridUpdate& update);

    void sendEMAOnUpdate(uint32_t vectorId, bool isOn) {
        GridUpdate gu;
        gu._update.set_updatetype(proto::GridUpdateType::CellsType);
        gu._update.set_gridid(toUInt(WindowIds::VectorParameters));
        auto cells = gu._update.mutable_cells();
        auto cell = cells->Add();
        cell->set_row(vectorId);
        cell->set_col(toUInt(VectorParametersColumns::EMA_ON));
        cell->set_value(std::to_string(!isOn)); // the old value
        gu._oldValues.push_back(*cell);
        cell->set_value(std::to_string(isOn));
        sendChangeToAudit(toUInt(WindowIds::VectorParameters), gu);
        _clientAcceptor->sendGridUpdate(gu._update);
    }


#ifdef IS_DEV
        std::string getContractName(uint32_t id) {
            return _viperConfig._allContracts[id]._name;
        }
#else
        uint32_t getContractName(uint32_t id) {
            return id;
        }
#endif

    private:
    ClientsAcceptor* _clientAcceptor;
    std::unordered_map<uint32_t, GridUpdate> _gridUpdates;
    md::sym::SymbolsResource* _symbolsResource;

    Executor* _executor;

    GUIThings& _guiThings;

    PnlCalculator _pnlCalculator;
    PnlLimitsChecker _pnlLimitsChecker;

    PnlCalculatorsForNegativeScalpChecker _pnlCalculatorForNegativeScalpChecker;
    NegativeScalpChecker _negativeScalpChecker;
    FillRateChecker<> _fillRateChecker;

    uint64_t _nextRefreshTime = 0;
    uint64_t _refreshInterval;

    uint64_t _nextChartRefreshTime = 0;
    uint64_t _chartRefreshInterval;

    uint64_t _nextLtfStoreTime = 0;
    uint64_t _ltfStoreInterval;

    std::vector<uint64_t> _internalHeartbeatTimestamps;
    uint64_t _maxInternalHeartbeatTime;

    static constexpr int DEFAULT_GUI_HEARTBEAT_INTERVAL_IN_MS = 900;
    uint64_t _guiHeartbeatInterval;
    uint64_t _guiHeartbeatTime = 0;

    uint64_t _loginTime = 0;
    uint64_t _versionSendingTime;

    bool _canSendPnl = false;

    std::unordered_map<uint32_t, uint32_t> _internalIdToMarketMakerConfigOffset;
};

}

#endif //VIPER_FRONT_SIDE_H

