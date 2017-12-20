#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "IntegrationFixture.h"

using ::testing::_;

namespace Python {

TEST_F(IntegrationFixture, IntegrationTests) {
    ViperConfig config;

    auto& stackerConfig = config._stackerConfigs;
    stackerConfig.push_back({"c1", 4, 20});
    stackerConfig.push_back({"c2", 3, 10});

    config._allContracts.push_back(createContract("G18", 0, 0));
    config._allContracts.push_back(createContract("H18", 1, 1));

    _marketMakers->initalizeMarketMakers(config);

    QuoteOrderMessage bidQuote = createQuoteOrderMessage(1, BID, 65.12, 6, 65, 20);
    _marketMakers->onQuoteOrderFromRv(bidQuote);

    auto& oneIME = _marketMakers->_contractStates[1]._marketMaker._internalMatchingEngine;

    auto bidOrdersIt = oneIME._bids.begin();
    Order* bestBid = bidOrdersIt->second;

    ASSERT_EQ(bidQuote._internalMarketPrice, bestBid->_price);

    QuoteOrderMessage askQuote = createQuoteOrderMessage(1, ASK, 65.77, 3, 66, 12);
    _marketMakers->onQuoteOrderFromRv(askQuote);

    auto askOrdersIt = oneIME._asks.begin();
    Order* bestAsk = askOrdersIt->second;

    ASSERT_EQ(askQuote._internalMarketPrice, bestAsk->_price);

}

}