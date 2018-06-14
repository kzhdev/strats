#ifndef VIPER_EXECUTOR_H
#define VIPER_EXECUTOR_H

#include <hf/ConfigManager.h>
#include <hf/Service.h>
#include <stdint.h>
#include <hf/hf.h>
#include <md/md_event.h>
#include <om/om_event.h>
#include <strat/Strategy.hpp>
#include "SimpleMarketMaker.h"
#include "Common.h"
#include "RelativeValue.h"
#include "ViperConfig.h"
#include "ExecutorForOneContract.h"
#include <md/queue/MDQueue.h>
#include "Timer.h"
#include <hf/queue/DebugQueueLogger.h>
#include "Throttler.h"

namespace Python {

template <typename T>
struct ContractState {
    ExecutorForOneContract<T> _contractExecutor;
    SimpleMarketMaker<ExecutorForOneContract<T>> _marketMaker;
    Book _books[2];
};

struct GatewayStatus {
    std::string _message;
    bool _active = true;
};

template<typename StrategyT>
class Executor : public StrategyT {

public:
    typedef ContractState<Executor> ContractStateT;

    Executor()
    :
        _fillBannerTimer(*this),
        _quoteBannerTimer(*this),
        _checkQuotesTimer(*this)
    {
        _manualFills.resize(5);
    }

    void onStratBegin() override {
        resetQueues();
        initialize(*_config);
        if (!this->context().strategyLogin(_strategyName, _strategyVersion)) {
            logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Strategy login did not work. Name=%1% Version=%2%") % _strategyName % _strategyVersion));
        }
        LOG(0, "Executor: Started {i1}", 0)
    }

    void resetQueues() {
        _inFromFront->resetR();
        _inFromRV->resetR();
        _outToRV->resetW();
        _outToFront->resetW();
    }

    void onStratEnd() override {
        LOG(0, "Executor: Ended. {i1}", 0)
    }

#ifdef IS_DEV
    std::string getContract(uint32_t id) {
        return getContractName(id);
    }
#else
    uint32_t getContract(uint32_t id) {
        return id;
    }
#endif

    void onPostCreate(const libconfig::Setting& config) override {
        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_to_front");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _outToFront = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_outToFront) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_from_front");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _inFromFront = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_inFromFront) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_to_relative_value");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _outToRV = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_outToRV) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        {
            const auto queueName = hf::ConfigManager::getString(config, "queue_from_relative_value");
            auto *const ptr = hf::ResourceManager::getResourceP(queueName);
            if (!ptr) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
            _inFromRV = dynamic_cast<md::queue::MDQueueResource *> (ptr);
            if (!_inFromRV) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Cannot Find Queue Resource by name '%1%'") % queueName));
            }
        }

        _strategyName = hf::ConfigManager::getString(config, "strategy_name");
        _strategyVersion = hf::ConfigManager::getString(config, "strategy_version");

        _inFromRV->resetR();
        _inFromFront->resetR();
        _outToRV->resetW();
        _outToFront->resetW();

        const std::string loggerName = config["kdb_logger"];
        _kdbLogger = &(dynamic_cast<hf::DebugQueueLogger*> (hf::ResourceManager::getResourceP(loggerName)))->queue();

        _nextInternalHeartbeat = 0;
    }

    virtual void onStratLoop() override {
        if (BOOST_UNLIKELY(_needToSetLogger)) {
            _needToSetLogger = false;
            _kdbLogger->resetW();
            logging::setKdbLogger(_kdbLogger);
            logging::GetLogger().logInfo(boost::str(boost::format("EX logger %1%") % _kdbLogger));
        }
        processMessagesFromRV();
        processMessagesFromFront();
        checkSystemState();
        uint64_t now = clockUtil::GetCycles();
        checkTimers(now);
        sendInternalHeartbeat(now);
    }

    void sendInternalHeartbeat(uint64_t now) {
        if (BOOST_UNLIKELY(now > _nextInternalHeartbeat)) {
            _nextInternalHeartbeat = now + _internalHeartbeatPeriod;
            sendGenericMessageToFront(ViperMessageType::InternalHeartbeat);
        }
    }

    void processMessagesFromRV() {
        while (true) {
            uint64_t idx = _inFromRV->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromRV->freeNext();

            auto& message = *static_cast<ViperMessage*>(_inFromRV->getPoolP()->getData(idx));

            switch (message._type) {
                case ViperMessageType::QuoteOrder: {
                    QuoteOrderMessage& order = reinterpret_cast<QuoteOrderMessage &> (message._data[0]);
                    onQuoteOrderFromRv(order);
                    break;
                }
                case ViperMessageType::HedgeOrder: {
                    HedgeOrderMessage& order = reinterpret_cast<HedgeOrderMessage &> (message._data[0]);
                    onHedgeOrderFromRv(order);
                    break;
                }
                case ViperMessageType::Fill: {
                    FillMessage& fill = reinterpret_cast<FillMessage &> (message._data[0]);
                    handleReturnedFillFromRelativeValue(fill);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void handleReturnedFillFromRelativeValue(FillMessage& fill) {
        LOG(0, "Executor: Handling fill returned from RV. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(fill._contractId), fill._side, ToDollars(fill._price), fill._qty)
        ContractStateT& state = _contractStates[fill._contractId];
        state._marketMaker.forceTakeOverFill(fill._side, fill._qty);
        if (!state._marketMaker.isWithinPositionLimits()) {
            state._marketMaker.disable();
        }
    }

    void onQuoteOrderFromRv(QuoteOrderMessage& order) {
        ContractStateT& state = _contractStates[order._instrumentIndex];
        state._contractExecutor.onQuoteOrderFromRelativeValue(order);
    }

    void sendRelativeValueHedgeInternalFillToFront(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        sendFillToFront(internalId, side, price, size, StrategyId::RelativeValue, FillType::Internal, FillSource::Hedge);
    }

    void onInternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool payUp = false) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::Internal, FillSource::Quote);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.onInternalFill(order, price, size, state._books[BID], state._books[ASK], payUp);
                sendFillToFront(internalId, order->_side, price, size, StrategyId::MarketMaker, FillType::Internal, FillSource::Quote);
                break;
            }
            default:
                break;
        }
    }

    void onExternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size, bool payUp = false) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                LOG(0, "Executor: External fill. Sending to RV. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), order->_side, ToDollars(price), size)
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::External, FillSource::Quote);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.onExternalFill(order, price, size, state._books[BID], state._books[ASK], payUp);
                FillSource fillSource = order->_type == Order::Type::Hedging ? FillSource::Hedge : FillSource::Quote;
                sendFillToFront(internalId, order->_side, price, size, StrategyId::MarketMaker, FillType::External, fillSource);
                break;
            }
            default:
                break;
        }
    }

    void onExternalOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
       LOG(0, "Executor: External overfill. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), side, ToDollars(price), size)
        ContractStateT& state = _contractStates[internalId];
        hf::qty_t remainingSize = size;
        if (state._marketMaker.isEnabled()) {
            hf::qty_t takenSize = state._marketMaker.takeOverFill(side, remainingSize);
            if (takenSize > 0) {
                sendFillToFront(internalId, side, price, takenSize, StrategyId::MarketMaker, FillType::External, FillSource::Overfill);
            }
            remainingSize -= takenSize;
        }
        if (remainingSize > 0) {
            if (_systemState->_relativeValue == SystemState::Enabled) {
                sendFillToRV(internalId, side, price, remainingSize, StrategyId::RelativeValue, FillType::External, FillSource::Overfill);
            } else {
                state._marketMaker.forceTakeOverFill(side, remainingSize);
                sendFillToFront(internalId, side, price, remainingSize, StrategyId::MarketMaker, FillType::External, FillSource::Overfill);
                state._marketMaker.disable();
            }
        }
    }

    void sendFillToFront(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type, FillSource source) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractId = contractIndex;
        message._side = side;
        message._qty = size;
        message._strategy = strategyId;
        message._type = type;
        message._price = price;
        message._source = source;
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void onEndOfExternalFills(uint32_t internalId, hf::SIDE side) {
        ContractStateT& state = _contractStates[internalId];
        uint32_t s = ToViperSide(side);
        state._marketMaker.onEndOfExternalFills(s);
    }

    void onEndOInternalFills(uint32_t internalId) {
        ContractStateT& state = _contractStates[internalId];
        state._marketMaker.onEndOInternalFills();
    }

    void onHedgeOrderFromRv(HedgeOrderMessage& order) {
       LOG(0, "MM received hedge order from RV. Contract={C[i1]} Side={i2} Price={d1} Size={i3} HedgeId={i4}", getLoggingContract(order._instrumentIndex), order._side, ToDollars(order._price), order._size, order._hedgeId)
        ContractStateT& state = _contractStates[order._instrumentIndex];
        const hf::qty_t unmatchedSize = state._contractExecutor.onHedgeOrderFromRelativeValue(order);
        if ((unmatchedSize > 0) & (!order._isExternal)) {
            LOG(0, "Missed internal only hedge. Back to RV. Contract={C[i1]} Unfilled={i2}",getLoggingContract(order._instrumentIndex), unmatchedSize)
            const uint32_t otherSide = GetOtherSide(order._side);
            sendFillToRV(order._instrumentIndex, otherSide, order._price, unmatchedSize, StrategyId::RelativeValue, FillType::Internal, FillSource::MissedHedge);
        }
        state._contractExecutor.finishAfterHedgeOrderFromRv();
    }

    std::string& getContractName(uint32_t id) {
        return _config->_allContracts[id]._name;
    }

