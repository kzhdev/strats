#ifndef RHONEOMS_VIPER_H
#define RHONEOMS_VIPER_H

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
#include "RelativeValueConfig.h"

namespace Python {

struct ContractState {
    SimpleMarketMaker _marketMaker;
    Book _books[2];
};

template<typename StrategyT>
class Viper : public StrategyT {

    Viper() {
        auto inPool = new ViperPool();
        inPool->create();
        _inQueue = new ViperQueue();
        _inQueue->create();
        _inQueue->set_pool(inPool);

        auto outPool = new ViperPool();
        outPool->create();
        _outQueue = new ViperQueue();
        _outQueue->create();
        _outQueue->set_pool(outPool);
    }

    void onStratBegin() override {
    }

    void onStratEnd() override {
    }

    void onPostCreate(const libconfig::Setting& config) override {
        const auto name = hf::ConfigManager::getString(config, "config_file");
        _config.initialize(name);
        const std::string relativeValueName = config["relativeValueName"];
        auto relativeValue = dynamic_cast<RelativeValue*> (hf::ResourceManager::getResourceP(frontName));
        relativeValue->init(_inQueue, _outQueue, _config);
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
        ContractState& state = _contractStates[event.sid];
        const bool bidChanged = event.buy.updatedQtyNum == 1;
        const bool askChanged = event.sell.updatedQtyNum == 1;
        copyBooks(state, event, bidChanged, askChanged);
        if (bidChanged | askChanged) {
            state._marketMaker.onChangeInOtherBooks(state._books[BID], state._books[ASK], bidChanged, askChanged);
        }
    }

    void copyBooks(ContractState& state, const md::MDUpdateEvent& event, bool bidChanged, bool askChanged) {
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

    // ************** Order functions **************** //

    void onOrderAccepted(const om::OMOrderAcceptedEvent& event, const om::order_t& order) override {
    }

    void onOrderRejected(const om::OMOrderRejectedEvent& event, const om::order_t& order) override {
    }

    void onOrderReplaced(const om::OMOrderModifiedEvent& event, const om::order_t& order) override {
    }

    void onOrderReplaceRejected(const om::OMOrderModifyRejectedEvent& event, const om::order_t& order) override {
    }

    void onOrderCanceled(const om::OMOrderCanceledEvent& event, const om::order_t& order) override {
    }

    void onOrderExecution(const om::OMExecutionEvent &event, const om::order_t &order) override {
    }

    void onOrderCancelRejected(const om::OMOrderCancelRejectedEvent &event, const om::order_t &order) override {
    }

    void onOrderSpreadLegExecution(const om::OMSpreadLegExecutionEvent &event, const om::order_t &order) {}
    void onMassCancelAccepted(const om::OMMassCancelAcceptedEvent &event) {}
    void onMassCancelRejected(const om::OMMassCancelRejectedEvent &event) {}

    //  **************** End of Order functions *****************//

    std::vector<ContractState> _contractStates;
    ViperQueue* _inQueue;
    ViperQueue* _outQueue;
    RelativeValueConfig _config;
};

typedef Viper<::strat::Strategy> ViperT;

}

#endif //RHONEOMS_VIPER_H
