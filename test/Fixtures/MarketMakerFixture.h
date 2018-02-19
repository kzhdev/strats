#ifndef MARKET_MAKER_FIXTURE_H
#define MARKET_MAKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <SimpleMarketMaker.h>

namespace Python {

class MarketMakerFixture : public ::testing::Test {
protected:

    MarketMakerFixture() {
    }

    void SetUp() override {
    }

    void TearDown() override {
    }

};

}

#endif //MARKET_MAKER_FIXTURE_H