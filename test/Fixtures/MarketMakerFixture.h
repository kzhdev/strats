#ifndef MARKET_MAKER_FIXTURE_H
#define MARKET_MAKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <SimpleMarketMaker.h>

namespace Python {

class MarketMakerFixture : public ::testing::Test {
protected:

    struct Executor {
        void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0) { }
        void sendErrorMessage(const std::string& s, ErrorType type) { }

        void modifyHedgeAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        }

        void modifyHedgeBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        }

        void modifyQuoteBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        }

        void modifyQuoteAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
        }

        void removeBidOrder(OrderId_t orderId) {
        }

        void removeAskOrder(OrderId_t orderId) {
        }

        void synchronizeOrders() {
        }

        void matchOrders() {
        }
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

    Book createBook(hf::price_t price, hf::qty_t size, md::order_count_t orderCount) {
        Book book;
        book._price = price;
        book._otherQuantity = size;
        book._otherOrderCount = orderCount;
        return book;
    }

    MarketMakerT* _marketMaker;
    Executor* _executor;
};

}

#endif //MARKET_MAKER_FIXTURE_H