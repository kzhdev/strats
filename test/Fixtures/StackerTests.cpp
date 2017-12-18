#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "StackerFixture.h"

using ::testing::_;

namespace Python {

TEST_F(StackerFixture, StackerTests) {
    hf::price_t tickSize = ToRhonePrice(0.25);
    uint32_t nLevels = 4;
    hf::qty_t quoteSize = 20;
    hf::qty_t topSize = 3;
    _stacker->initialize(1, BID, tickSize, quoteSize, nLevels, _executor);
    hf::price_t price = ToRhonePrice(100);
    _stacker->stackBid(price, topSize);

    // 99.75, 99.5, 99.25, 99, 98.75 - 0
    // 100 - 4
    // 100.25, 100.5, 100.75, 101 - 20
    // 101.25, 101.5, 101.75, 102, 102.25 - 0

    auto topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_orders.size());
    auto order = topLevel->_orders[0];
    ASSERT_EQ(price, order->pr);
    ASSERT_EQ(topSize, order->qty);

    hf::price_t p = price;
    auto level = topLevel;
    for (uint32_t i = 0; i < NUMBER_OF_PRE_CREATED_LEVELS; ++i) {
        level = level->before;
        ASSERT_TRUE(level != nullptr);
        p = GetNextPrice<BID>(p, tickSize);
        ASSERT_EQ(p, level->_price);
        ASSERT_EQ(0, level->_target);
        ASSERT_EQ(0, level->_orders.size());
    }

    level = topLevel;
    p = price;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        ASSERT_TRUE(level != nullptr);
        p = GetPriorPrice<BID>(p, tickSize);
        ASSERT_EQ(p, level->_price);
        ASSERT_EQ(quoteSize, level->_target);
        ASSERT_EQ(1, level->_orders.size());
        order = level->_orders[0];
        ASSERT_EQ(p, order->pr);
        ASSERT_EQ(quoteSize, order->qty);
    }

    for (uint32_t i = 0; i < NUMBER_OF_PRE_CREATED_LEVELS; ++i) {
        level = level->after;
        ASSERT_TRUE(level != nullptr);
        p = GetPriorPrice<BID>(p, tickSize);
        ASSERT_EQ(p, level->_price);
        ASSERT_EQ(0, level->_target);
        ASSERT_EQ(0, level->_orders.size());
    }

    ASSERT_EQ(5, _executor->_orders.size());

    _executor->resetOrderCount();

    topSize = 2;
    _stacker->stackBid(price, topSize);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(3, topLevel->_flight);
    ASSERT_EQ(0, topLevel->_acknowledged);
    ASSERT_EQ(0, _stacker->_downFlight);
    ASSERT_EQ(1, topLevel->_orders.size());

    onOrderAccepted(3, *topLevel->_orders[0]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(-1, topLevel->_flight);
    ASSERT_EQ(3, topLevel->_acknowledged);
    ASSERT_EQ(1, _stacker->_downFlight);
    ASSERT_EQ(1, topLevel->_orders.size());

    topSize = 5;
    _stacker->stackBid(price, topSize);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_flight);
    ASSERT_EQ(3, topLevel->_acknowledged);
    ASSERT_EQ(1, _stacker->_downFlight);
    ASSERT_EQ(2, topLevel->_orders.size());

    order = topLevel->_orders[1];
    ASSERT_EQ(price, order->pr);
    ASSERT_EQ(2, order->qty);

    om::OMOrderModifiedEvent modifyAck;

    _stacker->onOrderReplaced(modifyAck, *topLevel->_orders[0]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(3, topLevel->_flight);
    ASSERT_EQ(2, topLevel->_acknowledged);
    ASSERT_EQ(0, _stacker->_downFlight);
    ASSERT_EQ(3, topLevel->_orders.size());

    onOrderAccepted(2, *topLevel->_orders[1]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_flight);
    ASSERT_EQ(4, topLevel->_acknowledged);
    ASSERT_EQ(0, _stacker->_downFlight);
    ASSERT_EQ(3, topLevel->_orders.size());

    onOrderAccepted(1, *topLevel->_orders[2]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(0, topLevel->_flight);
    ASSERT_EQ(5, topLevel->_acknowledged);
    ASSERT_EQ(0, _stacker->_downFlight);
    ASSERT_EQ(3, topLevel->_orders.size());

    // 1) price -> 99.75 (level below)  /  qty - 4

    topSize = 4;
    price = GetPriorPrice<BID>(price, tickSize);

    auto nextLevel = topLevel->after;

    ASSERT_EQ(price, nextLevel->_price);
    ASSERT_EQ(quoteSize, nextLevel->_target);
    ASSERT_EQ(20, nextLevel->_flight);
    ASSERT_EQ(0, nextLevel->_acknowledged);
    ASSERT_EQ(0, _stacker->_downFlight);
    ASSERT_EQ(1, nextLevel->_orders.size());

    onOrderAccepted(20, *nextLevel->_orders[0]);

    _stacker->stackBid(price, topSize);
    topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(-16, topLevel->_flight);
    ASSERT_EQ(20, topLevel->_acknowledged);
    ASSERT_EQ(16, _stacker->_downFlight);
    ASSERT_EQ(1, topLevel->_orders.size());
}

}