//
// Created by azhitlenok on 9/15/17.
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Spreader.h>
#include <CommonHelper.h>
#include "TestStrategy.h"
#include "SpreaderFixture.h"
#include "SpreadCommon.h"

using ::testing::_;

namespace oms { namespace spreader {

TEST_F(SpreaderFixture, TwoLegs__Rejected1__Rejected2__Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    EXPECT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    EXPECT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    EXPECT_EQ(1, spread->getLegByIndex(1)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);

    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);

    EXPECT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(-12500000, spread->getLegByIndex(0)._quotedPrice);

    EXPECT_EQ(1, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    sendOrderRejected(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);
    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling); //order is not cancelling because it's not acked

    //spread tries to cancel all (other leg) orders
    sendOrderRejected(*spread->getLegByIndex(1)._quoteOrder); //AZ700 BeingPaused not changed to Paused - still paused, so cannot be resumed

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);

    spread->resume();

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Running);
}

TEST_F(SpreaderFixture, TwoLegs__Rejected1_Ack2_Cancelled2__Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    EXPECT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    EXPECT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    EXPECT_EQ(1, spread->getLegByIndex(1)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);

    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);

    EXPECT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(-12500000, spread->getLegByIndex(0)._quotedPrice);

    EXPECT_EQ(1, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);
    sendOrderRejected(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);
    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling); //order is not cancelling because it's not acked

    //spread tries to cancel all (other leg) orders
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling); //order cancelling after being acked
    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);

    sendCancelled(*spread->getLegByIndex(1)._quoteOrder, 1);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);

    spread->resume();

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Running);
}

TEST_F(SpreaderFixture, TwoLegs__Rejected1__Ack2_PartiallyFilled2_Canceled2_HCanceled1_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    EXPECT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    EXPECT_EQ(10, spread->getLegByIndex(0)._targetQuantity);
    EXPECT_EQ(10, spread->getLegByIndex(1)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);
    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);

    EXPECT_EQ(10, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(-12500000, spread->getLegByIndex(0)._quotedPrice);

    EXPECT_EQ(10, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);
    sendOrderRejected(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);

    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling); //order is not cancelling because it's not acked

    //spread tries to cancel all (other leg) orders
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling); //order cancelling after being acked
    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);

    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 0u);

    sendCancelled(*spread->getLegByIndex(1)._quoteOrder);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);
}

TEST_F(SpreaderFixture, TwoLegs__Ack_Rejected__PartiallyFilled_05_acked_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 2;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 100;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    EXPECT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    EXPECT_EQ(100, spread->getLegByIndex(0)._targetQuantity);
    EXPECT_EQ(200, spread->getLegByIndex(1)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);
    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);

    EXPECT_EQ(100, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(-12500000, spread->getLegByIndex(0)._quotedPrice);

    EXPECT_EQ(200, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    //spread tries to cancel all (other leg) orders
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    sendOrderRejected(*spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_active);

    for(uint i(0); i < 30; ++i) {
        sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 5);
        EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 0u);
    }

    sendCancelled(*spread->getLegByIndex(1)._quoteOrder);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);
}

TEST_F(SpreaderFixture, TwoLegs__Ack_PartiallyFilled__05_acked_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 2;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 100;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    EXPECT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    EXPECT_EQ(100, spread->getLegByIndex(0)._targetQuantity);
    EXPECT_EQ(200, spread->getLegByIndex(1)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);
    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);

    EXPECT_EQ(100, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(-12500000, spread->getLegByIndex(0)._quotedPrice);

    EXPECT_EQ(200, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 5);
    //other leg quote order should be moved to hedge order
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 1u);
}

