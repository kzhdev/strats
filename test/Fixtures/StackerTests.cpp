#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "StackerFixture.h"

using ::testing::_;

namespace Python {

TEST_F(StackerFixture, StackDependency) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _ime2->getBidStacker()->initialize(1, BID, tickSize, quoteSize, nLevels, _ime2, "name");
    _ime2->getAskStacker()->initialize(1, ASK, tickSize, quoteSize, nLevels, _ime2, "name");

    hf::price_t bidPrice = ToRhonePrice(.35);
    hf::qty_t bidSize = 2;
    _ime2->getBidStacker()->quote<BID>(bidPrice, bidSize);

    onOrderAccepted(_ime2->getBidStacker(), bidSize, *_ime2->getBidStacker()->_topLevel->_orders[0]);

    auto bidLevel = _ime2->getBidStacker()->_topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        bidLevel = bidLevel->after;
        onOrderAccepted(_ime2->getBidStacker(), quoteSize, *bidLevel->_orders[0]);
    }

    auto bidTopLevel = _ime2->getBidStacker()->_topLevel;
    ASSERT_EQ(bidPrice, bidTopLevel->_price);
    ASSERT_EQ(bidSize, bidTopLevel->_target);
    ASSERT_EQ(0, bidTopLevel->_flight);
    ASSERT_EQ(bidSize, bidTopLevel->_acknowledged);
    ASSERT_EQ(1, bidTopLevel->_orders.size());

    hf::price_t askPrice = ToRhonePrice(.36);
    hf::qty_t askSize = 3;
    _ime2->getAskStacker()->quote<ASK>(askPrice, askSize);

    onOrderAccepted(_ime2->getAskStacker(), askSize, *_ime2->getAskStacker()->_topLevel->_orders[0]);

    auto askLevel = _ime2->getAskStacker()->_topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        askLevel = askLevel->after;
        onOrderAccepted(_ime2->getAskStacker(), quoteSize, *askLevel->_orders[0]);
    }

    auto askTopLevel = _ime2->getAskStacker()->_topLevel;
    ASSERT_EQ(askPrice, askTopLevel->_price);
    ASSERT_EQ(askSize, askTopLevel->_target);
    ASSERT_EQ(0, askTopLevel->_flight);
    ASSERT_EQ(askSize, askTopLevel->_acknowledged);
    ASSERT_EQ(1, askTopLevel->_orders.size());

    bidPrice = ToRhonePrice(.36);
    bidSize = 2;
    _ime2->getBidStacker()->quote<BID>(bidPrice, bidSize);

    bidTopLevel = _ime2->getBidStacker()->_topLevel;
    ASSERT_EQ(bidPrice, bidTopLevel->_price);
    ASSERT_EQ(bidSize, bidTopLevel->_target);
    ASSERT_EQ(0, bidTopLevel->_flight);
    ASSERT_EQ(0, bidTopLevel->_acknowledged);
    ASSERT_EQ(0, bidTopLevel->_orders.size());

    auto priorAskTopLevel = _ime2->getAskStacker()->_topLevel;
    askPrice = ToRhonePrice(.37);
    askSize = 3;
    _ime2->getAskStacker()->quote<ASK>(askPrice, askSize);

    onOrderCanceled(_ime2->getAskStacker(), *priorAskTopLevel->_orders[0]);
    onOrderReplaced(_ime2->getAskStacker(), 2, *_ime2->getAskStacker()->_topLevel->_orders[0]);

    bidTopLevel = _ime2->getBidStacker()->_topLevel;
    ASSERT_EQ(bidPrice, bidTopLevel->_price);
    ASSERT_EQ(bidSize, bidTopLevel->_target);
    ASSERT_EQ(bidSize, bidTopLevel->_flight);
    ASSERT_EQ(0, bidTopLevel->_acknowledged);
    ASSERT_EQ(1, bidTopLevel->_orders.size());

    onOrderAccepted(_ime2->getBidStacker(), bidSize, *_ime2->getBidStacker()->_topLevel->_orders[0]);

    ASSERT_EQ(bidPrice, bidTopLevel->_price);
    ASSERT_EQ(bidSize, bidTopLevel->_target);
    ASSERT_EQ(0, bidTopLevel->_flight);
    ASSERT_EQ(bidSize, bidTopLevel->_acknowledged);
    ASSERT_EQ(1, bidTopLevel->_orders.size());
}

