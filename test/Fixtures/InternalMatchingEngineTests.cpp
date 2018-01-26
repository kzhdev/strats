#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "InternalMatchingEngineFixture.h"
#include "TestExecutor.h"

using ::testing::_;

namespace Python {

TEST_F(InternalMatchingEngineFixture, IntegrationTests) {
    StackerConfig stackerConfig;
    stackerConfig._quoteSize = 20;
    stackerConfig._numberOfLevels = 5;
    TestExecutor executor;
    _matchingEngine->initialize(0, ToRhonePrice(0.01), stackerConfig, &executor);

    _matchingEngine->modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.01), 1, ToRhonePrice(1.01), 1);
    _matchingEngine->modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.03), 2, ToRhonePrice(1.03), 2);

    _matchingEngine->modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.04), 1, ToRhonePrice(1.01), 1);

    _matchingEngine->matchOrders();

    ASSERT_EQ(2, executor._fills.size());

    {
        auto& fill_1 = executor._fills[0];
        ASSERT_EQ(ToRhonePrice(1.04), fill_1._price);
        ASSERT_EQ(1, fill_1._qty);

        auto& fill_2 = executor._fills[1];
        ASSERT_EQ(ToRhonePrice(1.04), fill_2._price);
        ASSERT_EQ(1, fill_2._qty);
    }

    executor.removeFills();

    ASSERT_EQ(0, _matchingEngine->_bids.size());
    ASSERT_EQ(1, _matchingEngine->_asks.size());

    _matchingEngine->modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.02), 2, ToRhonePrice(1.02), 2);
    _matchingEngine->matchOrders();

    ASSERT_EQ(0, executor._fills.size());

    _matchingEngine->modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.01), 1, ToRhonePrice(1.01), 1);

    ASSERT_EQ(1, _matchingEngine->_bids.size());
    ASSERT_EQ(1, _matchingEngine->_asks.size());

    _matchingEngine->matchOrders();

    ASSERT_EQ(2, executor._fills.size());

    {
        auto& fill_1 = executor._fills[0];
        ASSERT_EQ(ToRhonePrice(1.02), fill_1._price);
        ASSERT_EQ(1, fill_1._qty);

        auto& fill_2 = executor._fills[1];
        ASSERT_EQ(ToRhonePrice(1.02), fill_2._price);
        ASSERT_EQ(1, fill_2._qty);
    }

    ASSERT_EQ(1, _matchingEngine->_bids.size());
    ASSERT_EQ(0, _matchingEngine->_asks.size());
}

TEST_F(InternalMatchingEngineFixture, Test2) {
    StackerConfig stackerConfig;
    stackerConfig._quoteSize = 20;
    stackerConfig._numberOfLevels = 5;
    TestExecutor executor;
    _matchingEngine->initialize(0, ToRhonePrice(0.01), stackerConfig, &executor);

    _matchingEngine->modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.01), 3, ToRhonePrice(1.01), 3);
    _matchingEngine->modifyBidOrder(ORDER_ID_MM_QUOTE, ToRhonePrice(1.02), 2, ToRhonePrice(1.02), 2);

    ASSERT_EQ(2, _matchingEngine->_bids.size());
    ASSERT_EQ(0, _matchingEngine->_asks.size());

    _matchingEngine->modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.0), 6, ToRhonePrice(1.0), 6);

    _matchingEngine->matchOrders();

    ASSERT_EQ(4, executor._fills.size());

    {
        auto& fill_1 = executor._fills[0];
        ASSERT_EQ(ToRhonePrice(1.02), fill_1._price);
        ASSERT_EQ(1, fill_1._qty);

        auto& fill_2 = executor._fills[1];
        ASSERT_EQ(ToRhonePrice(1.02), fill_2._price);
        ASSERT_EQ(1, fill_2._qty);

        auto& fill_3 = executor._fills[2];
        ASSERT_EQ(ToRhonePrice(1.02), fill_1._price);
        ASSERT_EQ(1, fill_1._qty);

        auto& fill_4 = executor._fills[3];
        ASSERT_EQ(ToRhonePrice(1.02), fill_2._price);
        ASSERT_EQ(1, fill_2._qty);
    }
}


}