TEST_F(SpreaderFixture, Quote_Multiple_Legs_1_Test) {

    uint nLegs = 3;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    legsData[2].div = 3;
    legsData[2].fac = -1;
    legsData[2].iq = 10;
    legsData[2].tq = 20;
    legsData[2].quote = true;
    legsData[2].qf = -1;
    legsData[2].gid = 2;
    legsData[2].payup = 0;
    legsData[2].hammer = 0;
    legsData[2].lpt = LeanPricingType::TopLevel;
    legsData[2].iqt = InitiationQuantityType::Absolute;
    legsData[2].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    EXPECT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    EXPECT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    EXPECT_EQ(1, spread->getLegByIndex(1)._targetQuantity);
    EXPECT_EQ(1, spread->getLegByIndex(2)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);
    sendBidMD(3, -5, 76);

    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);
    sendAskMD(3, -.25, 23);

    EXPECT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(-10000000, spread->getLegByIndex(0)._quotedPrice);

    EXPECT_EQ(1, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    EXPECT_EQ(1, spread->getLegByIndex(2)._quotedQuantity);
    EXPECT_EQ(-15000000, spread->getLegByIndex(2)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(2)._quoteOrder);

    sendFullFill(*spread->getLegByIndex(2)._quoteOrder, 1);

    ASSERT_TRUE(spread->isQuoteOrderToMove(*spread->getLegByIndex(0)._quoteOrder));
    ASSERT_TRUE(spread->isQuoteOrderToMove(*spread->getLegByIndex(1)._quoteOrder));

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // quote leg was turned into hedge leg - leg 1
    EXPECT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(nullptr, spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder1 = spread->getLegByIndex(1)._hedgeOrders[0]._order;
    EXPECT_EQ(-5000000, hedgeOrder1->pr);
    EXPECT_EQ(1, hedgeOrder1->qty);

    // quote leg was turned into hedge leg - leg 0
    EXPECT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(nullptr, spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    om::order_t* hedgeOrder2 = spread->getLegByIndex(0)._hedgeOrders[0]._order;
    EXPECT_EQ(-7500000, hedgeOrder2->pr);
    EXPECT_EQ(1, hedgeOrder2->qty);

    sendOrderModified(*hedgeOrder1, -5000000, 1);
    ASSERT_FALSE(spread->isQuoteOrderToMove(*hedgeOrder1));

    sendOrderModified(*hedgeOrder2, -7500000, 1);
    ASSERT_FALSE(spread->isQuoteOrderToMove(*hedgeOrder2));

    sendFullFill(*hedgeOrder1, 1);

    EXPECT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    sendFullFill(*hedgeOrder2, 1);

    EXPECT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);
}

TEST_F(SpreaderFixture, Quote_Multiple_Legs_2_Test) {

    uint nLegs = 3;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    legsData[2].div = 3;
    legsData[2].fac = -1;
    legsData[2].iq = 10;
    legsData[2].tq = 20;
    legsData[2].quote = true;
    legsData[2].qf = -1;
    legsData[2].gid = 2;
    legsData[2].payup = 0;
    legsData[2].hammer = 0;
    legsData[2].lpt = LeanPricingType::TopLevel;
    legsData[2].iqt = InitiationQuantityType::Absolute;
    legsData[2].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    EXPECT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    EXPECT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    EXPECT_EQ(1, spread->getLegByIndex(1)._targetQuantity);
    EXPECT_EQ(1, spread->getLegByIndex(2)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);
    sendBidMD(3, -5, 76);

    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);
    sendAskMD(3, -.25, 23);

    EXPECT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(-10000000, spread->getLegByIndex(0)._quotedPrice);

    EXPECT_EQ(1, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    EXPECT_EQ(1, spread->getLegByIndex(2)._quotedQuantity);
    EXPECT_EQ(-15000000, spread->getLegByIndex(2)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(2)._quoteOrder);

    sendFullFill(*spread->getLegByIndex(2)._quoteOrder, 1);

    ASSERT_TRUE(spread->isQuoteOrderToMove(*spread->getLegByIndex(0)._quoteOrder));
    ASSERT_TRUE(spread->isQuoteOrderToMove(*spread->getLegByIndex(1)._quoteOrder));

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // quote leg was turned into hedge leg - leg 1
    EXPECT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    EXPECT_EQ(nullptr, spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder1 = spread->getLegByIndex(1)._hedgeOrders[0]._order;
    EXPECT_EQ(-5000000, hedgeOrder1->pr);
    EXPECT_EQ(1, hedgeOrder1->qty);

    // quote leg was turned into hedge leg - leg 0
    EXPECT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    EXPECT_EQ(nullptr, spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    om::order_t* hedgeOrder2 = spread->getLegByIndex(0)._hedgeOrders[0]._order;
    EXPECT_EQ(-7500000, hedgeOrder2->pr);
    EXPECT_EQ(1, hedgeOrder2->qty);

    sendOrderModified(*hedgeOrder1, -5000000, 1);
    ASSERT_FALSE(spread->isQuoteOrderToMove(*hedgeOrder1));

    sendOrderModified(*hedgeOrder2, -7500000, 1);
    ASSERT_FALSE(spread->isQuoteOrderToMove(*hedgeOrder2));

    sendFullFill(*hedgeOrder1, 1);

    EXPECT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    sendFullFill(*hedgeOrder2, 1);

    EXPECT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);
}

TEST_F(SpreaderFixture, Bid0_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];
    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr); //OK - quoting leg is SELL leg for the case

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
}

