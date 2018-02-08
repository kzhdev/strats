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

    ASSERT_EQ(markPrice_1, detail_1._markPrice);
    ASSERT_EQ(1000, detail_1._multiplier);

    auto& aggregateRvPnl = _calc->getAggregatedPnl()._relativeValue;
    auto& aggregateTotalPnl = _calc->getAggregatedPnl()._total;

    FillMessage fill = createFill(1, BID, ToRhonePrice(0.61), 8);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(0, detail_1._buyCost);
    ASSERT_EQ(0, detail_1._sellCost);
    ASSERT_EQ(8, detail_1._openPosition);
    ASSERT_EQ(BID, detail_1._openSide);
    ASSERT_EQ(ToRhonePrice(4.88), detail_1._openCost);
    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, detail_1._open);
    ASSERT_EQ(0, detail_1._closed);
    ASSERT_EQ(0, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(0, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(0, aggregateTotalPnl._closed);

    fill = createFill(1, ASK, ToRhonePrice(0.65), 3);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(1.83), detail_1._buyCost);
    ASSERT_EQ(ToRhonePrice(1.95), detail_1._sellCost);
    ASSERT_EQ(5, detail_1._openPosition);
    ASSERT_EQ(BID, detail_1._openSide);
    ASSERT_EQ(ToRhonePrice(3.05), detail_1._openCost);
    ASSERT_EQ(ToRhonePrice(0.1) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.12) * _multiplier, detail_1._closed);
    ASSERT_EQ(0, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(0, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(0, aggregateTotalPnl._closed);


}

}