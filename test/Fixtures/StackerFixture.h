#ifndef STACKER_FIXTURE_H
#define STACKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Stacker.h"

namespace Python {

struct Executor {
    void cancelOrder(om::order_t* order) {
        order->is_cancelling = true;
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
        _orders.insert(std::pair<om::order_id_t, om::order_t *>(order->oid, order));
        return order;
    }

    uint32_t _orderId = 0;
    std::unordered_map<om::order_id_t, om::order_t *> _orders;
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

    Stacker<Executor>* _stacker;
    Executor* _executor;
};

}

#endif //STACKER_FIXTURE_H