TEST_F(SpreaderFixture, Ask0_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];
    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookData);
    sendAskMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].o, 0u);

    //no quoting because only sell sides and sell update event on sell leg
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, BidAsk1_OneSide_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;
    uint legIndex = 1;

    level_data_t bookDataSell[10];
    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 164;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -.85;
        bookDataBuy[0].q = 33;
        bookDataBuy[0].o = 0;
    }
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].pr, -6500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -8500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 33);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->side, hf::SIDE::BUY);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -15000000);
}

TEST_F(SpreaderFixture, BidAskFilled0_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex = 1;

    level_data_t bookDataSell[10];
    {
        bookDataSell[0].pr = -.75;
        bookDataSell[0].q = 164;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -.95;
        bookDataBuy[0].q = 33;
        bookDataBuy[0].o = 0;
    }
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -9500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 33);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendFullFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 1);
}

TEST_F(SpreaderFixture, BidAskPartialFilled0_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex = 1;

    level_data_t bookDataSell[10];
    {
        bookDataSell[0].pr = -.75;
        bookDataSell[0].q = 164;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -.95;
        bookDataBuy[0].q = 33;
        bookDataBuy[0].o = 0;
    }
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_askBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -9500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 33);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 1);
}

//second level update influence
//second update does not have any effect
TEST_F(SpreaderFixture, Bid_SecondLevel_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 9; //less than 10
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 9);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    //now we want to update second level - so we don't expect any changes
    {
        bookData[0].pr = -.75;
        bookData[0].q = 9;
        bookData[0].o = 0;
    }
    {
        bookData[1].pr = -.85;
        bookData[1].q = 20;
        bookData[1].o = 0;
    }
    uint levels = 2;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData, levels);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
}

//second level update influence
//second update does not have any effect
TEST_F(SpreaderFixture, Bid1_SecondLevel_2levelsMode_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];
    {
        bookData[0].pr = -.75;
        bookData[0].q = 9; //less than 10
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 9);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    //now we want to update second level - so we don't expect any changes
    {
        bookData[0].pr = -.75;
        bookData[0].q = 9;
        bookData[0].o = 0;
    }
    {
        bookData[1].pr = -.85;
        bookData[1].q = 20;
        bookData[1].o = 0;
    }

    uint levels = 2;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData, levels);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, Hedge_Fill_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr); //AZ700?

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendFullFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 1);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    //should be different in different tests
    sendOrderAccepted(*spread->getLegByIndex(1)._hedgeOrders[0]._order);
    sendFullFill(*spread->getLegByIndex(1)._hedgeOrders[0]._order, 1);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 0u);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::Finished);
}

TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_ModifiedPrice0_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr); //quoting starts on BUY leg
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    //market changed, so quote order is modified
    {
        bookData[0].pr = -.15;
        bookData[0].q = 22;
        bookData[0].o = 0;
    }

    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -2500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    //hedge order is not acked, so it's not changed
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);
}

TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_ModifiedQty0_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr); //quoting starts on BUY leg
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    //market changed, so quote order is modified
    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    //hedge order is not acked, so it's not changed
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);
}

TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_Ack1_Modified0_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr); //quoting starts on BUY leg
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    sendOrderAccepted(*spread->getLegByIndex(1)._hedgeOrders[0]._order);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_acked);

    //market changed, so quote order is modified
    {
        bookData[0].pr = -.15;
        bookData[0].q = 22;
        bookData[0].o = 0;
    }

    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -2500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    //hedge order is acked, so it's changed
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, 1000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);
}

TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_Rejected0_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr); //quoting starts on BUY leg
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    sendOrderRejected(*spread->getLegByIndex(1)._hedgeOrders[0]._order);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 0u);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);
    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);

}

