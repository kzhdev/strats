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

namespace Python {

template <typename T>
struct ContractState {
    ExecutorForOneContract<T> _contractExecutor;
    SimpleMarketMaker<ExecutorForOneContract<T>> _marketMaker;
    Book _books[2];
};

struct GatewayStatus {
    std::string _message;
    bool _active = false;
};

template<typename StrategyT>
class Executor : public StrategyT {

public:
    typedef ContractState<Executor> ContractStateT;

    Executor() {
    }

    void onStratBegin() override {
        resetQueues();
        initialize(*_config);
    }

    void resetQueues() {
        _inFromFront->resetR();
        _inFromRV->resetR();
        _outToRV->resetW();
        _outToFront->resetW();
    }

    void onStratEnd() override {
    }

    void onPostCreate(const libconfig::Setting& config) override {
    }

    virtual void onStratLoop() override {
        processMessagesFromRV();
    }

    virtual void onStratIdle() {
        processMessagesFromFront();
        checkSystemState();
        uint64_t now = clockUtil::GetCycles();
        expireBanners(now);
    }

    void processMessagesFromRV() {
        while (true) {
            uint64_t idx = _inFromRV->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                return;

            _inFromRV->freeNext();

            auto& message = _inFromRV->pool()->getMessage(idx);

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
                default:
                    break;
            }
        }
    }

    void expireBanners(uint64_t now) {
        if (now > _bannerExpiryTime) {
            for (auto& state : _contractStates) {
                if (state._marketMaker._toMarketMake) {
                    state._contractExecutor.expireBanner();
                }
            }
            _bannerExpiryTime = now + _bannerExpiryPeriod;
        }
    }

    void onQuoteOrderFromRv(QuoteOrderMessage& order) {
        ContractStateT& state = _contractStates[order._instrumentIndex];
        state._contractExecutor.onQuoteOrderFromRelativeValue(order);
    }

