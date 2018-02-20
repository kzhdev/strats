#ifndef MARKET_MAKER_FIXTURE_H
#define MARKET_MAKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <SimpleMarketMaker.h>

namespace Python {

class MarketMakerFixture : public ::testing::Test {
protected:

    struct ModifyOrder {
        OrderId_t _orderId;
        hf::price_t _internalPrice;
        hf::qty_t _internalSize;
        hf::price_t _externalPrice;
        hf::qty_t _externalSize;
    };

    struct RemoveOrder {
        OrderId_t _orderId;
    };

    struct Executor {
        void sendGenericMessageToFront(ViperMessageType type, int64_t value1 = 0, int64_t value2 = 0) { }
        void sendErrorMessage(const std::string& s, ErrorType type) { }

        void modifyHedgeAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
            _modifyHedgeAskOrders.push_back({orderId, internalPrice, internalSize, externalPrice, externalSize});
        }

        void modifyHedgeBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
            _modifyHedgeBidOrders.push_back({orderId, internalPrice, internalSize, externalPrice, externalSize});
        }

        void modifyQuoteBidOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
            _modifyQuoteBidOrders.push_back({orderId, internalPrice, internalSize, externalPrice, externalSize});
        }

        void modifyQuoteAskOrder(OrderId_t orderId, hf::price_t internalPrice, hf::qty_t internalSize, hf::price_t externalPrice, hf::qty_t externalSize) {
            _modifyQuoteAskOrders.push_back({orderId, internalPrice, internalSize, externalPrice, externalSize});
        }

        void removeBidOrder(OrderId_t orderId) {
            _removeBidOrders.push_back({orderId});
        }

        void removeAskOrder(OrderId_t orderId) {
            _removeAskOrders.push_back({orderId});
        }

        void synchronizeOrders() {
            ++_synchronizeOrders;
        }

        void matchOrders() {
            ++_matchOrders;
        }

        void clearStats() {
            _modifyHedgeAskOrders.clear();
            _modifyHedgeBidOrders.clear();
            _modifyQuoteBidOrders.clear();
            _modifyQuoteAskOrders.clear();
            _removeBidOrders.clear();
            _removeAskOrders.clear();
            _matchOrders = 0;
            _synchronizeOrders = 0;
        }

        std::vector<ModifyOrder> _modifyHedgeAskOrders;
        std::vector<ModifyOrder> _modifyHedgeBidOrders;
        std::vector<ModifyOrder> _modifyQuoteBidOrders;
        std::vector<ModifyOrder> _modifyQuoteAskOrders;
        std::vector<RemoveOrder> _removeBidOrders;
        std::vector<RemoveOrder> _removeAskOrders;
        uint32_t _matchOrders = 0;
        uint32_t _synchronizeOrders = 0;
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