TEST_F(SpreaderFixture, Bid0_Bid1_Modify10_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 9;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 9);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    for(uint i(0); i < 15; ++i) {
        //modifies qty to 10
        //market changed, so quote order is modified
        {
            bookData[0].pr = -.75;
            bookData[0].q = 10;
            bookData[0].o = 0;
        }

        change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
        sendBidMD(2, change, bookData);

        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);

        sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
        //market changed, so quote order is modified
        //back t0 9
        {
            bookData[0].pr = -.75;
            bookData[0].q = 9;
            bookData[0].o = 0;
        }

        change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
        sendBidMD(2, change, bookData);

        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
        EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);
        sendCancelled(*spread->getLegByIndex(0)._quoteOrder);
        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    }
}

TEST_F(SpreaderFixture, Bid0_Bid1_Modify0_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];

    {
        bookData[0].pr = -.75;
        bookData[0].q = 9;
        bookData[0].o = 0;
    }

    uint legIndex = 1;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 9);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    for(uint i(0); i < 15; ++i) {
        //modifies qty to 10
        //market changed, so quote order is modified
        {
            bookData[0].pr = -.75;
            bookData[0].q = 10;
            bookData[0].o = 0;
        }

        change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
        sendBidMD(2, change, bookData);

        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);

        //don't ack order here
        //market changed, so quote order is modified
        //back t0 9
        {
            bookData[0].pr = -.75;
            bookData[0].q = 9;
            bookData[0].o = 0;
        }

        change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
        sendBidMD(2, change, bookData);

        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
        EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

        sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
        EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

        sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    }
}

TEST_F(SpreaderFixture, Bid0_Bid1_Modify02_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    level_data_t bookData[10];
    {
        bookData[0].pr = -0.75;
        bookData[0].q = 44;
        bookData[0].o = 0;
    }

    uint legIndex = 0;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(1, change, bookData);

    {
        bookData[0].pr = -.75;
        bookData[0].q = 9;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 44);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 9);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    for(uint i(0); i < 15; ++i) {
        //modifies qty to 10
        //market changed, so quote order is modified
        {
            bookData[0].pr = -.75;
            bookData[0].q = 9;
            bookData[0].o = 0;
        }

        change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
        sendBidMD(2, change, bookData);

        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

        //don't ack order here
        //market changed, so quote order is modified
        //back t0 9
        {
            bookData[0].pr = -.75;
            bookData[0].q = 9;
            bookData[0].o = 0;
        }
        {
            bookData[1].pr = -.85;
            bookData[1].q = 10;
            bookData[1].o = 0;
        }
        {
            bookData[2].pr = -.95;
            bookData[2].q = 10;
            bookData[2].o = 0;
        }

        uint levels = 3;

        change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData, levels);
        sendBidMD(2, change, bookData);

        EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
        EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    }
}

TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_Test_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex = 0;

    level_data_t bookData[10];
    {
        bookData[0].pr = -0.75;
        bookData[0].q = 44;
        bookData[0].o = 0;
    }

    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(1, change, bookData);

    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 44);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr); //AZ700?

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    //market changed, so quote order is modified
    {
        bookData[0].pr = -.15;
        bookData[0].q = 22;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -2500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);
}

TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_Fill0_Garbage_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookData[10];
    {
        bookData[0].pr = -0.75;
        bookData[0].q = 44;
        bookData[0].o = 0;
    }

    legIndex = 0;
    auto change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(1, change, bookData);

    {
        bookData[0].pr = -.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(2, change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 44);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr); //AZ700?

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    {
        bookData[0].pr = -.5;
        bookData[0].q = 22;
        bookData[0].o = 0;
    }

    //sets garbage to another side
    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = hf::NULL_PRICE;
        bookDataBuy[0].q = 0;
        bookDataBuy[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookData);
    sendAskBidMD(2, change, 0, bookData, bookDataBuy);

    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_cancelling); //AZ700 order should not be cancelled because of garbage
}

