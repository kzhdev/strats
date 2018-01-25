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
    executor.removeFills();

    _matchingEngine->modifyBidOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.02), 2, ToRhonePrice(1.02), 2);
    _matchingEngine->matchOrders();

    ASSERT_EQ(0, executor._fills.size());

    _matchingEngine->modifyAskOrder(ORDER_ID_RV_INTERNAL_QUOTE, ToRhonePrice(1.01), 1, ToRhonePrice(1.01), 1);
    _matchingEngine->matchOrders();

    ASSERT_EQ(2, executor._fills.size());
}

}