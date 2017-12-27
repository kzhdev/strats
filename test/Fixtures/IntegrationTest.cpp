#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "IntegrationFixture.h"

using ::testing::_;

namespace Python {

TEST_F(IntegrationFixture, IntegrationTests) {
    ViperConfig config;

    auto& stackerConfig = config._stackerConfigs;
    stackerConfig.push_back({"G18", 4, 20});
    stackerConfig.push_back({"H18", 3, 10});

    config._allContracts.push_back(createContract("G18", 0));
    config._allContracts.push_back(createContract("H18", 1));

    auto& marketMakerConfig = config._marketMakerConfigs;
    marketMakerConfig.push_back({"G18", true, 20, 0.2, 50, 30, 20, 1, 2, 100, 200, 0.2});
    marketMakerConfig.push_back({"H18", true, 20, 0.2, 50, 30, 20, 1, 2, 100, 200, 0.2});

    _marketMakers->resetQueues();
    _marketMakers->initializeMarketMakers(config);

    QuoteOrderMessage bidQuote = createQuoteOrderMessage(1, BID, 65.12, 6, 65, 20);
    _marketMakers->onQuoteOrderFromRv(bidQuote);

    auto& oneIME = _marketMakers->_contractStates[1]._marketMaker._internalMatchingEngine;

    auto bidOrdersIt = oneIME._bids.begin();
    Order* bestBid = bidOrdersIt->second;

    ASSERT_EQ(bidQuote._internalMarketPrice, bestBid->_price);

    QuoteOrderMessage askQuote = createQuoteOrderMessage(1, ASK, 65.77, 3, 66, 12);
    _marketMakers->onQuoteOrderFromRv(askQuote);

    auto& bidStacker = oneIME._bidStacker;
    auto& askStacker = oneIME._askStacker;

    auto bidTop = bidStacker._topLevel;
    auto askTop = askStacker._topLevel;

    ASSERT_EQ(ToRhonePrice(65), bidTop->_price);
    ASSERT_EQ(20, bidTop->_target);

    ASSERT_EQ(ToRhonePrice(66), askTop->_price);
    ASSERT_EQ(12, askTop->_target);

    bidQuote = createQuoteOrderMessage(1, BID, 65.8, 6, 65, 20);
    _marketMakers->onQuoteOrderFromRv(bidQuote);

    bidOrdersIt = oneIME._bids.begin();
    bestBid = bidOrdersIt->second;

    ASSERT_EQ(bidQuote._internalMarketPrice, bestBid->_price);

    FillMessage* internalFill = pickUpFillToRv();

    ASSERT_EQ(3, internalFill->_size);
    ASSERT_EQ(ToRhonePrice(65.8), internalFill->_price);
    ASSERT_EQ(BID, internalFill->_side);
    ASSERT_EQ(1, internalFill->_contractIndex);

    internalFill = pickUpFillToRv();

    ASSERT_EQ(3, internalFill->_size);
    ASSERT_EQ(ToRhonePrice(65.8), internalFill->_price);
    ASSERT_EQ(ASK, internalFill->_side);
    ASSERT_EQ(1, internalFill->_contractIndex);
}

TEST_F(IntegrationFixture, MarketMakerTests) {
    ViperConfig config;

    auto& stackerConfig = config._stackerConfigs;
    stackerConfig.push_back({"G18", 4, 20});
    stackerConfig.push_back({"H18", 3, 10});

    config._allContracts.push_back(createContract("G18", 0));
    config._allContracts.push_back(createContract("H18", 1));

    auto& marketMakerConfig = config._marketMakerConfigs;
    marketMakerConfig.push_back({"G18", true, 20, 0.2, 50, 30, 20, 1, 2, 100, 200, 0.2});
    marketMakerConfig.push_back({"H18", true, 20, 0.2, 50, 30, 20, 1, 2, 100, 200, 0.2});

    auto& pricingConfigs = config._pricingConfigs;
    pricingConfigs.push_back({"G18", 0.2, true, 100});
    pricingConfigs.push_back({"H18", 0.2, true, 100});

    _marketMakers->resetQueues();
    _marketMakers->initializeMarketMakers(config);


}


















}