TEST_F(SpreaderFixture, Bid1_BidAsk0_Test) {

    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    {
        bookDataSell[0].pr = -0.65;
        bookDataSell[0].q = 33;
        bookDataSell[0].o = 0;
    }
    legIndex = 0;
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 44;
        bookDataBuy[0].o = 0;
    }
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);

    {
        bookDataBuy[0].pr = -.75;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }

    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 44);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, QuoteActiveLeg_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 2;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    legIndex = 1;
    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    {
        bookDataSell[0].pr = -.75;
        bookDataSell[0].q = 30;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    {
        bookDataBuy[0].pr = -2.75;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    legIndex = 0;

    {
        bookDataBuy[0].pr = -1.75;
        bookDataBuy[0].q = 44;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -0.25;
        bookDataSell[0].q = 33;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->_legs[0]._books->_bidBook.l[0].pr, -17500000);
    EXPECT_EQ(spread->_legs[0]._books->_bidBook.l[0].q, 44);
    EXPECT_EQ(spread->_legs[0]._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->_legs[0]._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->_legs[0]._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->_legs[0]._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->_legs[0]._books->_askBook.l[0].pr, -2500000);
    EXPECT_EQ(spread->_legs[0]._books->_askBook.l[0].q, 33);
    EXPECT_EQ(spread->_legs[0]._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->_legs[0]._books->_askBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->_legs[0]._books->_askBook.l[1].q, 0);
    EXPECT_EQ(spread->_legs[0]._books->_askBook.l[1].o, 0u);

    EXPECT_EQ(spread->_legs[1]._books->_bidBook.l[0].pr, -27500000);
    EXPECT_EQ(spread->_legs[1]._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->_legs[1]._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->_legs[1]._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->_legs[1]._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->_legs[1]._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->_legs[1]._books->_askBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->_legs[1]._books->_askBook.l[0].q, 30);
    EXPECT_EQ(spread->_legs[1]._books->_askBook.l[0].o, 0u);

    EXPECT_EQ(spread->_legs[1]._books->_askBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->_legs[1]._books->_askBook.l[1].q, 0);
    EXPECT_EQ(spread->_legs[1]._books->_askBook.l[1].o, 0u);

    EXPECT_NE(spread->_legs[0]._quoteOrder, nullptr);
    EXPECT_EQ(spread->_legs[1]._quoteOrder, nullptr);

    //change leg 1
    legIndex = 1;
    {
        bookDataSell[0].pr = -.5;
        bookDataSell[0].q = 30;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    {
        bookDataBuy[0].pr = -.75;
        bookDataBuy[0].q = 45;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->_legs[0]._quoteOrder, nullptr);
    EXPECT_NE(spread->_legs[1]._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage1_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 2;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    uint changeBuy, changeSell;

    legIndex = 0;
    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 12;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendBidMD(1, changeBuy, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 1;
    {
        bookDataBuy[0].pr = -2.25;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -1.00;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 12);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -22500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    //update leg1
    legIndex = 1;
    {
        bookDataSell[0].pr = -1.25;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 134;
        bookDataBuy[0].o = 3;
    }
    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy); //garbage that can trigger order

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    bookDataSell[1].q = 15;
    changeSell = 2;

    bookDataBuy[0].pr = hf::NULL_PRICE;

    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage1_TopOrSecond_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopOrSecond;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 2;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    uint changeBuy, changeSell;

    legIndex = 0;
    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 12;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendBidMD(1, changeBuy, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 1;
    {
        bookDataBuy[0].pr = -2.25;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -1.00;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 12);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -22500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    //update leg1
    legIndex = 1;
    {
        bookDataSell[0].pr = -1.25;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 134;
        bookDataBuy[0].o = 3;
    }
    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy); //garbage that can trigger order

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    bookDataSell[1].q = 15;
    changeSell = 2;

    bookDataBuy[0].pr = hf::NULL_PRICE;

    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage1_FullLevel_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::FullLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 2;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    uint changeBuy, changeSell;

    legIndex = 0;
    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 12;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendBidMD(1, changeBuy, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 1;
    {
        bookDataBuy[0].pr = -2.25;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -1.00;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 12);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -22500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    //update leg1
    legIndex = 1;
    {
        bookDataSell[0].pr = -1.25;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 134;
        bookDataBuy[0].o = 3;
    }
    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy); //garbage that can trigger order

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    bookDataSell[1].q = 15;
    changeSell = 2;

    bookDataBuy[0].pr = hf::NULL_PRICE;

    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage1_WeightedAvg_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::WeightedAvg;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 2;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    uint changeBuy, changeSell;

    legIndex = 0;
    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 12;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendBidMD(1, changeBuy, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 1;
    {
        bookDataBuy[0].pr = -2.25;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -1.00;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 12);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -22500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    //update leg1
    legIndex = 1;
    {
        bookDataSell[0].pr = -1.25;
        bookDataSell[0].q = 12;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    {
        bookDataBuy[0].pr = -1.25;
        bookDataBuy[0].q = 134;
        bookDataBuy[0].o = 3;
    }
    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy); //garbage that can trigger order

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    bookDataSell[1].q = 15;
    changeSell = 2;

    bookDataBuy[0].pr = hf::NULL_PRICE;

    sendAskBidMD(2, changeSell, 0, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage2_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 5;
        bookDataBuy[0].o = 0;
    }
    legIndex = 1;
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendBidMD(2, changeBuy, bookDataBuy);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    //event must be the same as book if it's not empty
    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 154);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 5);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 44;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 56;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, 0, changeBuy, bookDataSell, bookDataBuy); //garbage
    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);

    //same not garbage
    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 55;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage2_TopOrSecond_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopOrSecond;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopOrSecond;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 5;
        bookDataBuy[0].o = 0;
    }
    legIndex = 1;
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendBidMD(2, changeBuy, bookDataBuy);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    //event must be the same as book if it's not empty
    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 154);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 5);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 44;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 56;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, 0, changeBuy, bookDataSell, bookDataBuy); //garbage
    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);

    //same not garbage
    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 55;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage2_FullLevel_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::FullLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::FullLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 5;
        bookDataBuy[0].o = 0;
    }
    legIndex = 1;
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendBidMD(2, changeBuy, bookDataBuy);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    //event must be the same as book if it's not empty
    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 154);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 5);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 44;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 56;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, 0, changeBuy, bookDataSell, bookDataBuy); //garbage
    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);

    //same not garbage
    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 55;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);
}

