#ifndef STACKER_FIXTURE_H
#define STACKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Stacker.h"

namespace Python {

struct Executor {
    void cancelOrder(om::order_t* order) {
        order->is_cancelling = true;
        _cancels.push_back(order);
    }

    void clearCancels() {
        _cancels.clear();
    }

    void modifyOrder(const om::order_t* order, hf::price_t price, hf::qty_t size) {
        om::order_t* mutableOrder = const_cast<om::order_t*>(order);
        mutableOrder->pr = price;
        mutableOrder->qty = size;
        mutableOrder->is_modifying = true;
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size) {
        om::order_t *order = new om::order_t();
        order->sid = 1;
        order->is_acked = false;
        order->is_active = true;
        order->side = ToRhoneSide(side);
        order->pr = price;
        order->qty = size;
        order->oid = ++_orderId;
        order->ot = om::ORDER_TYPE::LMT;
        order->venue = hf::VENUE::ICE;
        order->leavesQty = size;
        order->executedQty = 0;
        _orders.insert(std::pair<om::order_id_t, om::order_t *>(order->oid, order));
        ++_orderCount;
        return order;
    }

    void resetOrderCount() {
        _orderCount = 0;
    }

    uint32_t _orderId = 0;
    std::unordered_map<om::order_id_t, om::order_t *> _orders;
    uint32_t _orderCount = 0;
    std::vector<om::order_t*> _cancels;
};

class StackerFixture : public ::testing::Test {
protected:
    StackerFixture() { }

    void SetUp() override {
        _stacker = new Stacker<Executor>();
        _executor = new Executor();
    }

    void TearDown() override {
    }

    void onOrderAccepted(hf::qty_t size, const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        om::OMOrderAcceptedEvent orderAck;
        orderAck.acceptedQty = size;
        mutableOrder.is_acked = true;
        _stacker->onOrderAccepted(orderAck, order);
    }

    void onOrderReplaced(hf::qty_t size, const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        om::OMOrderModifiedEvent modifyAck;
        modifyAck.acceptedQty = size;
        _stacker->onOrderReplaced(modifyAck, order);
    }

    void onOrderCanceled(const om::order_t& order) {
        om::OMOrderCanceledEvent event;
        _stacker->onOrderCanceled(event, order);
    }

    void onOrderExecuted(const om::order_t& order, hf::price_t price, hf::qty_t size, bool active) {
        om::OMExecutionEvent event;
        event.filledPrice = price;
        event.filledQty = size;
        event.totalExecutedQty = order.executedQty + size;
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.is_active = active;
        _stacker->onOrderExecution(event, order);
    }

    Stacker<Executor>* _stacker;
    Executor* _executor;
};

}

#endif //STACKER_FIXTURE_H
