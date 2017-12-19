#ifndef INTEGRATION_FIXTURE_H
#define INTEGRATION_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MarketMakers.h"
#include "TestStrategy.h"

namespace Python {

class IntegrationFixture : public ::testing::Test {

protected:
    typedef MarketMakers<TestStrategy> MarketMakersT;


    IntegrationFixture() { }

    void SetUp() override {
        _marketMakers = new MarketMakersT();
    }

    void TearDown() override {
    }

    ConfigContract createContract(const std::string& name, uint32_t gid, uint32_t sid) {
        ConfigContract contract;
        contract._name = name;
        contract._sid = sid;
        contract._gid = gid;
        return contract;
    }

    QuoteOrderMessage createQuoteOrderMessage(uint32_t instrumentIndex, uint32_t side, double price, hf::qty_t size, double externalPrice, hf::qty_t externalSize) {
        QuoteOrderMessage order;
        order._instrumentIndex = instrumentIndex;
        order._side = side;
        order._internalMarketPrice  = ToRhonePrice(price);
        order._internalMarketSize = size;
        order._externalPrice = ToRhonePrice(externalPrice);
        order._externalSize = externalSize;
        return order;
    }

    MarketMakersT* _marketMakers;
};

}

#endif //INTEGRATION_FIXTURE_H
