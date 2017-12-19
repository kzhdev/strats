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

    QuoteOrderMessage order;
    order._instrumentIndex = 1;
    order._side = BID;
    order._internalMarketPrice  = ToRhonePrice(65.12);
    order._internalMarketSize = 6;
    order._externalPrice = ToRhonePrice(65);
    order._externalSize = 20;

    _marketMakers->onQuoteOrderFromRv(order);




}

}