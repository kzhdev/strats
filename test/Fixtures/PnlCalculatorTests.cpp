#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <PnlCalculator.h>
#include "PnlCalculatorFixture.h"

using ::testing::_;

namespace Python {

TEST_F(PnlCalculatorFixture, Test1) {
    auto& detail_1 = _calc->_relativeValueDetail.at(1);
    hf::price_t markPrice_1 = ToRhonePrice(0.63);
    _calc->recalculateOpenPnl(1, markPrice_1);

    FillMessage fill = createFill(1, BID, ToRhonePrice(0.61), 8);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(0, detail_1._buyCost);
    ASSERT_EQ(0, detail_1._sellCost);
    ASSERT_EQ(8, detail_1._openPosition);
    ASSERT_EQ(BID, detail_1._openSide);
    ASSERT_EQ(fill._price * fill._qty, detail_1._openCost);
    ASSERT_EQ(markPrice_1, detail_1._markPrice);
    ASSERT_EQ(1000, detail_1._multiplier);
    ASSERT_EQ(0, detail_1._fills.size());


    fill = createFill(1, ASK, ToRhonePrice(0.62), 4);

}

}