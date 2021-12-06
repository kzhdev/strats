#ifndef RELATIVE_VALUE_FIXTURE_H
#define RELATIVE_VALUE_FIXTURE_H

#include "RelativeValue.h"

namespace Python {

class RelativeValueFixture : public ::testing::Test {
protected:
    RelativeValueFixture() {}

    void SetUp() override {
        _relativeValue = new RelativeValue();
        _relativeValue->initializeQueues(createQueue(), createQueue(), createQueue(), createQueue());
    }

    void TearDown() override{
    }

    md::queue::MDQueueResource* createQueue() {
        auto pool = new md::pool::MDPool();
        pool->createPool("unit_test_pool", 10000000); // 10000000 in ViperPool
        md::queue::MDQueueResource* qr = new md::queue::MDQueueResource();
        qr->createQueue("unit_test_queue", 1000000);  //10000000 in ViperQueue
        qr->setPool(*pool);
        return qr;
    }

    ConfigContract createContract(const std::string& name, uint32_t gid, uint32_t sid) {
        ConfigContract contract;
        contract._name = name;
        contract._sid = sid;
        contract._gid = gid;
        return contract;
    }

    ConfigContract createContract(const std::string& name, uint32_t gid, uint32_t sid, uint32_t legOne, int32_t legOneRatio, uint32_t legTwo, int32_t legTwoRatio) {
        ConfigContract contract;
        contract._name = name;
        contract._sid = sid;
        contract._gid = gid;
        ConfigLeg one;
        one._index = legOne;
        one._ratio = legOneRatio;
        contract._legs.push_back(one);
        ConfigLeg two;
        two._index = legTwo;
        two._ratio = legTwoRatio;
        contract._legs.push_back(two);
        return contract;
    }

    void populateExternalMarket(uint32_t contractIndex, double bidPrice, hf::qty_t bidQty, double askPrice, hf::qty_t askQty) {
        std::vector<RvContract>& contracts = _relativeValue->_contracts;
        RvContract& c1 = contracts[contractIndex];
        Market& bid = c1._externalMarket[BID];
        if (std::isnan(bidPrice)) {
            bid.update<BID>(hf::NULL_PRICE, 0);
        } else {
            bid.update<BID>(ToRhonePrice(bidPrice), bidQty);
        }
        Market& ask = c1._externalMarket[ASK];
        if (std::isnan(askPrice)) {
            ask.update<ASK>(hf::NULL_PRICE, 0);
        } else {
            ask.update<ASK>(ToRhonePrice(askPrice), askQty);
        }
    }

    void retrieveMessagesFromRvToMM() {
        while (true) {
            uint64_t idx = _relativeValue->_outToMM->getNext();

            if (BOOST_UNLIKELY(idx == 0))
                break;

            _relativeValue->_outToMM->freeNext();

            auto& message = *static_cast<ViperMessage *>(_relativeValue->_outToMM->getPoolP()->getData(idx));

            switch (message._type) {
                case ViperMessageType::HedgeOrder: {
                    HedgeOrderMessage* hedge = &reinterpret_cast<HedgeOrderMessage&> (message._data[0]);
                   _hedges.push_back(hedge);
                    break;
                }
                case ViperMessageType::QuoteOrder: {
                    QuoteOrderMessage* quote = &reinterpret_cast<QuoteOrderMessage&> (message._data[0]);
                    _quotes.push_back(quote);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void flushMessges() {
        _hedges.clear();
        _quotes.clear();
    }

    std::vector<HedgeOrderMessage*> _hedges;
    std::vector<QuoteOrderMessage*> _quotes;

    RelativeValue* _relativeValue;
};

}

#endif //RELATIVE_VALUE_FIXTURE_H
