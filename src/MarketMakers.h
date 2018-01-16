#ifndef VIPER_MARKET_MAKERS_H
#define VIPER_MARKET_MAKERS_H

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
#include <md/queue/MDQueue.h>

namespace Python {

template <typename T>
struct ContractState {
    SimpleMarketMaker<T> _marketMaker;
    Book _books[2];
};

template<typename StrategyT>
class MarketMakers : public StrategyT {

public:
    typedef ContractState<MarketMakers> ContractStateT;

    MarketMakers() {
    }

    void onStratBegin() override {
        resetQueues();
        initializeMarketMakers(*_config);
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
        state._marketMaker.onQuoteOrderFromRv(order);
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
        const auto outIdx = _outToFront->pool()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToFront->appendNew(outIdx);
    }

    void onEndOfExternalFills(uint32_t internalId) {
        ContractStateT& state = _contractStates[internalId];
        state._marketMaker.onEndOfExternalFills();
    }

    void onHedgeOrderFromRv(HedgeOrderMessage& order) {
        ContractStateT& state = _contractStates[order._instrumentIndex];
        if (order._isExternal) {
            sendExternalRvHedge(state, order);
        } else {
            const hf::qty_t matchedSize = state._marketMaker.onHedgeOrderFromRv(order);
            if (matchedSize != order._size) {
                const hf::qty_t unmatchedSize = order._size - matchedSize;
                const uint32_t otherSide = GetOtherSide(order._side);
                sendFillToRV(order._instrumentIndex, otherSide, order._price, unmatchedSize, StrategyId::RelativeValue, FillType::External);
            }
        }
    }

    void sendExternalRvHedge(ContractStateT& state, HedgeOrderMessage& order) {
        om::order_t* o = newFAKOrder(order._instrumentIndex, order._side, order._price, order._size);
        o->debug.i[INTERNAL_ID] = order._instrumentIndex;
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
        const auto outIdx = _outToRV->pool()->append(sizeof(ViperMessageType) + sizeof(FillMessage));
        mb();
        _outToRV->appendNew(outIdx);
    }

    Stacker<MarketMakers>& getStacker(const om::order_t& order) {
        ContractStateT& state = _contractStates[getInternalId(order)];
        if (order.side == hf::SIDE::BUY) {
            return state._marketMaker._internalMatchingEngine._bidStacker;
        } else {
            return state._marketMaker._internalMatchingEngine._askStacker;
        }
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
        _logger.logInfo("Market makers handling client down.");
        _systemState = _newSystemState = SystemState::Disabled;
        disableMarketMakers();
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

    void sendCriticalMessage(const std::string& s) {
        auto &outMessage = _outToFront->pool()->getNextMessage();
        outMessage._type = ViperMessageType::CriticalMessage;

        CriticalMessage& message = reinterpret_cast<CriticalMessage &> (outMessage._data[0]);
        const uint32_t size = std::min(CRITICAL_MESSAGE_MAX_SIZE, (uint32_t) s.size());
        strncpy(message._message, s.c_str(), size);
        message._size = size;

        const auto outIdx = _outToFront->pool()->append(sizeof(ViperMessageType) + sizeof(CriticalMessage));
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

    void initializeMarketMakers(ViperConfig& viperConfig) {
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
                state._marketMaker.initialize(contract, rid, pc._establishedLevel, mmConfig, stackerConfig, this);
            }
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
            default:
                break;
        }
    }

    void handleHardShutdown() {
        hf::Service::exitAll();
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

    void onMDBatch(const md::MDBatch& batch) override {
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
        book._otherQuantity = topLevel.q - topLevel._ourQuantity;
        book._otherOrderCount = topLevel.o - topLevel._ourOrderCount;
        book._ourQuantity = topLevel._ourQuantity;
        book._ourOrderCount = topLevel._ourOrderCount;
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
                    const_cast<om::order_t &> (order).is_active = event.is_active;
                    const_cast<om::order_t &> (order).executedQty = event.totalExecutedQty;
                    onOrderExecution(event, order);
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
    }

    void onOMPortStatusUpdate(const om::PortStatusUpdate& event) override {
    }

    static const uint32_t INTERNAL_ID = 0;

    bool isRvHedge(const om::order_t& order) {
        return order.ot == om::ORDER_TYPE::FAK;
    }

    uint32_t getInternalId(const om::order_t& order) {
        return order.debug.i[INTERNAL_ID];
    }

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) override {
        if (!isRvHedge(order)) {
            Stacker<MarketMakers>& stacker = getStacker(order);
            stacker.onOrderAccepted(event, order);
        }
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) override {
        if (isRvHedge(order)) {
            sendUnfilledSizeToRv(getInternalId(order), order);
        } else {
            Stacker<MarketMakers>& stacker = getStacker(order);
            stacker.onOrderRejected(event, order);
        }
    }
    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) override {
        Stacker<MarketMakers>& stacker = getStacker(order);
        stacker.onOrderReplaced(event, order);
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) override {
        Stacker<MarketMakers>& stacker = getStacker(order);
        stacker.onOrderReplaceRejected(event, order);
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) override {
        if (!isRvHedge(order)) {
            ContractStateT& state = _contractStates[getInternalId(order)];
            state._marketMaker._internalMatchingEngine.onOrderExecution(event, order);
        } else {
            _logger.logInfo("RV Hedge filled");
        }
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) override {
        if (isRvHedge(order)) {
            sendUnfilledSizeToRv(getInternalId(order), order);
        } else {
            Stacker<MarketMakers>& stacker = getStacker(order);
            stacker.onOrderCanceled(event, order);
        }
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) override {
        Stacker<MarketMakers>& stacker = getStacker(order);
        stacker.onOrderCancelRejected(event, order);
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
        if (_systemState == SystemState::Enabled) {
            ContractStateT& state = _contractStates[internalId];
            return this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._marketMaker._rid, state._marketMaker._contract._sid, om::ORDER_TYPE::LMT, ToRhoneSide(side), price, size);
        }
        return nullptr;
    }

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        if (_systemState == SystemState::Enabled) {
            ContractStateT& state = _contractStates[internalId];
            return this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._marketMaker._rid, state._marketMaker._contract._sid, om::ORDER_TYPE::FAK, ToRhoneSide(side), price, size);
        }
        return nullptr;
    }

    std::vector<ContractStateT> _contractStates;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;

    ViperQueue* _inFromFront;
    ViperQueue* _inFromRV;
    ViperQueue* _outToRV;
    ViperQueue* _outToFront;

    SystemState _systemState = SystemState::LoggedOut;
    SystemState _newSystemState = SystemState::LoggedOut;;

    ViperConfig* _config;

    Logger _logger;
};

typedef MarketMakers<::strat::Strategy> MarketMakersT;

}

#endif //VIPER_MARKET_MAKERS_H
