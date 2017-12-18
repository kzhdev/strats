#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "StackerFixture.h"

using ::testing::_;

namespace Python {

TEST_F(StackerFixture, StackerTests) {
    hf::price_t tickSize = ToRhonePrice(0.25);
    uint32_t nLevels = 4;
    uint32_t quoteSize = 20;
    _stacker->initialize(1, BID, tickSize, quoteSize, nLevels, _executor);
    hf::price_t price = ToRhonePrice(100);
    _stacker->stackBid(price, 3);

    // 99.75, 99.5, 99.25, 99, 98.75 - 0
    // 100 - 4
    // 100.25, 100.5, 100.75, 101 - 20
    // 101.25, 101.5, 101.75, 102, 102.25 - 0

    auto topLevel = _stacker->_topLevel;

    ASSERT_EQ(price, topLevel->_price);
    ASSERT_EQ(3, topLevel->_target);

    hf::price_t p = price;
    auto level = topLevel;
    for (uint32_t i = 0; i < NUMBER_OF_PRE_CREATED_LEVELS; ++i) {
        level = level->before;
        ASSERT_TRUE(level != nullptr);
        p = GetNextPrice<BID>(p, tickSize);
        ASSERT_EQ(p, level->_price);
        ASSERT_EQ(0, level->_target);
    }

    level = topLevel;
    p = price;
    for (uint32_t i = 0; i < nLevels; ++i) {
        level = level->after;
        ASSERT_TRUE(level != nullptr);
        p = GetPriorPrice<BID>(p, tickSize);
        ASSERT_EQ(p, level->_price);
        ASSERT_EQ(quoteSize, level->_target);
    }


}

}