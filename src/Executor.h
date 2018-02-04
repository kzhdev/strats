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

    void onQuoteOrderFromRv(QuoteOrderMessage& order) {
        ContractStateT& state = _contractStates[order._instrumentIndex];
        state._contractExecutor.onQuoteOrderFromRelativeValue(order);
    }

    void onInternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::Internal);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.onInternalFill(order, price, size);
                break;
            }
            default:
                break;
        }
    }

    void onExternalFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                sendFillToRV(internalId, order->_side, price, size, StrategyId::RelativeValue, FillType::External);
                break;
            }
            case Order::Strategy::MarketMaker: {
                ContractStateT& state = _contractStates[internalId];
                state._marketMaker.onExternalFill(order, price, size);
                break;
            }
            default:
                break;
        }
    }

    void sendFillToFront(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size, StrategyId strategyId, FillType type) {
        auto &outMessage = _outToFront->pool()->getNextMessage();
        outMessage._type = ViperMessageType::Fill;
        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractId = contractIndex;
        message._side = side;
        message._qty = size;
        message._strategy = strategyId;
        message._type = type;
        message._price = price;
        const auto outIdx = _outToFront->pool()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void onEndOfExternalFills(uint32_t internalId) {
        ContractStateT& state = _contractStates[internalId];
        state._marketMaker.onEndOfExternalFills();
    }

    void onHedgeOrderFromRv(HedgeOrderMessage& order) {
        _logger.logInfo(boost::str(boost::format("MM received hedge order from RV. Contract=%1% Side=%2% Price=%3% Size=%4%") % _config->_allContracts[order._instrumentIndex]._name %
                                                                           getSideString(order._side) % order._price % order._size));
        ContractStateT& state = _contractStates[order._instrumentIndex];
        if (order._isExternal) {
            sendExternalRvHedge(state, order);
        } else {
            const hf::qty_t matchedSize = state._contractExecutor.onHedgeOrderFromRelativeValue(order);
            if (matchedSize != order._size) {
                const hf::qty_t unmatchedSize = order._size - matchedSize;
                const uint32_t otherSide = GetOtherSide(order._side);
                sendFillToRV(order._instrumentIndex, otherSide, order._price, unmatchedSize, StrategyId::RelativeValue, FillType::External);
            }
        }
    }

    void sendExternalRvHedge(ContractStateT& state, HedgeOrderMessage& order) {
        _logger.logInfo("Sending externally");
        om::order_t* o = newFAKOrder(order._instrumentIndex, order._side, order._price, order._size);
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

    void sendUnfilledSizeToRv(uint32_t internalId, const om::order_t& order) {
        const hf::qty_t unfilledSize = order.leavesQty;
        const uint32_t side = order.side == hf::SIDE::BUY ? 0 : 1;
        sendFillToRV(internalId, side, order.pr, unfilledSize, StrategyId::RelativeValue, FillType::External);
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
                case ViperMessageType::GuiInitialized: {
                    handleGuiInitialized();
                    break;
                }
                case ViperMessageType::Logout: {
                    bool out = handleLogout();
                    sendGenericMessageToFront(ViperMessageType::LogoutReply, out);
                    break;
                }
                case ViperMessageType::GetOutOfMarket: {
                    handleGetOutOfMarket();
                    break;
                }
                default:
                    break;
            }
        }
    }

    bool handleLogin() {
        _logger.logInfo("Market makers logging in.");
        _systemState = _newSystemState = SystemState::LoggedIn;
        bool success = true;
        for (auto& state : _contractStates) {
            success &= state._marketMaker.handleLogin();
        }
        return true;
    }

    bool handleLogout() {
        _logger.logInfo("Market makers logging out.");
        _systemState = _newSystemState = SystemState::Disabled;
        handleDisabled();
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

    void handleSwitchChange(GenericMessage& value) {
        MainControlAction action = static_cast<MainControlAction>(value._number_1);
        bool enabled = 1 == value._number_2;
        if (action == MainControlAction::Enabled) {
            if (enabled) {
                _systemState = SystemState::Enabled;
                handleEnabled();
            } else {
                _systemState = SystemState::Disabled;
                handleDisabled();
            }
        } else if (action == MainControlAction::Quoting) {
            if (enabled) {
                handleQuotingEnabled();
            } else {
                handleQuotingDisabled();
            }
        }
    }

    void init(ViperConfig& viperConfig, ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromRV, ViperQueue* outToRV) {
        initQueues(inFromFront, outToFront, inFromRV, outToRV);
        _config = &viperConfig;
    }

    void initQueues(ViperQueue* inFromFront, ViperQueue* outToFront, ViperQueue* inFromRV, ViperQueue* outToRV) {
        _inFromFront = inFromFront;
        _outToFront = outToFront;
        _inFromRV = inFromRV;
        _outToRV = outToRV;
    }

    void initialize(ViperConfig& viperConfig) {
        _contractStates.resize(viperConfig._allContracts.size());

        for (uint32_t i = 0; i < viperConfig._allContracts.size(); ++i) {
            ConfigContract& contract = viperConfig._allContracts[i];
            auto& state = _contractStates[i];
            uint32_t index;
            bool toMarketMake = viperConfig.isContractToMarketMake(contract._name, index);
            if (toMarketMake) {
                StackerConfig& stackerConfig = viperConfig._stackerConfigs[index];
                MarketMakerConfig& mmConfig = viperConfig._marketMakerConfigs[index];
                auto& pc = viperConfig._pricingConfigs[i];
                auto rid = this->context().getRoutingId(contract._venue, contract._sid);
                state._contractExecutor.initialize(contract, rid, stackerConfig, this);
                state._marketMaker.initialize(contract, pc._establishedLevel, mmConfig, &state._contractExecutor);
            }
            subscribeAll(hf::VENUE::ICE, contract._sid);
        }
    }

    void subscribeAll(hf::VENUE venue, hf::symbol_id_t sid) {
        this->context().subscribeMD(venue, sid, 1, true, true);
        this->context().subscribeOM(sid, true, true, true, true, true, true, true, true);
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
            default:
                break;
        }
    }

    void handleHardShutdown() {
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateHardShutdown);
        hf::Service::exitAll();
    }

    void disableAll(const std::string& reason) {
        std::string message("Disable All. Reason: ");
        message.append(reason);
        _logger.logInfo(message);
        _systemState = _newSystemState = SystemState::Disabled;
        sendGenericMessageToFront(ViperMessageType::DisableAll);
        handleDisabled();
    }

    void handleDisabled() {
        disableMarketMakers();
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateDisabled);
    }

    void disableMarketMakers() {
        for (auto& state : _contractStates) {
            state._marketMaker.disable();
        }
        _logger.logInfo("MM got disabled");
    }

    void handleEnabled() {
        for (auto& state : _contractStates) {
            state._marketMaker.enable(state._books[BID], state._books[ASK]);
        }
        _logger.logInfo("MM got enabled");
        sendGenericMessageToFront(ViperMessageType::SwitchChangeReply, toInt(MainControlAction::Enabled), proto::SystemState::SystemStateEnabled);
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

    void handleGetOutOfMarket() {
        for (auto& state : _contractStates) {
            state._contractExecutor.getOutOfMarket();
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
                state._marketMaker.onChangeInOtherBooks(state._books[BID], state._books[ASK], bidChanged, askChanged);
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
    }

    void onMDGap(hf::VENUE venue, const md::MDGapEvent& event) {
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
        if (_systemState != SystemState::LoggedOut) {
            sendGatewayStatusToFront();
        }
        if (!_gatewayStatus._active && _systemState == SystemState::Enabled) {
            disableAll("Gateway down");
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

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) override {
        if (_systemState != SystemState::LoggedOut) {
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
        if (_systemState != SystemState::LoggedOut) {
            if (isRvHedge(order)) {
                sendUnfilledSizeToRv(getInternalId(order), order);
            } else {
                auto& executor = getContractExecutor(order);
                executor.onOrderRejected(event, order);
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Rejected when logged out.") % event.oid));
        }
    }
    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) override {
        if (_systemState != SystemState::LoggedOut) {
            auto& executor = getContractExecutor(order);
            executor.onOrderReplaced(event, order);
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Replaced when logged out.") % event.oid));
        }
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) override {
        if (_systemState != SystemState::LoggedOut) {
            auto& executor = getContractExecutor(order);
            executor.onOrderReplaceRejected(event, order);
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Replace rejected when logged out.") % event.oid));
        }
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) override {
        if (_systemState != SystemState::LoggedOut) {
            if (!isRvHedge(order)) {
                auto& executor = getContractExecutor(order);
                executor.onOrderExecution(event, order);
            } else {
                _logger.logInfo("RV Hedge filled");
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Executed when logged out.") % event.oid));
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) override {
        if (_systemState != SystemState::LoggedOut) {
            if (isRvHedge(order)) {
                sendUnfilledSizeToRv(getInternalId(order), order);
            } else {
                auto& executor = getContractExecutor(order);
                executor.onOrderCanceled(event, order);
            }
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Cancelled when logged out.") % event.oid));
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) override {
        if (_systemState != SystemState::LoggedOut) {
            auto& executor = getContractExecutor(order);
            executor.onOrderCancelRejected(event, order);
        } else {
            _logger.logInfo(boost::str(boost::format("Received Order Cancel rejected when logged out.") % event.oid));
        }
    }

    void onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::order_t &order) {}
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event) {}
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event) {}


    void cancelOrder(om::order_t* order) {
        if (_systemState == SystemState::Enabled) {
            this->context().cancelOrder(order->oid);
        }
    }

    void modifyOrder(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        if (_systemState == SystemState::Enabled) {
            this->context().modifyOrder(order->oid, price, size);
        }
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        _logger.logError("Sending limit order");
        if (_systemState == SystemState::Enabled) {
            ContractStateT& state = _contractStates[internalId];
            om::order_t* order = this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._contractExecutor._rid, state._marketMaker._contract._sid, om::ORDER_TYPE::LMT, ToRhoneSide(side), price, size);
            if (order == nullptr) {
                _logger.logError("Error sending new limit order. - Order could not be created.");
            } else {
                order->debug.i[INTERNAL_ID] = internalId;
            }
            return order;
        }
        _logger.logError("Error sending new limit order - system not enabled.");
        return nullptr;
    }

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        if (_systemState == SystemState::Enabled) {
            ContractStateT& state = _contractStates[internalId];
            om::order_t* order =  this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._contractExecutor._rid, state._marketMaker._contract._sid, om::ORDER_TYPE::FAK, ToRhoneSide(side), price, size);
            if (order == nullptr) {
                _logger.logError("Error sending new FAK. - Order could not be created.");
            } else {
                order->debug.i[INTERNAL_ID] = internalId;
            }
            return order;
        }
        _logger.logError("Error sending new FAK - system not enabled.");
        return nullptr;
    }

    void onCriticalError(const std::string& message) {
        std::string m;
        m.append("Shutting down: ").append(message);
        sendErrorMessage(m, ErrorType::Critical);
        usleep(50000);
        _logger.logError("Shutting down - hard");
        handleHardShutdown();
    }

    std::vector<ContractStateT> _contractStates;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;

    ViperQueue* _inFromFront;
    ViperQueue* _inFromRV;
    ViperQueue* _outToRV;
    ViperQueue* _outToFront;

    SystemState _systemState = SystemState::LoggedOut;
    SystemState _newSystemState = SystemState::LoggedOut;

    GatewayStatus _gatewayStatus;

    ViperConfig* _config;

    Logger _logger;
};

typedef Executor<::strat::Strategy> ExecutorT;

}

#endif //VIPER_EXECUTOR_H
