#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MarketMakerFixture.h"

using ::testing::_;

namespace Python {

TEST_F(MarketMakerFixture, Test1) {
    ConfigContract contract;
    contract._internalId = 1;
    contract._name = "contract";
    contract._tickSize = ToRhonePrice(0.1);

    hf::qty_t establishedLevel = 200;

    MarketMakerConfig config;
    config._quoteExternal = true;
    config._width = 0.005;
    config._joinSize = 350;
    config._leaveSize = 150;
    config._payUpQty = 500;
    config._payUpMultiple = 1.5;
    config._payUpOrders = 3;
    config._commission = 0.0008;
    config._maxPosExternal = 100;
    config._maxPosInternal = 500;
    config._quoteSize = 100;

    _marketMaker->initialize(contract, establishedLevel, config, _executor);

    Book bidBook = createBook(hf::NULL_PRICE, 0, 0);
    Book askBook = createBook(hf::NULL_PRICE, 0, 0);

    _marketMaker->enable(bidBook, askBook);

    ASSERT_FALSE(_marketMaker->_enabled);

    ASSERT_EQ(0, _executor->_modifyQuoteBidOrders.size());
    ASSERT_EQ(0, _executor->_modifyQuoteAskOrders.size());

    // market 400@10 - 200@@11
    bidBook = createBook(ToRhonePrice(10), 400, 1);
    askBook = createBook(ToRhonePrice(11), 200, 0);

    _marketMaker->enable(bidBook, askBook);

    ASSERT_EQ(1, _executor->_modifyQuoteBidOrders.size());
    ASSERT_EQ(0, _executor->_modifyQuoteAskOrders.size());

}

}