TEST_F(SpreaderFixture, Bid1_Bid0_Garbage2_WeightedAvg_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::WeightedAvg;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::WeightedAvg;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 5;
        bookDataBuy[0].o = 0;
    }
    legIndex = 1;
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendBidMD(2, changeBuy, bookDataBuy);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    auto changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    //event must be the same as book if it's not empty
    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 154);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 5);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 154;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 44;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 56;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, 0, changeBuy, bookDataSell, bookDataBuy); //garbage
    EXPECT_FALSE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);

    //same not garbage
    legIndex = 0;
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 55;
        bookDataBuy[0].o = 0;
    }
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    {
        bookDataSell[0].pr = -.65;
        bookDataSell[0].q = 4;
        bookDataSell[0].o = 0;
    }
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);
    EXPECT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);
}

/*
TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_Ack0__Modify0_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = -1;
    legsData[1].hammer = 1;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataBuy[10];

    legIndex = 1;
    {
        bookDataBuy[0].pr = -.75;
        bookDataBuy[0].q = 19;
        bookDataBuy[0].o = 0;
    }
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendBidMD(2, changeBuy, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 19);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr); //AZ700?

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -5000000);

    //as -5000000 > -7500000 we expect book update, but in the test it's late
    sendOrderAccepted(*spread->getLegByIndex(1)._hedgeOrders[0]._order);

    //AZ700 - why in no market change order is modifying after ack - because of hammer price should not be
    EXPECT_FALSE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -7500000);

    //modify book 0
    //market changed on hedge side - nothing modified
    {
        bookDataBuy[0].pr = -.15;
        bookDataBuy[0].q = 22;
        bookDataBuy[0].o = 0;
    }
    legIndex = 0;

    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendBidMD(1, changeBuy, bookDataBuy);

    //market is updated - order is modifying
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -10000000);
}
 */