#ifdef IS_DEV
    std::string getLoggingContract(uint32_t id) {
        return _config->_allContracts[id]._name;
    }
#else
    uint32_t getLoggingContract(uint32_t id) {
            return _config->_allContracts[id]._internalId;
        }
#endif

    void sendExternalRvHedge(HedgeOrderMessage& order) {
        om::order_t* rhoneOrder = newFAKOrder(order._instrumentIndex, order._side, order._price, order._size);
        sendHedgeMarketDataAdjustmentAck(order._hedgeId, order._instrumentIndex, GetOtherSide(order._side), rhoneOrder->oid, order._size);
        sendFillToFront(order._instrumentIndex, order._side, order._price, order._size, StrategyId::RelativeValue, FillType::External, FillSource::Hedge);
    }

    void sendHedgeMarketDataAdjustmentAckWithZeroSize(HedgeOrderMessage& order) {
        sendHedgeMarketDataAdjustmentAck(order._hedgeId, order._instrumentIndex, GetOtherSide(order._side), 0, 0);
    }

    void onInternalHedgeDone(uint32_t instrumentId, uint32_t marketId, uint32_t side, uint32_t hedgeId) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::InternalHedgeDone;
        InternalHedgeDone& message = reinterpret_cast<InternalHedgeDone &> (outMessage._data[0]);
        message._hedgeId = hedgeId;
        message._instrumentIndex = instrumentId;
        message._side = side;
        message._marketIndex = marketId;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(InternalHedgeDone));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendHedgeMarketDataAdjustmentAck(uint32_t hedgeId, uint32_t instrumentIndex, uint32_t side, om::order_id_t orderId, hf::qty_t size) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::HedgeMDAdjsutAck;
        HedgeMarketDataAdjustmentAck& message = reinterpret_cast<HedgeMarketDataAdjustmentAck &> (outMessage._data[0]);
        message._hedgeId = hedgeId;
        message._instrumentIndex = instrumentIndex;
        message._side = side;
        message._size = size;
        message._orderId = orderId;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(HedgeMarketDataAdjustmentAck));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendFillToRV(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type, FillSource source) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractId = contractIndex;
        message._side = side;
        message._qty = size;
        message._strategy = strategyId;
        message._type = type;
        message._price = price;
        message._source = source;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    ExecutorForOneContract<Executor>& getContractExecutor(const om::order_t& order) {
        ContractStateT& state = _contractStates[getInternalId(order)];
        return state._contractExecutor;
    }

    void sendReverseFillToRelativeValue(uint32_t internalId, const om::order_t& order) {
        const hf::qty_t unfilledSize = order.qty - order.executedQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        LOG(0, "Sending unfilled size as reverse to RV. Contract={C[i1]} OID={i2}", getLoggingContract(internalId), order.oid)
        sendFillToRV(internalId, reverseSide, order.pr, unfilledSize, StrategyId::RelativeValue, FillType::External, FillSource::MissedHedge);
    }

    void processMessagesFromFront() {
        while (true) {
            uint64_t idx = _inFromFront->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromFront->freeNext();

            auto& message = *static_cast<ViperMessage*>(_inFromFront->getPoolP()->getData(idx));

            switch (message._type) {
                case ViperMessageType::GridUpdate: {
                    GridUpdateMessage& gridChange = reinterpret_cast<GridUpdateMessage &> (message._data[0]);
                    bool accepted = processGridChange(*gridChange._gridUpdate);
                    sendGenericMessageToFront(ViperMessageType::GridUpdateAccepted, gridChange._gridUpdate->_update.gridid());
                    break;
                }
                case ViperMessageType::GuiInitialized: {
                    handleGuiInitialized();
                    break;
                }
                case ViperMessageType::Quoting: {
                    GenericMessage& gm = reinterpret_cast<GenericMessage &> (message._data[0]);
                    SystemState state = (SystemState) gm._number_1;
                    const bool canMarketMakerInitiateExternally = state == SystemState::Enabled;
                    updateCanMarketMakerInitiateExternally(canMarketMakerInitiateExternally);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void updateCanMarketMakerInitiateExternally(bool can) {
        LOG(0, "Market makers quoting externally changed. Value={i1}", 0);
        _canMarketMakersQuoteExternally = can;
        for (auto& state : _contractStates) {
            state._marketMaker.onGlobalQuoteExternallyChanged();
        }
        proto::SystemState state = can ? proto::SystemState::SystemStateEnabled : proto::SystemState::SystemStateDisabled;
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Quoting), state);
    }

    bool canQuoteExternally() {
        return _canMarketMakersQuoteExternally;
    }

    bool handleLogin() {
        LOG(0, "Market makers logging in. {i1}", 0)
        bool success = true;
        for (auto& state : _contractStates) {
            success &= state._marketMaker.handleLogin();
        }
        return true;
    }

    bool handleLogout() {
       LOG(0, "Market makers logging out. {i1}", 0);
        handleSystemDisabled();
        return true;
    }

    void sendMarketUpdateToRelativeValue(uint32_t internalId, OrderIds id, hf::price_t bidPrice, hf::qty_t bidSize, hf::price_t askPrice, hf::qty_t askSize) {
        auto& outMessage = *static_cast<ViperMessage*>(_outToRV->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::MarketUpdate;
        MarketUpdate& message = reinterpret_cast<MarketUpdate &> (outMessage._data[0]);
        message._instrumentIndex = internalId;
        message._marketId = id;
        message._bidPrice = bidPrice;
        message._bidSize = bidSize;
        message._askPrice = askPrice;
        message._askSize = askSize;
        const auto outIdx = _outToRV->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GenericMessage));
        mb();
        _outToRV->appendNew(outIdx);
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

    bool processGridChange(GridUpdate& update) {
        if (update._update.gridid() == toUInt(WindowIds::MarketMakerParameters)) {
            bool valid = true;
            for (const proto::GridCell& cell : update._update.cells()) {
                const uint32_t internalId = cell.row();
                ContractStateT& state = _contractStates[internalId];
                valid &= state._marketMaker.validateParametersUpdate(cell);
            }

            if (!valid)
                return valid;

            for (const proto::GridCell& cell : update._update.cells()) {
                const uint32_t internalId = cell.row();
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.handleParametersUpdate(cell);
            }
            return true;
        }
        else if (update._update.gridid() == toUInt(WindowIds::StackerParameters)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                const uint32_t internalId = cell.row();
                ContractStateT& state = _contractStates[internalId];
                state._contractExecutor.handleStackerParameterUpdate(cell);
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::GlobalConfigs)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                GlobalConfig& config = _config->_globalConfigs[cell.row()];
                if (config._key == QUOTE_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._contractExecutor.handleQuoteBannerUpdate(period);
                    }
                } else if (config._key == FILL_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._contractExecutor.handleFillBannerUpdate(period);
                    }
                } else if (config._key == CHECK_HEDGE_QUOTE_PERIOD) {
                    _checkHedgeQuotePeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                }
                else if (config._key == MSG_REDUCE_QUOTE_MICS) {
                    uint64_t inDangerZonePeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    _throttler.updateInDangerZonePeriod(inDangerZonePeriod);
                }
                else if (config._key == MSG_HEDGES_INTERVAL_MICS) {
                    uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    _throttler.updateAtCapacityPeriod(atCapacityPeriod);
                }
                else if (config._key == PERCENT_MSG_HEDGES) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    _throttler.updateHedgePercentage(percent);
                }
                else if (config._key == PERCENT_MSG_REDUCE_QUOTE) {
                    double percent = boost::lexical_cast<double>(cell.value());
                    _throttler.updateDangerZonePercentage(percent);
                }
                else if (config._key == HEDGE_BANNER_MICS) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    for (auto& state : _contractStates) {
                        state._contractExecutor.handleHedgeQuoteBannerUpdate(period);
                    }
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::ContractParametersRv)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(cell.col());
                if (columnType == ContractParameterRVColumns::HedgeTolerance) {
                    const uint32_t internalId = cell.row();
                    ContractStateT& state = _contractStates[internalId];
                    double value = boost::lexical_cast<double>(cell.value());
                    state._contractExecutor.updateHedgeTolerance(ToRhonePrice(value));
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::ManualFillsEntry)) {
            onManualFills(update);
        }
        return true;
    }

    void init(ViperConfig& viperConfig, double frequency, SharedSystemState& systemState, uint64_t internalHeartbeatPeriod, std::function<bool(uint32_t)> pnlLimitsChecker) {
        _config = &viperConfig;
        _frequency = frequency;
        _systemState = &systemState;
        _pnlLimitsChecker = pnlLimitsChecker;
        _internalHeartbeatPeriod = internalHeartbeatPeriod;
        logging::GetLogger().logInfo(boost::str(boost::format("Executor initialized. Frequency=%1%") % _frequency));
    }

    bool canExecute() {
        return  isEnabled() & _gatewayStatus._active;
    }

    bool isEnabled() {
        return _systemState->_marketMaker == SystemState::Enabled;
    }

    bool isWithinPnlLimits() {
        return _pnlLimitsChecker(toUInt(StrategyId::MarketMaker));
    }

    bool isGatewayReady() {
        return _gatewayStatus._active;
    }

    void initialize(ViperConfig& viperConfig) {
        _contractStates.resize(viperConfig._allContracts.size());
        uint32_t fillBannerMicros = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(FILL_BANNER_PERIOD));
        uint64_t fillBannerPeriod = GetCyclesFromMicroSeconds(fillBannerMicros, _frequency);
        uint32_t quoteBannerMicros = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(QUOTE_BANNER_PERIOD));
        uint64_t quoteBannerPeriod = GetCyclesFromMicroSeconds(quoteBannerMicros, _frequency);
        uint32_t checkHedgeQuoteInMicros = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(CHECK_HEDGE_QUOTE_PERIOD));
        _checkHedgeQuotePeriod = GetCyclesFromMicroSeconds(checkHedgeQuoteInMicros, _frequency);
        uint32_t hedgeBannerMicos = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(HEDGE_BANNER_MICS));
        uint64_t hedgeBannerPeriod = GetCyclesFromMicroSeconds(hedgeBannerMicos, _frequency);
        for (uint32_t i = 0; i < viperConfig._allContracts.size(); ++i) {
            ConfigContract& contract = viperConfig._allContracts[i];
            _sidToInternalId.insert({contract._sid, contract._internalId});
            auto& state = _contractStates[i];
            uint32_t index;
            bool toMarketMake = viperConfig.isContractToMarketMake(contract._name, index);
            if (toMarketMake) {
                StackerConfig& stackerConfig = viperConfig._stackerConfigs[index];
                MarketMakerConfig& mmConfig = viperConfig._marketMakerConfigs[index];
                RelativeValueConfig& rvConfig = viperConfig._relativeValueConfigs[index];
                hf::price_t rvHedgeMatchTolerance = ToRhonePrice(rvConfig._hedgeTolerance);
                state._contractExecutor.initialize(contract, getRoutingIds(contract), stackerConfig, this, rvHedgeMatchTolerance, fillBannerPeriod, quoteBannerPeriod, hedgeBannerPeriod);
                state._marketMaker.initialize(contract, mmConfig, &state._contractExecutor);
            }
            subscribeAll(hf::VENUE::ICE, contract._sid);
        }
        const uint32_t nRoutes = this->context().getNumberOfOMPorts(hf::VENUE::ICE);

        double percentMsgHedges = boost::lexical_cast<double>(viperConfig.getGlobal(PERCENT_MSG_HEDGES));
        double percentMsgReduceQuote = boost::lexical_cast<double>(viperConfig.getGlobal(PERCENT_MSG_REDUCE_QUOTE));
        uint32_t atCapacityPeriodInMics = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(MSG_HEDGES_INTERVAL_MICS));
        uint64_t atCapacityPeriod = GetCyclesFromMicroSeconds(atCapacityPeriodInMics, _frequency);
        uint32_t inDangerZonePeriodInMics = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(MSG_REDUCE_QUOTE_MICS));
        uint64_t inDangerZonePeriod = GetCyclesFromMicroSeconds(inDangerZonePeriodInMics, _frequency);

        _throttler.initialize(_frequency, nRoutes, 1, 275, percentMsgReduceQuote, percentMsgHedges, inDangerZonePeriod, atCapacityPeriod);
    }

    std::vector<om::routing_id_t> getRoutingIds(ConfigContract& contract) {
        std::vector<om::routing_id_t> ids;
        const uint32_t nPorts = this->context().getNumberOfOMPorts(contract._venue);
        for (uint32_t i = 0; i < nPorts; ++i) {
            om::routing_id_t id = this->context().getRoutingId(contract._venue, contract._sid, i);
            if (id == om::NULL_ROUTING_ID) {
                logging::GetLogger().logErrorAndThrow(boost::str(boost::format("Got NULL routing id for Contract=%1% Port=%2%") % contract._name % i));
            }
            ids.push_back(id);
        }
        return ids;
    }

    void subscribeAll(hf::VENUE venue, hf::symbol_id_t sid) {
        this->context().subscribeMD(venue, sid, 1, true, true);
        this->context().subscribeOM(sid, true, true, true, true, true, true, true, true);
    }

    void checkSystemState() {
        if (_systemState->_marketMaker != _executorOldSystemState) {
            onExecutorStateChange();
            _executorOldSystemState = _systemState->_marketMaker;
        }
        if (_systemState->_relativeValue != _relativeValueOldSystemState) {
            onRelativeValueStateChange();
            _relativeValueOldSystemState = _systemState->_relativeValue;
        }
    }

    void onRelativeValueStateChange() {
        switch (_systemState->_relativeValue) {
            case SystemState::HardShutdown: {
                handleShutdown();
                break;
            }
            case SystemState::Disabled: {
                handleRelativeValueDisabled();
                break;
            }
            default:
                break;
        }
    }

    void onExecutorStateChange() {
        switch (_systemState->_marketMaker) {
            case SystemState::HardShutdown: {
                handleShutdown();
                break;
            }
            case SystemState::Disabled: {
                handleExecutorDisabled();
                break;
            }
            case SystemState::Enabled: {
                handleExecutorEnabled();
                break;
            }
            case SystemState::LoggedIn: {
                handleLogin();
                break;
            }
            case SystemState::LoggedOut: {
                handleLogout();
                break;
            }
            default:
                break;
        }
    }

    void handleShutdown() {
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateHardShutdown);
        LOG(0, "Executor - shutting down {i1}", 0)
        hf::Service::exitAll();
    }

    void disableSystem(const std::string& reason) {
        getAllOutOfMarket();
        LOG(0, "Disable System from Executor.  {i1}", 0)
        sendErrorMessage(boost::str(boost::format("Disable System from Executor. Reason=%1%") % reason), ErrorType::Critical);
        disableMarketMakers();
        _systemState->_marketMaker = SystemState::Disabled;
        _systemState->_relativeValue = SystemState::Disabled;
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void disableSystemViaShutdown(const std::string& reason) {
        LOG(0, "Shutdown System from Executor. {i1}", 0)
        sendErrorMessage(boost::str(boost::format("Shutdown System from Executor. Reason=%1%") % reason), ErrorType::Critical);
        handleShutdown();
    }

    void handleExecutorDisabled() {
        LOG(0, "Executor. Market Makers disabled. Removing orders. Reason=Requested {i1}", 0)
        if (_systemState->_relativeValue == SystemState::Enabled) {
            getMarketMakersOutOfMarket();
        } else {
            getAllOutOfMarket();
        }
        sendErrorMessage("Executor. Market Makers disabled. Removing orders. Reason=Requested", ErrorType::Critical);
        disableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void handleRelativeValueDisabled() {
        LOG(0, "Executor. Relative Value Disabled. Removing orders. Reason=Requested {i1}", 0)
        sendErrorMessage("Executor. Relative Value Disabled. Removing orders. Reason=Requested", ErrorType::Critical);
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                if (state._marketMaker._enabled) {
                    state._contractExecutor.getRelativeValueOutOfMarket();
                } else {
                    state._contractExecutor.getAllOutOfMarket();
                }
            }
        }
    }

    void handleSystemDisabled() {
        disableSystem("Logout");
    }

    void disableMarketMakers() {
        for (auto& state : _contractStates) {
            state._marketMaker.disable(false);
        }
    }

    bool isPnlWithinLimits() {
        const bool withinLimits = _pnlLimitsChecker(toUInt(StrategyId::MarketMaker));
        if (!withinLimits) {
            sendErrorMessage("MM - not within pnl limits.",  ErrorType::Critical);
            LOG(0, "MM - is not within pnl limits {i}", 0)
        }
        return withinLimits;
    }

    bool canEnable() {
        bool canEnable = true;
        if (!isPnlWithinLimits()) {
            canEnable = false;
        }
        if (!isGatewayReady()) {
            canEnable = false;
        }
        return canEnable;
    }

    void handleExecutorEnabled() {
        if (canEnable()) {
            enableMarketMakers();
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled);
            LOG(0, "Executor got enabled {i1}", 0)
        } else {
            _systemState->_marketMaker = SystemState::Disabled;
            sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
            LOG(0, "Executor could NOT be enabled {i1}", 0)
        }
    }

    void enableMarketMakers() {
        for (auto& state : _contractStates) {
            state._marketMaker.start(state._books[BID], state._books[ASK]);
        }
    }

    void getAllOutOfMarket() {
       LOG(0, "Executor. Removing all orders from market. {i1}", 0)
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                state._contractExecutor.getAllOutOfMarket();
            }
        }
    }

    void getMarketMakersOutOfMarket() {
        LOG(0, "Executor. Removing all market makers orders from market. {i1}", 0)
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                state._contractExecutor.getMarketMakerOutOfMarket();
            }
        }
    }

    void onMDBatch(const md::MDBatch& batch) override {
        LOG(0, "MM received MD batch. {i1}", batch.bid)
        const auto* data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto& mde = reinterpret_cast<const md::MDEvent&> (*data);
            data += mde.size;
            switch (mde.type) {
                case md::MDEvent::TYPE::UPDATE: {
                    const auto& event = static_cast<const md::MDUpdateEvent&> (mde);
                    onMDUpdate(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::TRADE: {
                    const auto& event = static_cast<const md::MDTradeEvent&> (mde);
                    onMDTrade(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::GAP: {
                    const auto& event = static_cast<const md::MDGapEvent&> (mde);
                    onMDGap(batch.venue, event);
                    break;
                }
                case md::MDEvent::TYPE::STATUS: {
                    const auto& event = static_cast<const md::MDStatusEvent&> (mde);
                    onMDStatus(batch.venue, event);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void onMDUpdate(hf::VENUE venue, const md::MDUpdateEvent& event) {
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            ContractStateT& state = _contractStates[internalId];
            const bool bidChanged = event.buy.updatedQtyNum == 1;
            const bool askChanged = event.sell.updatedQtyNum == 1;
            copyBooks(state, event, bidChanged, askChanged);
            if (bidChanged | askChanged) {
                if (canExecute()) {
                    LOG(0, "Market data changed. {i1}", 0)
                    if (state._marketMaker._toMarketMake) {
                        state._marketMaker.onChangeInOtherBooks(state._books[BID], state._books[ASK], bidChanged, askChanged);
                    }
                }
            }
        }
    }

    void copyBooks(ContractStateT& state, const md::MDUpdateEvent& event, bool bidChanged, bool askChanged) {
        if (bidChanged) {
            const md::top_t::level_t& topLevel = event.buy.top.l[0];
            copyBook(state._books[BID], topLevel);
        }
        if (askChanged) {
            const md::top_t::level_t& topLevel = event.sell.top.l[0];
            copyBook(state._books[ASK], topLevel);
        }
    }

    void copyBook(Book& book, const md::top_t::level_t& topLevel) {
        book._price = topLevel.pr;
        book._otherQuantity = topLevel.q;
        book._otherOrderCount = topLevel.o;
    }

    void onMDTrade(hf::VENUE venue, const md::MDTradeEvent& event) {
    }

    void onMDStatus(hf::VENUE venue, const md::MDStatusEvent& event) {
        auto it = _sidToInternalId.find(event.sid);
        if (it != _sidToInternalId.end()) {
            const uint32_t internalId = it->second;
            ContractStateT& state = _contractStates[internalId];
            switch (event.statusType) {
                case md::MDStatusEvent::TYPE::RECOVERING:
                case md::MDStatusEvent::TYPE::TIMEOUT:
                case md::MDStatusEvent::TYPE::NOT_CONFIGURED: {
                    LOG(0, "Contract {C[i1]} market data not working. Reason={i2}", getLoggingContract(internalId), toUInt(event.statusType))
                    stopContract(state,  boost::str(boost::format("Contract %1% market data not working. Reason=%2%") % state._contractExecutor._name % toUInt(event.statusType)));
                    break;
                }
                case md::MDStatusEvent::TYPE::RECOVERED: {
                    state._contractExecutor._contractCanTrade = true;
                    LOG(0, "Contract {C[i1]} market data recovered.", getLoggingContract(internalId))
                    break;
                }
                case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                    switch (event.extStatus) {
                        case 'O': {
                            state._contractExecutor._contractCanTrade = true;
                            LOG(0, "Contract {C[i1]} market data - opened.", getLoggingContract(internalId))
                            break;
                        }
                        case 'C': {
                            LOG(0, "Contract {C[i1]} market data - closed.", getLoggingContract(internalId))
                            stopContract(state, boost::str(boost::format("Contract %1% market data - closed.") % state._contractExecutor._name));
                        }
                        case 'E': {
                            LOG(0, "Contract {C[i1]} market data - expired.", getLoggingContract(internalId))
                            stopContract(state, boost::str(boost::format("Contract %1% market data - expired.") % state._contractExecutor._name));
                        }
                        case '1': {
                            LOG(0, "Contract {C[i1]} market data - entered pre-open.", getLoggingContract(internalId))
                            break;
                        }
                        default: {
                            break;
                        }
                    }
                    break;
                }
                case md::MDStatusEvent::TYPE::UNKNOWN: {
                    LOG(0, "Contract {C[i1]} market data - received unknown md status.", getLoggingContract(internalId))
                    sendErrorMessage(boost::str(boost::format("Contract %1% market data - received unknown md status.") % state._contractExecutor._name), ErrorType::Critical);
                    break;
                }
            }
        }
    }

    template <typename T>
    void stopContract(T& state, const std::string& message) {
        state._contractExecutor._contractCanTrade = false;
        if (isEnabled()) {
            state._marketMaker.disable();
        }
        sendErrorMessage(message, ErrorType::Critical);
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent& event) {
        disableSystem("MD feed gapped.");
    }

    virtual void onOMBatch(const om::OMBatch &batch) override {
        const auto *data = batch.data;
        for (auto i = batch.count; i; --i) {
            const auto &ome = reinterpret_cast<const om::OMEvent &> (*data);
            data += ome.size;
            switch (ome.type) {
                case om::OMEvent::TYPE::OE: {
                    const auto &event = static_cast<const om::OMExecutionEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    if (event.totalExecutedQty > order.executedQty) {
                        const_cast<om::order_t&> (order).is_active = event.is_active;
                        const_cast<om::order_t&> (order).executedQty = event.totalExecutedQty;
                        onOrderExecution(event, order);
                    }
                    break;
                }
                case om::OMEvent::TYPE::SPREAD_LEG_EXECUTION: {
                    const auto &event = static_cast<const om::OMSpreadLegExecutionEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    const_cast<om::order_t &> (order).is_active = event.is_active;
                    onOrderSpreadLegExecution(event, order);
                    break;
                }
                case om::OMEvent::TYPE::OA: {
                    const auto &event = static_cast<const om::OMOrderAcceptedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    onOrderAccepted(event, order);
                    break;
                }
                case om::OMEvent::TYPE::OR: {
                    const auto &event = static_cast<const om::OMOrderRejectedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    const_cast<om::order_t &> (order).is_active = false;
                    onOrderRejected(event, order);
                    break;
                }
                case om::OMEvent::TYPE::OC: {
                    const auto &event = static_cast<const om::OMOrderCanceledEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    const_cast<om::order_t &> (order).is_active = false;
                    onOrderCanceled(event, order);
                    break;
                }
                case om::OMEvent::TYPE::CR: {
                    const auto &event = static_cast<const om::OMOrderCancelRejectedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    onOrderCancelRejected(event, order);
                    break;
                }
                case om::OMEvent::TYPE::MASS_CANCEL_ACCEPTED: {
                    const auto &event = static_cast<const om::OMMassCancelAcceptedEvent &> (ome);
                    onMassCancelAccepted(event);
                    break;
                }
                case om::OMEvent::TYPE::MASS_CANCEL_REJECTED: {
                    const auto &event = static_cast<const om::OMMassCancelRejectedEvent &> (ome);
                    onMassCancelRejected(event);
                    break;
                }
                case om::OMEvent::TYPE::OM: {
                    const auto &event = static_cast<const om::OMOrderModifiedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    onOrderReplaced(event, order);
                    break;
                }
                case om::OMEvent::TYPE::MR: {
                    const auto &event = static_cast<const om::OMOrderModifyRejectedEvent &> (ome);
                    const auto &order = this->context().getOrder(event.oid);
                    onOrderReplaceRejected(event, order);
                    break;
                }
                case om::OMEvent::TYPE::PORT_STATUS: {
                    const auto &event = static_cast<const om::PortStatusUpdate &> (ome);
                    onOMPortStatusUpdate(event);
                    break;
                }
                case om::OMEvent::TYPE::GW_STATUS: {
                    const auto &event = static_cast<const om::GWStatusUpdate &> (ome);
                    onOMGWStatusUpdate(event);
                    break;
                }
                case om::OMEvent::TYPE::STRATEGY_LOGIN_RESPONSE: {
                    const auto &event = static_cast<const om::StrategyLoginResponseEvent &> (ome);
                    onStrategyLoginResponse(event);
                    break;
                }
                default:
                    break;
            }
        }
    }

    virtual void onStrategyLoginResponse(const om::StrategyLoginResponseEvent &event) {
        LOG(0, "Received Strategy login response. Success={i1} PID={i2} AlgoId={i3}", event.success, event.pid, event.algo_id)
    }

    void onOMGWStatusUpdate(const om::GWStatusUpdate& event) override {
        handleGatewayStatusChange(event);
    }

    void onOMPortStatusUpdate(const om::PortStatusUpdate& event) override {
        handleGatewayStatusChange(event);
    }

    template <typename T>
    void handleGatewayStatusChange(T& event) {
        _gatewayStatus._active = event.active;
        _gatewayStatus._message = event.reasonText;
        if (_systemState->_marketMaker != SystemState::LoggedOut) {
            sendGatewayStatusToFront();
            std::string message = boost::str(boost::format("Gateway status change. Running=%1%") % (event.active ? "YES" : "NO"));
            sendErrorMessage(message, ErrorType::Critical);
        }
        if (!_gatewayStatus._active) {
            if (isEnabled()) {
                disableSystem("Gateway down.");
            }
        }
    }

    void sendGatewayStatusToFront() {
        auto& outMessage = *static_cast<ViperMessage*>(_outToFront->getPoolP()->getNextP());
        outMessage._type = ViperMessageType::GatewayStatus;
        GatewayStatusMessage& message = reinterpret_cast<GatewayStatusMessage &> (outMessage._data[0]);
        message._active = _gatewayStatus._active;
        if (_gatewayStatus._message.empty()) {
            strcpy(message._message, _gatewayStatus._message.c_str());
        }
        const auto outIdx = _outToFront->getPoolP()->append(sizeof(ViperMessageType) + sizeof(GatewayStatusMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void handleGuiInitialized() {
        sendGatewayStatusToFront();
    }

    static const uint32_t INTERNAL_ID = 0;

    bool isRvHedge(const om::order_t& order) {
        return order.ot == om::ORDER_TYPE::FAK;
    }

    uint32_t getInternalId(const om::order_t& order) {
        return order.debug.i[INTERNAL_ID];
    }

    bool isLoggedIn() {
        return _systemState->_marketMaker != SystemState::LoggedOut;
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            const_cast<om::order_t&> (order).leavesQty = event.acceptedQty;
            if (!isRvHedge(order)) {
                auto& executor = getContractExecutor(order);
                executor.onOrderAccepted(event, order);
            }
        } else {
            LOG(0, "Received Order Accepted when logged out. {i1}", event.oid)
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            if (isRvHedge(order)) {
                LOG(0, "Hedge order rejected. Contract={C[i1]} OID={i2} Reason={i3}", getLoggingContract(getInternalId(order)), order.oid, event.reasonCodeExt)
                disableSystem("Hedge order rejected");
            } else {
                auto& executor = getContractExecutor(order);
                executor.onOrderRejected(event, order);
            }
        } else {
            LOG(0, "Received Order Rejected when logged out. {i1}", event.oid)
        }
    }
    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            auto& executor = getContractExecutor(order);
            executor.onOrderReplaced(event, order);
        } else {
            LOG(0, "Received Order Replaced when logged out. {i1}", event.oid)
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            auto& executor = getContractExecutor(order);
            executor.onOrderReplaceRejected(event, order);
            ++executor._messages._modifyRejects;
        } else {
            LOG(0, "Received Order Replace Rejected when logged out. {i1}", event.oid)
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) override {
        if (isLoggedIn()) {
            auto& executor = getContractExecutor(order);
            if (!isRvHedge(order)) {
                executor.onOrderExecution(event, order);
            } else {
                const uint32_t internalId = executor._internalMatchingEngine._internalId;
                LOG(0, "Hedge order filled. Contract={C[i1]} OID={i2}", getLoggingContract(internalId), order.oid)
            }
            executor._messages._fills += event.filledQty;
        } else {
            LOG(0, "Received Order Replace Executed when logged out. {i1}", event.oid)
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            if (isRvHedge(order)) {
                const uint32_t internalId = getInternalId(order);
                LOG(0, "Hedge order cancelled. Contract={C[i1]} OID={i2}", getLoggingContract(internalId), order.oid)
                sendReverseFillToRelativeValue(internalId, order);
            } else {
                auto& executor = getContractExecutor(order);
                executor.onOrderCanceled(event, order);
            }
        } else {
            LOG(0, "Received Order Cancelled when logged out. {i1}", event.oid)
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) override {
        if (isLoggedIn()) {
            auto& executor = getContractExecutor(order);
            executor.onOrderCancelRejected(event, order);
            ++executor._messages._cancelRejects;
        } else {
            LOG(0, "Received Order Cancel Rejected when logged out. {i1}", event.oid)
        }
    }

    void onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::order_t &order) {}
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event) {}
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event) {}

    void cancelOrder(uint32_t internalId, const om::order_t* order) {
        this->context().cancelOrder(order->oid);
        ContractStateT& state = _contractStates[internalId];
        ++state._contractExecutor._messages._cancels;
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, bool isHedge = false) {
        const uint32_t route = _throttler.getRoute();
        ContractStateT& state = _contractStates[internalId];
        om::order_t* order = this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._contractExecutor._rid[route], state._marketMaker._contract._sid, om::ORDER_TYPE::LMT, ToRhoneSide(side), price, size);
        if (order == nullptr) {
            LOG(0, "Could not send limit order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), side, ToDollars(price), size)
        } else {
            LOG(0, "Sent limit order. Contract={C[i1]} Side={i2} Price={d1} Size={i3} OID={i4}", getLoggingContract(internalId), side, ToDollars(price), size, order->oid)
            order->debug.i[INTERNAL_ID] = internalId;
            ++state._contractExecutor._messages._inserts;
            if (!isHedge) {
                addMessageToThrottler(route);
            }
        }
        return order;
    }

    void addMessageToThrottler(uint32_t route) {
        const uint64_t now = clockUtil::GetCycles();
        bool isInDangerZone = false;
        bool isAtCapacity = false;
        _throttler.addMessage(route, now, isInDangerZone, isAtCapacity);

        if (isAtCapacity) {
            _throttler.setAtCapacity(now);
            setThrottlingBanner(THROTTLING_MODE_AT_CAPACITY);
        } else {
            if (!_throttler.wasInDangerZone() & isInDangerZone) {
                _throttler.setInDangerZone(now);
                setThrottlingBanner(THROTTLING_MODE_DANGER_ZONE);
            }
        }
    }

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        ContractStateT& state = _contractStates[internalId];
        const uint32_t route = _throttler.getRoute();
        om::order_t* order =  this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._contractExecutor._rid[route], state._marketMaker._contract._sid, om::ORDER_TYPE::FAK, ToRhoneSide(side), price, size);
        if (order == nullptr) {
            LOG(0, "Could not send FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), side, ToDollars(price), size)
        } else {
            LOG(0, "Sent FAK order. Contract={C[i1]} Side={i2} Price={d1} Size={i3} OID={i4}", getLoggingContract(internalId), side, ToDollars(price), size, order->oid)
            order->debug.i[INTERNAL_ID] = internalId;
            ++state._contractExecutor._messages._inserts;
        }
        return order;
    }

    void modifyOrder(uint32_t internalId, const om::order_t* order, hf::price_t price, hf::qty_t size) {
        const bool done = this->context().modifyOrder(order->oid, price, size);
        ContractStateT& state = _contractStates[internalId];
        if (done) {
            ++state._contractExecutor._messages._modifies;
            LOG(0, "Sent modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), ToViperSide(order->side), ToDollars(price), size)
            addMessageToThrottler(order->pid);
        } else {
            LOG(0, "Could not modify order. Contract={C[i1]} Side={i2} Price={d1} Size={i3}", getLoggingContract(internalId), ToViperSide(order->side),ToDollars(price), size)
        }
    }

    void setThrottlingBanner(THROTTLING_MODE mode) {
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                if (mode == _throttler.getMode()) {
                    state._contractExecutor.setThrottlingBanner(mode);
                }
            }
        }
    }

    void onCriticalError(const std::string& reason) {
        std::string message = boost::str(boost::format("Critical Error: %1%") % reason);
        disableSystemViaShutdown(reason);
    }

    void addCheckHedgeQuoteTimer(uint32_t internalId, om::order_id_t oid) {
        uint64_t expiryTime = clockUtil::GetCycles() + _checkHedgeQuotePeriod;
        _checkQuotesTimer.addCheckQuotesEntry(expiryTime, internalId, oid, TimerType::CheckHedgeQuote);
    }

    void addFillBannerTimer(uint64_t expiryTime, uint32_t internalId, uint32_t side) {
        _fillBannerTimer.addBannerEntry(expiryTime, internalId, side, TimerType::FillBanner);
    }

    void addQuoteBannerTimer(uint64_t expiryTime, uint32_t internalId, uint32_t side, TimerType type) {
        _quoteBannerTimer.addBannerEntry(expiryTime, internalId, side, type);
    }

    void checkTimers(uint64_t now) {
        _checkQuotesTimer.expire(now);
        _fillBannerTimer.expire(now);
        _quoteBannerTimer.expire(now);
        if (_throttler.isBanOn()) {
            if (_throttler.didBanExpire(now)) {
                setThrottlingBanner(_throttler.getNewMode(now));
            }
        }
    }

    void expireTimer(TimerEntry& action) {
        switch (action._type) {
            case TimerType::FillBanner: {
                expireFillBanner(action._internalId, action._side);
                break;
            }
            case TimerType::QuoteBanner: {
                expireQuoteBanner(action._internalId, action._side);
                break;
            }
            case TimerType::CheckHedgeQuote: {
                checkHedgeQuote(action._internalId, action._oid);
                break;
            }
            case TimerType::HedgeBanner: {
                expireHedgeQuoteBanner(action._internalId, action._side);
                break;
            }
        }
    }

    void expireHedgeQuoteBanner(uint32_t internalId, uint32_t side) {
        ContractStateT& state = _contractStates[internalId];
        state._contractExecutor.expireHedgeQuoteBanner(side);
    }

    void expireFillBanner(uint32_t internalId, uint32_t side) {
        ContractStateT& state = _contractStates[internalId];
        state._contractExecutor.expireFillBanner(side);
    }

    void expireQuoteBanner(uint32_t internalId, uint32_t side) {
        ContractStateT& state = _contractStates[internalId];
        state._contractExecutor.expireQuoteBanner(side);
    }

    void checkHedgeQuote(uint32_t internalId, om::order_id_t oid) {
        const om::order_t& order = this->context().getOrder(oid);
        ContractStateT& state = _contractStates[internalId];
        state._marketMaker.handleUnfilledPayUp(order);
    }

    bool onManualFills(GridUpdate& update) {
        uint32_t numberOfFills = 0;
        for (int i = 0; i < update._update.cells_size(); ++i) {
            const proto::GridCell& gridCell = update._update.cells(i);
            ManualFillsEntryColumns columnType = static_cast<ManualFillsEntryColumns>(gridCell.col());
            ManualFill& fill = _manualFills[gridCell.row()];
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
                    fill._strategyId = atoi(gridCell.value().c_str());
                    break;
                }
            }
            numberOfFills = std::max(numberOfFills, gridCell.row());
        }

        for (uint32_t index = 0; index <= numberOfFills; ++index) {
            ManualFill& fill = _manualFills[index];
            if (fill._strategyId == toUInt(StrategyId::MarketMaker)) {
                ContractStateT& state = _contractStates[fill._contract];
                uint32_t size = std::abs(fill._size);
                uint32_t side = fill._size > 0 ? BID : ASK;
                state._marketMaker.addManualPosition(size, side);
            }
        }

        return true;
    }

    void sendAcceptedManualFillToFront(uint32_t internalId, hf::qty_t size) {
        sendGenericMessageToFront(ViperMessageType::AcceptedManualFill, internalId, size);
    }

    std::vector<ContractStateT> _contractStates;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;

    md::queue::MDQueueResource* _inFromFront;
    md::queue::MDQueueResource* _inFromRV;
    md::queue::MDQueueResource* _outToRV;
    md::queue::MDQueueResource* _outToFront;

    SharedSystemState* _systemState;
    SystemState _executorOldSystemState = SystemState::LoggedOut;
    SystemState _relativeValueOldSystemState = SystemState::LoggedOut;

    bool _canMarketMakersQuoteExternally = true;

    GatewayStatus _gatewayStatus;

    ViperConfig* _config;
    double _frequency;

    Timer<Executor> _fillBannerTimer;
    Timer<Executor> _quoteBannerTimer;
    Timer<Executor> _checkQuotesTimer;
    uint64_t _checkHedgeQuotePeriod;
    Throttler _throttler;

    uint64_t _nextInternalHeartbeat;
    uint64_t _internalHeartbeatPeriod;

    std::vector<ManualFill> _manualFills;

    std::function<bool(uint32_t)> _pnlLimitsChecker;

    std::string _strategyName;
    std::string _strategyVersion;

    hf::queue::DebugQueue* _kdbLogger;
    bool _needToSetLogger = true;
};

typedef Executor<::strat::Strategy> ExecutorT;

}

#endif //VIPER_EXECUTOR_H
