#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <NegativeScalpChecker.h>
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

    // long position
    FillMessage fill = createFill(1, BID, ToRhonePrice(0.61), 8);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(8, detail_1._unmatchedPosition);
    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, detail_1._open);
    ASSERT_EQ(0, detail_1._closed);
    ASSERT_EQ(0, detail_1._buys.size());
    ASSERT_EQ(0, detail_1._sells.size());

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(0, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(0, aggregateTotalPnl._closed);

    // partially offset long position
    fill = createFill(1, ASK, ToRhonePrice(0.65), 3);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(5, detail_1._unmatchedPosition);
    ASSERT_EQ(ToRhonePrice(0.1) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.12) * _multiplier, detail_1._closed);
    ASSERT_EQ(0, detail_1._buys.size());
    ASSERT_EQ(0, detail_1._sells.size());

    ASSERT_EQ(ToRhonePrice(0.1) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.12) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.1) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.12) * _multiplier, aggregateTotalPnl._closed);

    // fill rest of long and leave short position
    fill = createFill(1, ASK, ToRhonePrice(0.66), 11);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(-6, detail_1._unmatchedPosition);
    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, detail_1._closed);
    ASSERT_EQ(0, detail_1._buys.size());
    ASSERT_EQ(0, detail_1._sells.size());

    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateTotalPnl._closed);

    // add to short position
    fill = createFill(1, ASK, ToRhonePrice(0.65), 3);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(-6, detail_1._unmatchedPosition);
    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, detail_1._closed);
    ASSERT_EQ(0, detail_1._buys.size());
    ASSERT_EQ(0, detail_1._sells.size());

    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateTotalPnl._closed);

}

}