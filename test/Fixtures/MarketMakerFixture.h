#ifndef MARKET_MAKER_FIXTURE_H
#define MARKET_MAKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <SimpleMarketMaker.h>

namespace Python {

class MarketMakerFixture : public ::testing::Test {
protected:

    struct Executor {

    };

    typedef SimpleMarketMaker<Executor> MarketMakerT;

    MarketMakerFixture() {
    }

    void SetUp() override {
        _marketMaker = new MarketMakerT();
        _executor = new Executor();
    }

    void TearDown() override {
    }

    MarketMakerT* _marketMaker;
    Executor* _executor;
};

}

#endif //MARKET_MAKER_FIXTURE_H