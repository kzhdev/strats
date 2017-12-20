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
        initMarketMakers();
    }

    void initMarketMakers() {
        _queueFromFrontToMM = createQueue();
        _queueFromMMToFront = createQueue();
        _queueFromRVToMM = createQueue();
        _queueFromMMToRV = createQueue();
        _marketMakers->initQueues(_queueFromFrontToMM, _queueFromMMToFront, _queueFromRVToMM, _queueFromMMToRV);
        _marketMakers->onStratBegin();
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

    ViperQueue* createQueue() {
        auto pool = new ViperPool();
        pool->create();
        auto queue = new ViperQueue();
        queue->create();
        queue->set_pool(pool);
        return queue;
    }

    FillMessage* pickUpFillToRv() {
        uint64_t idx = _queueFromMMToRV->getNext();

        if (BOOST_UNLIKELY(idx == 0))
            return nullptr;

        _queueFromMMToRV->freeNext();
        auto& message = _queueFromMMToRV->pool()->getMessage(idx);
        return &reinterpret_cast<FillMessage &> (message._data[0]);
    }

    MarketMakersT* _marketMakers;
    ViperQueue* _queueFromFrontToMM;
    ViperQueue* _queueFromMMToFront;
    ViperQueue* _queueFromRVToMM;
    ViperQueue* _queueFromMMToRV;
};

}

#endif //INTEGRATION_FIXTURE_H
