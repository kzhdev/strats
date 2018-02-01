#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "StackerFixture.h"

using ::testing::_;

namespace Python {

TEST_F(StackerFixture, Test1) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _stacker->initialize(1, ASK, tickSize, quoteSize, nLevels, _executor, "name", true);

    hf::price_t price = ToRhonePrice(.35);
    hf::qty_t topSize = 50;
    _stacker->stackAsk(price, topSize);

    auto topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_orders.size());
    auto order = topLevel->_orders[0];
    ASSERT_EQ(price, order->pr);
    ASSERT_EQ(topSize, order->qty);

    onOrderAccepted(3, *topLevel->_orders[0]);

    auto level = topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        onOrderAccepted(20, *level->_orders[0]);
    }

    price = ToRhonePrice(.28);
    topSize = 50;
    _stacker->stackAsk(price, topSize);
}

TEST_F(StackerFixture, Test2) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _stacker->initialize(1, ASK, tickSize, quoteSize, nLevels, _executor, "name", true);

    hf::price_t price = ToRhonePrice(1.27);
    hf::qty_t topSize = 10;
    _stacker->stackAsk(price, topSize);

    auto topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_orders.size());
    auto order = topLevel->_orders[0];
    ASSERT_EQ(price, order->pr);
    ASSERT_EQ(topSize, order->qty);

    onOrderAccepted(3, *topLevel->_orders[0]);

    auto level = topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        onOrderAccepted(20, *level->_orders[0]);
    }

    topSize = 4;
    _stacker->stackAsk(price, topSize);

    ASSERT_EQ(0, _executor->_cancels.size());
}

TEST_F(StackerFixture, StackerTests) {
    hf::price_t tickSize = ToRhonePrice(0.25);
    uint32_t nLevels = 4;
    hf::qty_t quoteSize = 20;
    hf::qty_t topSize = 3;
    _stacker->initialize(1, BID, tickSize, quoteSize, nLevels, _executor, "name", true);
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
    ASSERT_EQ(1, topLevel->_orders.size());

    onOrderAccepted(3, *topLevel->_orders[0]);

    level = topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        onOrderAccepted(20, *level->_orders[0]);
    }

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(-1, topLevel->_flight);
    ASSERT_EQ(3, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());

    topSize = 5;
    _stacker->stackBid(price, topSize);
    topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(2, topLevel->_flight);
    ASSERT_EQ(3, topLevel->_acknowledged);
    ASSERT_EQ(2, topLevel->_orders.size());

    order = topLevel->_orders[1];
    ASSERT_EQ(price, order->pr);
    ASSERT_EQ(3, order->qty);

    onOrderReplaced(2, *topLevel->_orders[0]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(3, topLevel->_flight);
    ASSERT_EQ(2, topLevel->_acknowledged);
    ASSERT_EQ(2, topLevel->_orders.size());

    onOrderAccepted(3, *topLevel->_orders[1]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(0, topLevel->_flight);
    ASSERT_EQ(5, topLevel->_acknowledged);
    ASSERT_EQ(2, topLevel->_orders.size());

    // 1) price -> 99.75 (level below)  /  qty - 4

    topSize = 4;
    price = GetPriorPrice<BID>(price, tickSize);

    auto nextLevel = topLevel->after;

    ASSERT_EQ(price, nextLevel->_price);
    ASSERT_EQ(quoteSize, nextLevel->_target);
    ASSERT_EQ(0, nextLevel->_flight);
    ASSERT_EQ(20, nextLevel->_acknowledged);
    ASSERT_EQ(1, nextLevel->_orders.size());

    _stacker->stackBid(price, topSize);

    auto priorTop = topLevel;
    ASSERT_EQ(0, priorTop->_target);
    ASSERT_EQ(-5, priorTop->_flight);
    ASSERT_EQ(5, priorTop->_acknowledged);
    ASSERT_EQ(2, priorTop->_orders.size());

    topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(-16, topLevel->_flight);
    ASSERT_EQ(20, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());

    level = topLevel;
    for (uint32_t i = 0; i < nLevels - 1; ++i) {
        level = level->after;
        ASSERT_EQ(quoteSize, level->_target);
        ASSERT_EQ(0, level->_flight);
        ASSERT_EQ(20, level->_acknowledged);
    }

    level = level->after;
    ASSERT_EQ(quoteSize, level->_target);
    ASSERT_EQ(20, level->_flight);
    ASSERT_EQ(0, level->_acknowledged);

    ASSERT_EQ(level, _stacker->_bottomLevel);

    onOrderCanceled(*priorTop->_orders[0]);
    onOrderCanceled(*priorTop->_orders[0]);

    onOrderReplaced(4, *topLevel->_orders[0]);

    // top level fully acknowledged now
    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(0, topLevel->_flight);
    ASSERT_EQ(topSize, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());

    // last stacker level now has remaining size in flight
    ASSERT_EQ(quoteSize, level->_target);
    ASSERT_EQ(20, level->_flight);
    ASSERT_EQ(0, level->_acknowledged);
    ASSERT_EQ(1, level->_orders.size());

    onOrderAccepted(20, *level->_orders[0]);

    level = level->after;
    ASSERT_EQ(0, level->_target);

    // 2) going back to level above: price -> 100 (level below)  /  qty - 3
    hf::price_t priorPrice = price;

    topSize = 2;
    price = GetNextPrice<BID>(price, tickSize);

    priorTop = topLevel;

    _stacker->stackBid(price, topSize);

    // top level
    topLevel = _stacker->_topLevel;
    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(2, topLevel->_flight);
    ASSERT_EQ(0, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());

    // stacker levels
    level = topLevel->after;

    // first stacker level which was top level before
    ASSERT_EQ(quoteSize, level->_target);
    ASSERT_EQ(16, level->_flight);
    ASSERT_EQ(4, level->_acknowledged);
    ASSERT_EQ(2, level->_orders.size());

    for (uint32_t i = 0; i < nLevels - 1; ++i) {
        level = level->after;
        ASSERT_EQ(quoteSize, level->_target);
        ASSERT_EQ(0, level->_flight);
        ASSERT_EQ(20, level->_acknowledged);
    }

    // level which used to be stacker
    level = level->after;
    ASSERT_EQ(0, level->_target);
    ASSERT_EQ(-20, level->_flight);
    ASSERT_EQ(20, level->_acknowledged);

    onOrderAccepted(2, *topLevel->_orders[0]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(0, topLevel->_flight);
    ASSERT_EQ(2, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());

    // state of first stacker level
    level = topLevel->after;

    onOrderAccepted(16, *level->_orders[1]);

    ASSERT_EQ(quoteSize, level->_target);
    ASSERT_EQ(0, level->_flight);
    ASSERT_EQ(20, level->_acknowledged);
    ASSERT_EQ(2, level->_orders.size());

    om::OMExecutionEvent event;
    event.filledPrice = price;
    event.filledQty = topSize;


    om::order_t& topOrder =  *topLevel->_orders[0];
    topOrder.is_active = false;

    _stacker->onOrderExecution(event, topOrder);

    ASSERT_EQ(0, topLevel->_orders.size());
}

}