TEST_F(StackerFixture, TwoOrdersTest2) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _stacker->initialize(1, ASK, tickSize, quoteSize, nLevels, _ime, "name");

    hf::price_t price = ToRhonePrice(.35);
    hf::qty_t topSize = 2;
    _stacker->quote<ASK>(price, topSize);

    auto topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_orders.size());

    onOrderAccepted(2, *topLevel->_orders[0]);

    {
        auto order = topLevel->_orders[0];
        ASSERT_EQ(price, order->pr);
        ASSERT_EQ(topSize, GetQuantity(*order, PRIOR_QUANTITY));
    }

    auto level = topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        onOrderAccepted(100, *level->_orders[0]);
    }

    topSize = 100;
    _stacker->quote<ASK>(price, topSize);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(2, topLevel->_orders.size());
    {
        auto order = topLevel->_orders[0];
        ASSERT_EQ(price, order->pr);
        ASSERT_EQ(2, GetQuantity(*order, PRIOR_QUANTITY));
    }

    _stacker->quote<ASK>(price, 2);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(2, topLevel->_target);
    ASSERT_EQ(2, topLevel->_orders.size());

    ASSERT_EQ(true, topLevel->_orders[0]->is_cancelling);

    onOrderAccepted(98, *topLevel->_orders[1]);

    {
        auto order = topLevel->_orders[1];
        ASSERT_EQ(price, order->pr);
        ASSERT_EQ(98, GetQuantity(*order, PRIOR_QUANTITY));
    }

    onOrderCanceled(*topLevel->_orders[0]);

    onOrderReplaced(2, *topLevel->_orders[0]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(2, topLevel->_target);
    ASSERT_EQ(1, topLevel->_orders.size());

    {
        auto order = topLevel->_orders[0];
        ASSERT_EQ(price, order->pr);
        ASSERT_EQ(2, GetQuantity(*order, PRIOR_QUANTITY));
    }
}

TEST_F(StackerFixture, TwoOrdersTest) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _stacker->initialize(1, ASK, tickSize, quoteSize, nLevels, _ime, "name");

    hf::price_t price = ToRhonePrice(.35);
    hf::qty_t topSize = 2;
    _stacker->quote<ASK>(price, topSize);

    auto topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_orders.size());

    onOrderAccepted(2, *topLevel->_orders[0]);

    {
        auto order = topLevel->_orders[0];
        ASSERT_EQ(price, order->pr);
        ASSERT_EQ(topSize, GetQuantity(*order, PRIOR_QUANTITY));
    }

    auto level = topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        onOrderAccepted(100, *level->_orders[0]);
    }

    topSize = 100;
    _stacker->quote<ASK>(price, topSize);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(2, topLevel->_orders.size());
    {
        auto order = topLevel->_orders[0];
        ASSERT_EQ(price, order->pr);
        ASSERT_EQ(2, GetQuantity(*order, PRIOR_QUANTITY));
    }

    onOrderAccepted(98, *topLevel->_orders[1]);

    {
        auto order = topLevel->_orders[1];
        ASSERT_EQ(price, order->pr);
        ASSERT_EQ(98, GetQuantity(*order, PRIOR_QUANTITY));
    }

    _stacker->quote<ASK>(price, 2);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(2, topLevel->_target);
    ASSERT_EQ(2, topLevel->_orders.size());

    ASSERT_EQ(true, topLevel->_orders[1]->is_cancelling);
}

TEST_F(StackerFixture, AskGoingAgainst) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _stacker->initialize(1, ASK, tickSize, quoteSize, nLevels, _ime, "name");

    hf::price_t price = ToRhonePrice(.35);
    hf::qty_t topSize = 50;
    _stacker->quote<ASK>(price, topSize);

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
        onOrderAccepted(100, *level->_orders[0]);
    }

    price = ToRhonePrice(.28);
    topSize = 50;
    _stacker->quote<ASK>(price, topSize);
}

TEST_F(StackerFixture, Test1) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _stacker->initialize(1, ASK, tickSize, quoteSize, nLevels, _ime, "name");

    hf::price_t price = ToRhonePrice(.35);
    hf::qty_t topSize = 50;
    _stacker->quote<ASK>(price, topSize);

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
    _stacker->quote<ASK>(price, topSize);
}