    void onInternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::Internal);
                sendFillToFront(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::Internal, FillSource::Quote);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.onInternalFill(order, price, size, state._books[BID], state._books[ASK]);
                sendFillToFront(internalId, order->_side, price, size, StrategyId::MarketMaker, FillType::Internal, FillSource::Quote);
                break;
            }
            default:
                break;
        }
    }

    void onExternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                _logger.logInfo(boost::str(boost::format("Executor: External fill. Sending to RV. Contract=%1% Side=%2% Price=%3% Size=%4%") % getContractName(internalId) % GetSideString(order->_side) % price % size));
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::External);
                sendFillToFront(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::External, FillSource::Quote);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.onExternalFill(order, price, size, state._books[BID], state._books[ASK]);
                sendFillToFront(internalId, order->_side, price, size, StrategyId::MarketMaker, FillType::External, FillSource::Quote);
                break;
            }
            default:
                break;
        }
    }

    void onExternalOverFill(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        _logger.logInfo(boost::str(boost::format("Executor: External overfill. Contract=%1% Side=%2% Price=%3% Size=%4%") % getContractName(internalId) % GetSideString(side) % price % size));
        ContractStateT& state = _contractStates[internalId];
        hf::qty_t remainingSize = size;
        if (state._marketMaker._enabled) {
            hf::qty_t takenSize = state._marketMaker.takeOverFill(side, remainingSize);
            sendFillToFront(internalId, side, price, takenSize, StrategyId::MarketMaker, FillType::External, FillSource::Overfill);
            remainingSize -= takenSize;
        }
        if (remainingSize > 0) {
            if (_systemState->_relativeValue == SystemState::Enabled) {
                sendFillToRV(internalId, side, price, remainingSize, StrategyId::RelativeValue, FillType::External);
                sendFillToFront(internalId, side, price, remainingSize, StrategyId::RelativeValue, FillType::External, FillSource::Overfill);
            } else {
                sendFillToFront(internalId, side, price, remainingSize, StrategyId::MarketMaker, FillType::External, FillSource::Overfill);
                std::string message = boost::str(boost::format("Executor: Nobody can handle overfill. MM at capacity. RV disabled. Contract=%1% Side=%2% Size=%3%") % getContractName(internalId) % GetSideString(side) %  remainingSize);
                disableSystem(message);
            }
        }
    }

    void sendFillToFront(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type, FillSource source) {
        auto &outMessage = _outToFront->pool()->getNextMessage();
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractId = contractIndex;
        message._side = side;
        message._qty = size;
        message._strategy = strategyId;
        message._type = type;
        message._price = price;
        message._source = source;
        const auto outIdx = _outToFront->pool()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void onEndOfExternalFills(uint32_t internalId, hf::SIDE side) {
        ContractStateT& state = _contractStates[internalId];
        uint32_t s = ToViperSide(side);
        state._marketMaker.onEndOfExternalFills(s);
    }

    void onHedgeOrderFromRv(HedgeOrderMessage& order) {
        _logger.logInfo(boost::str(boost::format("MM received hedge order from RV. Contract=%1% Side=%2% Price=%3% Size=%4%") % _config->_allContracts[order._instrumentIndex]._name %
                                                                           GetSideString(order._side) % order._price % order._size));
        ContractStateT& state = _contractStates[order._instrumentIndex];
        const hf::qty_t unmatchedSize = state._contractExecutor.onHedgeOrderFromRelativeValue(order);
        if (!order._isExternal) {
            _logger.logInfo(boost::str(boost::format("Missed internal only hedge. Back to RV. Contract=%1%") % getContractName(order._instrumentIndex)));
            sendFillToRV(order._instrumentIndex, GetOtherSide(order._side), order._price, unmatchedSize, StrategyId::RelativeValue, FillType::Internal);
        }
    }

    std::string& getContractName(uint32_t id) {
        return _config->_allContracts[id]._name;
    }

    void sendExternalRvHedge(HedgeOrderMessage& order) {
        om::order_t* rhoneOrder = newFAKOrder(order._instrumentIndex, order._side, order._price, order._size);
        sendHedgeMarketDataAdjustmentAck(order._hedgeId, order._instrumentIndex, GetOtherSide(order._side), rhoneOrder->oid, order._size);
    }

    void sendHedgeMarketDataAdjustmentAck(uint32_t hedgeId, uint32_t instrumentIndex, uint32_t side, om::order_id_t orderId, hf::qty_t size) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = ViperMessageType::HedgeMDAdjsutAck;
        HedgeMarketDataAdjustmentAck& message = reinterpret_cast<HedgeMarketDataAdjustmentAck &> (outMessage._data[0]);
        message._hedgeId = hedgeId;
        message._instrumentIndex = instrumentIndex;
        message._side = side;
        message._size = size;
        message._orderId = orderId;
        const auto outIdx = _outToRV->pool()->append(sizeof(ViperMessageType) + sizeof(HedgeMarketDataAdjustmentAck));
        mb();
        _outToRV->appendNew(outIdx);
    }

    void sendFillToRV(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractId = contractIndex;
        message._side = side;
        message._qty = size;
        message._strategy = strategyId;
        message._type = type;
        message._price = price;
        const auto outIdx = _outToRV->pool()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    ExecutorForOneContract<Executor>& getContractExecutor(const om::order_t& order) {
        ContractStateT& state = _contractStates[getInternalId(order)];
        return state._contractExecutor;
    }

    void sendReverseFillToRelativeValue(uint32_t internalId, const om::order_t& order) {
        const hf::qty_t unfilledSize = order.leavesQty;
        const uint32_t reverseSide = order.side == hf::SIDE::BUY ? 1 : 0;
        ContractStateT& state = _contractStates[internalId];
        _logger.logInfo(boost::str(boost::format("Sending unfilled size as reverse to RV. Contract=%1% OID=%2%") % state._contractExecutor._name % order.oid));
        sendFillToRV(internalId, reverseSide, order.pr, unfilledSize, StrategyId::RelativeValue, FillType::External);
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
        // TODO
    }

    bool handleLogin() {
        _logger.logInfo("Market makers logging in.");
        bool success = true;
        for (auto& state : _contractStates) {
            success &= state._marketMaker.handleLogin();
        }
        return true;
    }

    bool handleLogout() {
        _logger.logInfo("Market makers logging out.");
        handleSystemDisabled();
        return true;
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

    bool processGridChange(GridUpdate& update) {
        if (update._update.gridid() == toUInt(WindowIds::MarketMakerParameters)) {
            bool valid = true;
            for (const proto::GridCell& cell : update._update.cells()) {
                const uint32_t internalId = cell.col();
                ContractStateT& state = _contractStates[internalId];
                valid &= state._marketMaker.validateParametersUpdate(cell);
            }

            if (!valid)
                return valid;

            for (const proto::GridCell& cell : update._update.cells()) {
                const uint32_t internalId = cell.col();
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
                const uint32_t internalId = cell.col();
                ContractStateT& state = _contractStates[internalId];
                GlobalConfig& config = _config->_globalConfigs[cell.row()];
                if (config._key == QUOTE_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    state._contractExecutor.handleQuoteBannerUpdate(period);
                } else if (config._key == FILL_BANNER_PERIOD) {
                    uint64_t period = GetCyclesFromMicroSeconds(boost::lexical_cast<uint32_t>(cell.value()), _frequency);
                    state._contractExecutor.handleFillBannerUpdate(period);
                }
            }
        }
        else if (update._update.gridid() == toUInt(WindowIds::ContractParametersRv)) {
            for (const proto::GridCell& cell : update._update.cells()) {
                ContractParameterRVColumns columnType = static_cast<ContractParameterRVColumns>(cell.col());
                if (columnType == ContractParameterRVColumns::HedgeTolerance) {
                    const uint32_t internalId = cell.col();
                    ContractStateT& state = _contractStates[internalId];
                    double value = boost::lexical_cast<double>(cell.value());
                    state._contractExecutor.updateHedgeTolerance(ToRhonePrice(value));
                }
            }
        }
        return true;
    }

    void init(ViperConfig& viperConfig, ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromRV, ViperQueue* outToRV, double frequency, SharedSystemState& systemState) {
        initQueues(inFromFront, outToFront, inFromRV, outToRV);
        _config = &viperConfig;
        _frequency = frequency;
        _bannerExpiryPeriod = GetCyclesFromSeconds(1, _frequency);
        _systemState = &systemState;
    }

    bool canExecute() {
        return  isEnabled() & _gatewayStatus._active;
    }

    bool isEnabled() {
        return _systemState->_marketMaker == SystemState::Enabled;
    }

    void initQueues(ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromRV, ViperQueue* outToRV) {
        _inFromFront = inFromFront;
        _outToFront = outToFront;
        _inFromRV = inFromRV;
        _outToRV = outToRV;
    }

    void initialize(ViperConfig& viperConfig) {
        _contractStates.resize(viperConfig._allContracts.size());
        uint32_t minBanner = std::numeric_limits<uint32_t>::max();
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
                auto& pc = viperConfig._pricingConfigs[i];
                auto rid = this->context().getRoutingId(contract._venue, contract._sid);
                hf::price_t rvHedgeMatchTolerance = ToRhonePrice(rvConfig._hedgeTolerance);
                uint32_t fillBannerMicros = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(FILL_BANNER_PERIOD));
                minBanner = std::min(minBanner, fillBannerMicros);
                uint64_t fillBannerPeriod = GetCyclesFromMicroSeconds(fillBannerMicros, _frequency);
                uint32_t quoteBannerMicros = boost::lexical_cast<uint32_t>(viperConfig.getGlobal(QUOTE_BANNER_PERIOD));
                minBanner = std::min(minBanner, quoteBannerMicros);
                uint64_t quoteBannerPeriod = GetCyclesFromMicroSeconds(quoteBannerMicros, _frequency);;
                state._contractExecutor.initialize(contract, rid, stackerConfig, this, rvHedgeMatchTolerance, fillBannerPeriod, quoteBannerPeriod);
                state._marketMaker.initialize(contract, pc._establishedLevel, mmConfig, &state._contractExecutor);
            }
            subscribeAll(hf::VENUE::ICE, contract._sid);
        }
        _bannerExpiryPeriod = GetCyclesFromMicroSeconds(minBanner * 0.5, _frequency);
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
        _logger.logInfo("Executor - shutting down");
        hf::Service::exitAll();
    }

    void disableSystem(const std::string& reason) {
        getAllOutOfMarket();
        std::string message = boost::str(boost::format("Disable System from Executor. Reason=%1%") % reason);
        _logger.logInfo(message);
        sendErrorMessage(message, ErrorType::Critical);
        disableMarketMakers();
        _systemState->_marketMaker = SystemState::Disabled;
        _systemState->_relativeValue = SystemState::Disabled;
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void disableSystemViaShutdown(const std::string& reason) {
        std::string message = boost::str(boost::format("Shutdown System from Executor. Reason=%1%") % reason);
        _logger.logInfo(message);
        sendErrorMessage(message, ErrorType::Critical);
        handleShutdown();
    }

    void handleExecutorDisabled() {
        std::string message = boost::str(boost::format("Executor. Market Makers disabled. Removing orders. Reason=Requested"));
        _logger.logInfo(message);
        if (_systemState->_relativeValue == SystemState::Enabled) {
            getMarketMakersOutOfMarket();
        } else {
            getAllOutOfMarket();
        }
        sendErrorMessage(message, ErrorType::Critical);
        disableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void handleRelativeValueDisabled() {
        std::string message = ("Executor. Relative Value Disabled. Removing orders. Reason=Requested");
        _logger.logInfo(message);
        sendErrorMessage(message, ErrorType::Critical);
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
            state._marketMaker.disable();
        }
    }

    void handleExecutorEnabled() {
        enableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled);
        _logger.logInfo("Executor got enabled");
    }

    void enableMarketMakers() {
        for (auto& state : _contractStates) {
            state._marketMaker.enable(state._books[BID], state._books[ASK]);
        }
    }

    void handleQuotingEnabled() {
        for (auto& state : _contractStates) {
            state._marketMaker.enableQuoting(state._books[BID], state._books[ASK]);
        }
        _logger.logInfo("MM quoting got enabled");
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Quoting), proto::SystemState::SystemStateEnabled);
    }

    void handleQuotingDisabled() {
        for (auto& state : _contractStates) {
            state._marketMaker.disableQuoting();
        }
        _logger.logInfo("MM quoting got disabled");
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Quoting), proto::SystemState::SystemStateDisabled);
    }

    void getAllOutOfMarket() {
        _logger.logInfo("Executor. Removing all orders from market.");
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                state._contractExecutor.getAllOutOfMarket();
            }
        }
    }

    void getMarketMakersOutOfMarket() {
        _logger.logInfo("Executor. Removing all market makers orders from market.");
        for (auto& state : _contractStates) {
            if (state._marketMaker._toMarketMake) {
                state._contractExecutor.getMarketMakerOutOfMarket();
            }
        }
    }

    void onMDBatch(const md::MDBatch& batch) override {
        _logger.logInfo(boost::str(boost::format("MM received MD batch. %1%") % batch.bid));
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
                    state._marketMaker.onChangeInOtherBooks(state._books[BID], state._books[ASK], bidChanged, askChanged);
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
                    std::string message = boost::str(boost::format("Contract %1% market data not working. Reason=%2% MM disabling.") % state._contractExecutor._name % toUInt(event.statusType));
                    stopContract(state, message);
                    break;
                }
                case md::MDStatusEvent::TYPE::RECOVERED: {
                    std::string message = boost::str(boost::format("Contract %1% market data recovered. MM enabling.") % state._contractExecutor._name);
                    startContract(state, message);
                    break;
                }
                case md::MDStatusEvent::TYPE::TRADING_STATUS: {
                    switch (event.extStatus) {
                        case 'O': {
                            std::string message = boost::str(boost::format("Contract %1% market data - opened. MM enabling.") % state._contractExecutor._name);
                            startContract(state, message);
                            break;
                        }
                        case 'C': {
                            std::string message = boost::str(boost::format("Contract %1% market data - closed. MM disabling.") % state._contractExecutor._name);
                            stopContract(state, message);
                        }
                        case 'E': {
                            std::string message = boost::str(boost::format("Contract %1% market data - expired. MM disabling.") % state._contractExecutor._name);
                            stopContract(state, message);
                        }
                        case '1': {
                            std::string message = boost::str(boost::format("Contract %1% market data - entered pre-open. MM.") % state._contractExecutor._name);
                            _logger.logInfo(message);
                            break;
                        }
                        default: {
                            break;
                        }
                    }
                    break;
                }
                case md::MDStatusEvent::TYPE::UNKNOWN: {
                    std::string message = boost::str(boost::format("Contract %1% market data - received unknown md status.") % state._contractExecutor._name);
                    sendErrorMessage(message, ErrorType::Critical);
                    _logger.logInfo(message);
                    break;
                }
            }
        }
    }

    template <typename T>
    void startContract(T& state, const std::string& message) {
        return;
        const bool didEnable = state._marketMaker.enable(state._books[BID], state._books[ASK]);
        if (didEnable) {
            sendErrorMessage(message, ErrorType::Critical);
            _logger.logInfo(message);
        }
    }

    template <typename T>
    void stopContract(T& state, const std::string& message) {
        const bool didDisable = state._marketMaker.disable();
        if (didDisable) {
            sendErrorMessage(message, ErrorType::Critical);
            _logger.logInfo(message);
        }
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
                default:
                    break;
            }
        }
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
        auto &outMessage = _outToFront->pool()->getNextMessage();
        outMessage._type = ViperMessageType::GatewayStatus;
        GatewayStatusMessage& message = reinterpret_cast<GatewayStatusMessage &> (outMessage._data[0]);
        message._active = _gatewayStatus._active;
        if (_gatewayStatus._message.empty()) {
            strcpy(message._message, _gatewayStatus._message.c_str());
        }
        const auto outIdx = _outToFront->pool()->append(sizeof(ViperMessageType) + sizeof(GatewayStatusMessage));
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
            _logger.logInfo(boost::str(boost::format("Received Order Accepted when logged out.") % event.oid));
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            if (isRvHedge(order)) {
                auto& executor = getContractExecutor(order);
                _logger.logInfo(boost::str(boost::format("Hedge order rejected. Contract=%1% OID=%2% Reason=%3% ReasonText=%4%") % executor._name % order.oid % event.reasonCodeExt % event.reasonText));
                disableSystem("Hedge order rejected");
            } else {
                auto& executor = getContractExecutor(order);
                executor.onOrderRejected(event, order);
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Rejected when logged out.") % event.oid));
        }
    }
    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            auto& executor = getContractExecutor(order);
            executor.onOrderReplaced(event, order);
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Replaced when logged out.") % event.oid));
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            auto& executor = getContractExecutor(order);
            executor.onOrderReplaceRejected(event, order);
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Replace rejected when logged out.") % event.oid));
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) override {
        if (isLoggedIn()) {
            if (!isRvHedge(order)) {
                auto& executor = getContractExecutor(order);
                executor.onOrderExecution(event, order);
            } else {
                auto& executor = getContractExecutor(order);
                uint32_t side = order.side == hf::SIDE::BUY ? 0 : 1;
                sendFillToFront(executor._internalMatchingEngine._internalId, side, event.filledPrice, event.filledQty, StrategyId::RelativeValue, FillType::External, FillSource::Hedge);
                _logger.logInfo(boost::str(boost::format("Hedge order filled. Contract=%1% OID=%2%") % executor._name % order.oid));
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Executed when logged out.") % event.oid));
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) override {
        if (isLoggedIn()) {
            if (isRvHedge(order)) {
                auto& executor = getContractExecutor(order);
                _logger.logInfo(boost::str(boost::format("Hedge order cancelled. Contract=%1% OID=%2%") % executor._name % order.oid));
                sendReverseFillToRelativeValue(getInternalId(order), order);
            } else {
                auto& executor = getContractExecutor(order);
                executor.onOrderCanceled(event, order);
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Cancelled when logged out.") % event.oid));
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) override {
        if (isLoggedIn()) {
            auto& executor = getContractExecutor(order);
            executor.onOrderCancelRejected(event, order);
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Cancel rejected when logged out.") % event.oid));
        }
    }

    void onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::order_t &order) {}
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event) {}
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event) {}


    void cancelOrder(uint32_t internalId, om::order_t* order) {
        this->context().cancelOrder(order->oid);
        ContractStateT& state = _contractStates[internalId];
        ++state._contractExecutor._messages._cancels;
    }

    void modifyOrder(uint32_t internalId, const om::order_t* order, hf::price_t price, hf::qty_t size) {
        this->context().modifyOrder(order->oid, price, size);
        ContractStateT& state = _contractStates[internalId];
        ++state._contractExecutor._messages._modifies;
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        ContractStateT& state = _contractStates[internalId];
        om::order_t* order = this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._contractExecutor._rid, state._marketMaker._contract._sid, om::ORDER_TYPE::LMT, ToRhoneSide(side), price, size);
        if (order == nullptr) {
            _logger.logInfo(boost::str(boost::format("Could not send limit order. Contract=%1% Side=%2% Price=%3% Size=%4%") % state._contractExecutor._name % GetSideString(side) % price % size));
        } else {
            _logger.logInfo(boost::str(boost::format("Sent limit order. Contract=%1% Side=%2% Price=%3% Size=%4% OID=%5%") % state._contractExecutor._name % GetSideString(side) % price % size % order->oid));
            order->debug.i[INTERNAL_ID] = internalId;
            ++state._contractExecutor._messages._inserts;
        }
        return order;
    }

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        ContractStateT& state = _contractStates[internalId];
        om::order_t* order =  this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._contractExecutor._rid, state._marketMaker._contract._sid, om::ORDER_TYPE::FAK, ToRhoneSide(side), price, size);
        if (order == nullptr) {
            _logger.logInfo(boost::str(boost::format("Could not send FAK order. Contract=%1% Side=%2% Price=%3% Size=%4%") % state._contractExecutor._name % GetSideString(side) % price % size));
        } else {
            _logger.logInfo(boost::str(boost::format("Sent FAK order. Contract=%1% Side=%2% Price=%3% Size=%4% OID=%5%") % state._contractExecutor._name % GetSideString(side) % price % size % order->oid));
            order->debug.i[INTERNAL_ID] = internalId;
            ++state._contractExecutor._messages._inserts;
        }
        return order;
    }

    void onCriticalError(const std::string& reason) {
        std::string message = boost::str(boost::format("Critical Error: %1%") % reason);
        disableSystemViaShutdown(reason);
    }

    std::vector<ContractStateT> _contractStates;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;

    ViperQueue* _inFromFront;
    ViperQueue* _inFromRV;
    ViperQueue* _outToRV;
    ViperQueue* _outToFront;

    SharedSystemState* _systemState;
    SystemState _executorOldSystemState = SystemState::LoggedOut;
    SystemState _relativeValueOldSystemState = SystemState::LoggedOut;

    GatewayStatus _gatewayStatus;

    ViperConfig* _config;
    double _frequency;

    uint64_t _bannerExpiryPeriod = 0;
    uint64_t _bannerExpiryTime = 0;

    Logger _logger;
};

typedef Executor<::strat::Strategy> ExecutorT;

}

#endif //VIPER_EXECUTOR_H
