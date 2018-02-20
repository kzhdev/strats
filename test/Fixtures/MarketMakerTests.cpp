#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MarketMakerFixture.h"

using ::testing::_;

namespace Python {

TEST_F(MarketMakerFixture, Test1) {
    ConfigContract contract;
    contract._internalId = 1;
    contract._name = "contract";
    contract._tickSize = ToRhonePrice(0.1);

    hf::qty_t establishedLevel = 200;

    MarketMakerConfig config;
    config._quoteExternal = true;
    config._width = 0.005;
    config._joinSize = 350;
    config._leaveSize = 150;
    config._payUpQty = 500;
    config._payUpMultiple = 1.5;
    config._payUpOrders = 3;
    config._commission = 0.0008;
    config._maxPosExternal = 100;
    config._maxPosInternal = 500;
    config._quoteSize = 100;

    _marketMaker->initialize(contract, establishedLevel, config, _executor);

    Book bidBook = createBook(hf::NULL_PRICE, 0, 0);
    Book askBook = createBook(hf::NULL_PRICE, 0, 0);

    _marketMaker->enable(bidBook, askBook);

    ASSERT_FALSE(_marketMaker->_enabled);

    ASSERT_EQ(0, _executor->_modifyQuoteBidOrders.size());
    ASSERT_EQ(0, _executor->_modifyQuoteAskOrders.size());

    // market 400@10 - 200@@11
    bidBook = createBook(ToRhonePrice(10), 400, 1);
    askBook = createBook(ToRhonePrice(11), 200, 0);

    _marketMaker->enable(bidBook, askBook);

    ASSERT_EQ(1, _executor->_modifyQuoteBidOrders.size());

    // not enough join size
    ASSERT_EQ(0, _executor->_modifyQuoteAskOrders.size());

    double mid = 10.6666;

    {
        ModifyOrder& bidOrder = _executor->_modifyQuoteBidOrders[0];
        double internalBid = 10.6608666;
        ASSERT_EQ(ToRhonePrice(internalBid), bidOrder._internalPrice);
        ASSERT_EQ(ToRhonePrice(10.6), bidOrder._externalPrice);
        ASSERT_EQ(500, bidOrder._internalSize);
        ASSERT_EQ(100, bidOrder._externalSize);
    }

    _executor->_modifyQuoteBidOrders.clear();

    // bid size is less then leave size => removing bid
    bidBook = createBook(ToRhonePrice(10), 149, 1);
    _marketMaker->onChangeInOtherBooks(bidBook, askBook, true, false);

    ASSERT_EQ(0, _executor->_modifyQuoteBidOrders.size());
    ASSERT_EQ(0, _executor->_modifyQuoteAskOrders.size());
    ASSERT_EQ(1, _executor->_removeBidOrders.size());

    // get above join size for both bid and ask
    bidBook = createBook(ToRhonePrice(10), 400, 1);
    askBook = createBook(ToRhonePrice(11), 351, 0);
    _marketMaker->onChangeInOtherBooks(bidBook, askBook, true, true);

    ASSERT_EQ(1, _executor->_modifyQuoteBidOrders.size());
    ASSERT_EQ(1, _executor->_modifyQuoteAskOrders.size());

    mid = 10.5326232;

    {
        ModifyOrder& bidOrder = _executor->_modifyQuoteBidOrders[0];
        double internalBid = 10.5268231;
        ASSERT_EQ(ToRhonePrice(internalBid), bidOrder._internalPrice);
        ASSERT_EQ(ToRhonePrice(10.5), bidOrder._externalPrice);
        ASSERT_EQ(500, bidOrder._internalSize);
        ASSERT_EQ(100, bidOrder._externalSize);
    }

    {
        ModifyOrder& askOrder = _executor->_modifyQuoteAskOrders[0];
        double internalAsk = 10.5384231;
        ASSERT_EQ(ToRhonePrice(internalAsk), askOrder._internalPrice);
        ASSERT_EQ(ToRhonePrice(10.6), askOrder._externalPrice);
        ASSERT_EQ(500, askOrder._internalSize);
        ASSERT_EQ(100, askOrder._externalSize);
    }

    Order bidQuoteOrder;
    bidQuoteOrder._externalPrice = ToRhonePrice(10.5);
    bidQuoteOrder._externalSize = 100;
    bidQuoteOrder._side = BID;
    bidQuoteOrder._type = Order::Type::Quoting;

    _marketMaker->onExternalFill(&bidQuoteOrder, bidQuoteOrder._externalPrice, 20, bidBook, askBook);

    ASSERT_EQ(20, _marketMaker->_externallyInitiatedPosition);

    ASSERT_EQ(0, _executor->_modifyHedgeBidOrders.size());
    ASSERT_EQ(1, _executor->_modifyHedgeAskOrders.size());

    {
        ModifyOrder& askHedge = _executor->_modifyHedgeAskOrders[0];
        ASSERT_EQ(ToRhonePrice(10.0992), askHedge._internalPrice);
        ASSERT_EQ(ToRhonePrice(10.1), askHedge._externalPrice);
        ASSERT_EQ(20, askHedge._internalSize);
        ASSERT_EQ(20, askHedge._externalSize);
    }

    // bid quote should re-load but no change on the ask side
    ASSERT_EQ(1, _executor->_modifyQuoteBidOrders.size());
    ASSERT_EQ(1, _executor->_modifyQuoteAskOrders.size()); // TODO - wrong

    _marketMaker->_executor->clearStats();

    _marketMaker->onInternalFill(&bidQuoteOrder, bidQuoteOrder._price, 400, bidBook, askBook);

    ASSERT_EQ(400, _marketMaker->_internallyInitiatedPosition);

    {
        ModifyOrder& askHedge = _executor->_modifyHedgeAskOrders[0];
        ASSERT_EQ(ToRhonePrice(10.0992), askHedge._internalPrice);
        ASSERT_EQ(ToRhonePrice(10.1), askHedge._externalPrice);
        ASSERT_EQ(420, askHedge._internalSize);
        ASSERT_EQ(420, askHedge._externalSize);
    }

    _marketMaker->_executor->clearStats();

    Order askHedgeOrder;
    askHedgeOrder._externalPrice = ToRhonePrice(10.1);
    askHedgeOrder._price = ToRhonePrice(10.0992);
    askHedgeOrder._externalSize = 420;
    askHedgeOrder._size = 420;
    askHedgeOrder._side = ASK;
    askHedgeOrder._type = Order::Type::Hedging;

    // filling hedge order
    _marketMaker->onExternalFill(&askHedgeOrder, askHedgeOrder._externalPrice, 150, bidBook, askBook);

    ASSERT_EQ(250, _marketMaker->_internallyInitiatedPosition);

    {
        MarketMakerT::Hedge& askHedge = _marketMaker->_hedges[ASK];
        ASSERT_EQ(ToRhonePrice(10.0992), askHedge._internalPrice);
        ASSERT_EQ(ToRhonePrice(10.1), askHedge._externalPrice);
        ASSERT_EQ(270, askHedge._size);
    }
}

}