TEST_F(StackerFixture, Test2) {
    hf::price_t tickSize = ToRhonePrice(0.01);
    uint32_t nLevels = 5;
    hf::qty_t quoteSize = 100;
    _stacker->initialize(1, ASK, tickSize, quoteSize, nLevels, _ime, "name");

    hf::price_t price = ToRhonePrice(1.27);
    hf::qty_t topSize = 10;
    _stacker->quote<ASK>(price, topSize);

    auto topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(1, topLevel->_orders.size());
    auto order = topLevel->_orders[0];
    ASSERT_EQ(price, order->pr);
    ASSERT_EQ(topSize, order->qty);

    onOrderAccepted(topSize, *topLevel->_orders[0]);

    auto level = topLevel;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        onOrderAccepted(20, *level->_orders[0]);
    }

    topSize = 4;
    _stacker->quote<ASK>(price, topSize);

    ASSERT_EQ(0, _ime->_executor->_cancels.size());
}

TEST_F(StackerFixture, StackerTests) {
    hf::price_t tickSize = ToRhonePrice(0.25);
    uint32_t nLevels = 4;
    hf::qty_t quoteSize = 20;
    hf::qty_t topSize = 3;
    _stacker->initialize(1, BID, tickSize, quoteSize, nLevels, _ime, "name");
    hf::price_t price = ToRhonePrice(100);
    _stacker->quote<BID>(price, topSize);

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

    ASSERT_EQ(5, _ime->_executor->_orders.size());

    _ime->_executor->resetOrderCount();

    topSize = 2;
    _stacker->quote<BID>(price, topSize);

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
    _stacker->quote<BID>(price, topSize);
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

    _stacker->quote<BID>(price, topSize);

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

    ASSERT_EQ(0, priorTop->_target);
    ASSERT_EQ(0, priorTop->_flight);
    ASSERT_EQ(0, priorTop->_acknowledged);
    ASSERT_EQ(0, priorTop->_orders.size());

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

    _stacker->quote<BID>(price, topSize);

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

    // Filling full size on top level
    {
        om::order_t& topOrder = *topLevel->_orders[0];
        onOrderExecuted(topOrder, price, topSize, false);
    }

    ASSERT_EQ(0, topLevel->_orders.size());
    ASSERT_EQ(topSize, topLevel->_traded);

    // new stack to 3
    topSize = 3;
    _stacker->quote<BID>(price, topSize);

    topLevel = _stacker->_topLevel;
    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(3, topLevel->_flight);
    ASSERT_EQ(0, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());
    ASSERT_EQ(0, topLevel->_traded);

    onOrderAccepted(3, *topLevel->_orders[0]);

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(0, topLevel->_flight);
    ASSERT_EQ(3, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());
    ASSERT_EQ(0, topLevel->_traded);

    // Partial fill - leaving 1 size
    {
        om::order_t& topOrder = *topLevel->_orders[0];
        onOrderExecuted(topOrder, price, 2, true);
    }

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(0, topLevel->_flight);
    ASSERT_EQ(3, topLevel->_acknowledged);
    ASSERT_EQ(1, topLevel->_orders.size());
    ASSERT_EQ(2, topLevel->_traded);

    // new stacking same price and increase size to 5
    topSize = 5;
    _stacker->quote<BID>(price, topSize);

    topLevel = _stacker->_topLevel;
    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(4, topLevel->_flight);
    ASSERT_EQ(1, topLevel->_acknowledged);
    ASSERT_EQ(2, topLevel->_orders.size());
    ASSERT_EQ(0, topLevel->_traded);

    // new stacking same price and increase size to 7
    // prior new order is in flight
    topSize = 7;
    _stacker->quote<BID>(price, topSize);

    topLevel = _stacker->_topLevel;
    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(6, topLevel->_flight);
    ASSERT_EQ(1, topLevel->_acknowledged);
    ASSERT_EQ(3, topLevel->_orders.size());
    ASSERT_EQ(0, topLevel->_traded);

    // Fully filling first order
    {
        om::order_t& topOrder = *topLevel->_orders[0];
        onOrderExecuted(topOrder, price, 1, false);
    }

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(topSize, topLevel->_target);
    ASSERT_EQ(6, topLevel->_flight);
    ASSERT_EQ(1, topLevel->_acknowledged);
    ASSERT_EQ(2, topLevel->_orders.size());
    ASSERT_EQ(1, topLevel->_traded);
}

}