TEST_F(SpreaderFixture, Bid0_Bid1_Ack1_PFill1_Hedge0_Ack0__NotModify0_Test) {
    uint nLegs = 2;
    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = 0;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = 1;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    level_data_t bookDataBuy[10];
    {
        bookDataBuy[0].pr = -0.75;
        bookDataBuy[0].q = 44;
        bookDataBuy[0].o = 0;
    }
    legIndex = 0;
    auto changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendBidMD(1, changeBuy, bookDataBuy);

    {
        bookDataBuy[0].pr = -.75;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }
    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);
    sendBidMD(2, changeBuy, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].q, 44);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(0)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].pr, -7500000);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].q, 164);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[0].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].pr, hf::NULL_PRICE);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].q, 0);
    EXPECT_EQ(spread->getLegByIndex(1)._books->_bidBook.l[1].o, 0u);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr); //AZ700?

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 10);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->executedQty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_TRUE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_active);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);

    //as -5000000 > -7500000 we expect book update
    {
        bookDataBuy[0].pr = -.5;
        bookDataBuy[0].q = 2;
        bookDataBuy[0].o = 0;
    }
    {
        bookDataBuy[1].pr = -.75;
        bookDataBuy[1].q = 164;
        bookDataBuy[1].o = 0;
    }
    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendBidMD(2, changeBuy, bookDataBuy);

    sendOrderAccepted(*spread->getLegByIndex(1)._hedgeOrders[0]._order);

    //AZ700 - why in no market change order is modifying after ack
    EXPECT_FALSE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);

    //modify book 0
    //market changed on hedge side - nothing modified
    {
        bookDataBuy[0].pr = -.15;
        bookDataBuy[0].q = 22;
        bookDataBuy[0].o = 0;
    }
    legIndex = 0;

    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);

    sendBidMD(1, changeBuy, bookDataBuy);

    //market is updated on guoting side - order is not modifying
    EXPECT_FALSE(spread->getLegByIndex(1)._hedgeOrders[0]._order->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._priceToDoLastCross, -7500000);
}

TEST_F(SpreaderFixture, TwoExchanges_Test) {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(hf::VENUE::CME, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(hf::VENUE::ICE, symbolLeg2);
    symbols.push_back(symbolLeg2);

    _testSpreader->context().init(symbols);

    spread_leg_data_t legsData[nLegs];
    legsData[0].div = 1.5;
    legsData[0].fac = -1;
    legsData[0].iq = 10;
    legsData[0].tq = 20;
    legsData[0].quote = true;
    legsData[0].qf = -1;
    legsData[0].gid = symbolLeg1.gid;
    legsData[0].payup = 0;
    legsData[0].hammer = 0;
    legsData[0].lpt = LeanPricingType::TopLevel;
    legsData[0].iqt = InitiationQuantityType::Absolute;
    legsData[0].inMult = 0;

    legsData[1].div = 1;
    legsData[1].fac = 1;
    legsData[1].iq = 10;
    legsData[1].tq = 20;
    legsData[1].quote = true;
    legsData[1].qf = 1;
    legsData[1].gid = symbolLeg2.gid;
    legsData[1].payup = 0;
    legsData[1].hammer = 0;
    legsData[1].lpt = LeanPricingType::TopLevel;
    legsData[1].iqt = InitiationQuantityType::Absolute;
    legsData[1].inMult = 0;

    hf::price_t price = 0;
    hf::SIDE side = hf::SIDE::SELL;
    hf::price_t adjustor = 0;
    hf::qty_t clips = 1;
    hf::qty_t lotSize = 10;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

    _testSpreader->onStratIdle();

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    uint legIndex;

    uint changeBuy, changeSell;

    level_data_t bookDataSell[10];
    level_data_t bookDataBuy[10];

    {
        bookDataBuy[0].pr = -.75;
        bookDataBuy[0].q = 164;
        bookDataBuy[0].o = 0;
    }

    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);


    {
        bookDataSell[0].pr = -.5;
        bookDataSell[0].q = 33;
        bookDataSell[0].o = 0;
    }

    legIndex = 1;
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(2, changeSell, changeBuy, bookDataSell, bookDataBuy);

    {
        bookDataBuy[0].pr = -.75;
        bookDataBuy[0].q = 22;
        bookDataBuy[0].o = 0;
    }

    legIndex = 0;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);


    {
        bookDataSell[0].pr = -.5;
        bookDataSell[0].q = 23;
        bookDataSell[0].o = 0;
    }

    legIndex = 0;
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(1, changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    auto *order1 = spread->getLegByIndex(0)._quoteOrder;
    auto *order2 = spread->getLegByIndex(1)._quoteOrder;

    sendPartialFill(*order1, 2);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 0u);

    //ok we expect to have hedge 2
    EXPECT_EQ(order2->qty, 2);

    EXPECT_TRUE(order2->is_modifying);

    sendOrderModified(*order2, order2->pr, order2->qty);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder->qty, 8u);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 0u);
}


}
}
