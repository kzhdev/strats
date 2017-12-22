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
    hf::symbol_id_t _sid;
    om::routing_id_t _rid;
    SimpleMarketMaker<T> _marketMaker;
    Book _books[2];
};

template<typename StrategyT>
class MarketMakers : public StrategyT {

public:
    typedef ContractState<MarketMakers> ContractStateT;

    MarketMakers() {
    }

    void onConfig(ViperConfig& viperConfig) {
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
        processMessagesFromFront();
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

    void onFill(uint32_t internalId, Order* order, hf::price_t price, hf::qty_t size) {
        switch (order->_strategy) {
            case Order::Strategy::RelativeValue: {
                sendFillToRV(internalId, order->_side, price, size);
                break;
            }
            case Order::Strategy::MarketMaker: {
                break;
            }
            default:
                break;
        }
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
                sendFillToRV(order._instrumentIndex, otherSide, order._price, unmatchedSize);
            }
        }
    }

    void sendExternalRvHedge(ContractStateT& state, HedgeOrderMessage& order) {
        om::order_t* o = newFAKOrder(order._instrumentIndex, order._side, order._price, order._size);
        o->debug.i[INTERNAL_ID] = order._instrumentIndex;
    }

    void sendFillToRV(uint32_t contractIndex, uint32_t side, hf::price_t price, hf::qty_t size) {
        auto &outMessage = _outToRV->pool()->getNextMessage();
        outMessage._type = ViperMessageType::Fill;

        FillMessage& message = reinterpret_cast<FillMessage &> (outMessage._data[0]);
        message._contractIndex = contractIndex;
        message._side = side;
        message._price = price;
        message._size = size;

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
        sendFillToRV(internalId, side, order.pr, unfilledSize);
    }

    void processMessagesFromFront() {

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
        _contractStates.resize(viperConfig._stackerConfigs.size());

        for (uint32_t i = 0; i < _contractStates.size(); ++i) {
            ContractStateT& state = _contractStates[i];
            StackerConfig& stackerConfig = viperConfig._stackerConfigs[i];
            ConfigContract& configContract = viperConfig._allContracts[i];
            state._sid = configContract._sid;
            state._rid = this->context().getRoutingId(configContract._venue, configContract._sid);
            ImConfig imConfig;
            imConfig._numberOfLevels = stackerConfig._numberOfLevels;
            imConfig._quoteSize = stackerConfig._quoteSize;
            imConfig._internalId = i;
            imConfig._tickSize = configContract._tickSize;
            state._marketMaker.initialize(imConfig, this);
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
    }

    void handleDisabled() {
    }

    void handleEnabled() {
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
            Stacker<MarketMakers>& stacker = getStacker(order);
            stacker.onOrderExecution(event, order);
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
            return this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._rid, state._sid, om::ORDER_TYPE::LMT, ToRhoneSide(side), price, size);
        }
        return nullptr;
    }

    om::order_t* newFAKOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        if (_systemState == SystemState::Enabled) {
            ContractStateT& state = _contractStates[internalId];
            return this->context().createOrder(0, om::RISK_TYPE::UNKNOWN, state._rid, state._sid, om::ORDER_TYPE::FAK, ToRhoneSide(side), price, size);
        }
        return nullptr;
    }

    std::vector<ContractStateT> _contractStates;
    std::unordered_map<hf::symbol_id_t, uint32_t> _sidToInternalId;

    ViperQueue* _inFromFront;
    ViperQueue* _inFromRV;
    ViperQueue* _outToRV;
    ViperQueue* _outToFront;

    SystemState _systemState;
    SystemState _newSystemState;

    ViperConfig* _config;

    Logger _logger;
};

typedef MarketMakers<::strat::Strategy> MarketMakersT;

}

#endif //VIPER_MARKET_MAKERS_H
