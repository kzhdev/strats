#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <NegativeScalpChecker.h>
#include "PnlCalculatorFixture.h"

using ::testing::_;

namespace Python {

TEST_F(PnlCalculatorFixture, Test1) {
    auto& detail_1 = _calc->_relativeValueContractPnlCalculators.at(1);
    hf::price_t markPrice_1 = ToRhonePrice(0.63);
    _calc->recalculateOpenPnl(1, markPrice_1);

    ASSERT_EQ(markPrice_1, detail_1._markPrice);
    ASSERT_EQ(1000, detail_1._multiplier);

    auto& aggregateRvPnl = _calc->getAggregatedPnl()._relativeValue;
    auto& aggregateTotalPnl = _calc->getAggregatedPnl()._total;

    // long position
    FillMessage fill = createFill(1, BID, ToRhonePrice(0.61), 8);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, detail_1._open);
    ASSERT_EQ(0, detail_1._closed);
    ASSERT_EQ(1, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(0, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(0, aggregateTotalPnl._closed);

    // partially offset long position
    fill = createFill(1, ASK, ToRhonePrice(0.65), 3);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(0.1) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.12) * _multiplier, detail_1._closed);
    ASSERT_EQ(1, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.1) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.12) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.1) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.12) * _multiplier, aggregateTotalPnl._closed);

    // fill rest of long and leave short position
    fill = createFill(1, ASK, ToRhonePrice(0.66), 11);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, detail_1._closed);
    ASSERT_EQ(1, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.18) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateTotalPnl._closed);

    // add to short position
    fill = createFill(1, ASK, ToRhonePrice(0.65), 3);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(0.24) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, detail_1._closed);
    ASSERT_EQ(2, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.24) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.24) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.37) * _multiplier, aggregateTotalPnl._closed);

    // buy fill matching short position
    fill = createFill(1, BID, ToRhonePrice(0.62), 9);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(0) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.7) * _multiplier, detail_1._closed);
    ASSERT_EQ(0, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.7) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.7) * _multiplier, aggregateTotalPnl._closed);

    // adding 3 more buys
    fill = createFill(1, BID, ToRhonePrice(0.61), 2);
    _calc->onFillFromRelativeValue(fill);

    fill = createFill(1, BID, ToRhonePrice(0.61), 4);
    _calc->onFillFromRelativeValue(fill);

    fill = createFill(1, BID, ToRhonePrice(0.61), 5);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(0.22) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.7) * _multiplier, detail_1._closed);
    ASSERT_EQ(3, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.22) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.7) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.22) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.7) * _multiplier, aggregateTotalPnl._closed);

    // sell - filling unmatched and partially first queued
    fill = createFill(1, ASK, ToRhonePrice(0.64), 3);
    _calc->onFillFromRelativeValue(fill);

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, detail_1._open);
    ASSERT_EQ(ToRhonePrice(0.79) * _multiplier, detail_1._closed);
    ASSERT_EQ(2, detail_1._fills.size());

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateRvPnl._open);
    ASSERT_EQ(ToRhonePrice(0.79) * _multiplier, aggregateRvPnl._closed);

    ASSERT_EQ(ToRhonePrice(0.16) * _multiplier, aggregateTotalPnl._open);
    ASSERT_EQ(ToRhonePrice(0.79) * _multiplier, aggregateTotalPnl._closed);
}

}