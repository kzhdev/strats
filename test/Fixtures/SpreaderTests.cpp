//
// Created by pkusch on 1/16/17.
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


TEST_F(SpreaderFixture, TickRoundTest) {
    // rounding down negative
    ASSERT_EQ(-175, RoundToTickSize(-158, 25, hf::SIDE::BUY));
    ASSERT_EQ(-25, RoundToTickSize(-1, 25, hf::SIDE::BUY));

    // roudning down positive
    ASSERT_EQ(150, RoundToTickSize(158, 25, hf::SIDE::BUY));
    ASSERT_EQ(0, RoundToTickSize(1, 25, hf::SIDE::BUY));

    // no rounding
    ASSERT_EQ(-150, RoundToTickSize(-150, 25, hf::SIDE::BUY));
    ASSERT_EQ(150, RoundToTickSize(150, 25, hf::SIDE::BUY));
    ASSERT_EQ(0, RoundToTickSize(0, 25, hf::SIDE::BUY));

    // rounding up negative
    ASSERT_EQ(-150, RoundToTickSize(-158, 25, hf::SIDE::SELL));
    ASSERT_EQ(0, RoundToTickSize(-1, 25, hf::SIDE::SELL));

    // rounding up positive
    ASSERT_EQ(175, RoundToTickSize(158, 25, hf::SIDE::SELL));
    ASSERT_EQ(25, RoundToTickSize(1, 25, hf::SIDE::SELL));

    // no rounding
    ASSERT_EQ(-150, RoundToTickSize(-150, 25, hf::SIDE::SELL));
    ASSERT_EQ(150, RoundToTickSize(150, 25, hf::SIDE::SELL));
    ASSERT_EQ(0, RoundToTickSize(0, 25, hf::SIDE::SELL));
}

TEST_F(SpreaderFixture, Quote_Multiple_Legs_Test) {
    createNewSpreadMessage(0, hf::SIDE::SELL, 0, 1, 1, 10,
                           1.5, -1, 10, 20, true, -1, 0, 0, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, 1, 10, 20,    true, 1, 1, 0, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           3, -1, 10, 20,   true, -1, 2, 0, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(1, spread->getLegByIndex(1)._targetQuantity);
    ASSERT_EQ(1, spread->getLegByIndex(2)._targetQuantity);

    sendBidMD(1, -.75, 44);
    sendBidMD(2, -.75, 164);
    sendBidMD(3, -5, 76);

    sendAskMD(1, -.5, 228);
    sendAskMD(2, -.25, 189);
    sendAskMD(3, -.25, 23);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(-10000000, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_TRUE(spread->getLegByIndex(0)._quoteOrder != nullptr);

    ASSERT_EQ(1, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(-2500000, spread->getLegByIndex(1)._quotedPrice);

    ASSERT_EQ(1, spread->getLegByIndex(2)._quotedQuantity);
    ASSERT_EQ(-15000000, spread->getLegByIndex(2)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(2)._quoteOrder);

    sendFullFill(*spread->getLegByIndex(2)._quoteOrder, 1);

    ASSERT_TRUE(spread->isQuoteOrderToMove(*spread->getLegByIndex(0)._quoteOrder));
    ASSERT_TRUE(spread->isQuoteOrderToMove(*spread->getLegByIndex(1)._quoteOrder));

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // quote leg was turned into hedge leg - leg 1
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(nullptr, spread->getLegByIndex(0)._quoteOrder);

    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder1 = spread->getLegByIndex(1)._hedgeOrders[0]._order;
    ASSERT_EQ(-5000000, hedgeOrder1->pr);
    ASSERT_EQ(1, hedgeOrder1->qty);

    // quote leg was turned into hedge leg - leg 0
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(nullptr, spread->getLegByIndex(0)._quoteOrder);

    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    om::order_t* hedgeOrder2 = spread->getLegByIndex(0)._hedgeOrders[0]._order;
    ASSERT_EQ(-7500000, hedgeOrder2->pr);
    ASSERT_EQ(1, hedgeOrder2->qty);

    sendOrderModified(*hedgeOrder1, -5000000, 1);
    ASSERT_FALSE(spread->isQuoteOrderToMove(*hedgeOrder1));

    sendOrderModified(*hedgeOrder2, -7500000, 1);
    ASSERT_FALSE(spread->isQuoteOrderToMove(*hedgeOrder2));

    sendFullFill(*hedgeOrder1, 1);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    sendFullFill(*hedgeOrder2, 1);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);
}

TEST_F(SpreaderFixture, One_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, false, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 15, 40);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1200000000, spread->getLegByIndex(0)._quotedPrice);

    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(2, 16, 40);

    // still quoting the same price since order was not acked yet
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1200000000, spread->getLegByIndex(0)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // now repriced
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1260000000, spread->getLegByIndex(0)._quotedPrice);

    sendBidMD(2, 14, 40);

    // modifies in flight
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1140000000, spread->getLegByIndex(0)._quotedPrice);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_EQ(142500000, hedgeOrder->pr);
    ASSERT_EQ(3, hedgeOrder->qty);
    ASSERT_EQ(1, spread->getLegByIndex(0).getQuoteQuantity());

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1, false);

    // filled on the buy side => stop quoting
    ASSERT_EQ(0, spread->getLegByIndex(0).getQuoteQuantity());
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);

    ASSERT_EQ(2u, spread->getLegByIndex(1)._hedgeOrders.size());

    om::order_t* hedgeOrder2 = spread->getLegByIndex(1)._hedgeOrders[1]._order;
    ASSERT_EQ(142500000, hedgeOrder2->pr);
    ASSERT_EQ(3, hedgeOrder2->qty);

    sendOrderAccepted(*hedgeOrder);
    sendOrderAccepted(*hedgeOrder2);

    // size on the sell leg bid bellow trigger quantity
    sendBidMD(2, 14, 19);

    // hedge orders crossed
    ASSERT_EQ(140000000, hedgeOrder->pr);
    ASSERT_EQ(140000000, hedgeOrder2->pr);

    sendFullFill(*hedgeOrder, 3);
    sendFullFill(*hedgeOrder2, 3);

    ASSERT_EQ(0u, spread->getLegByIndex(1)._hedgeOrders.size());

    // first lot was filled => reloaded
    ASSERT_EQ(4, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(12, spread->getLegByIndex(1)._targetQuantity);

    // but buy leg is not quoted since sell leg bid size < initial quantity
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);

    // increase the sell leg bid size so that it is more than initial quantity
    sendBidMD(2, 14, 40);

    // started quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1140000000, spread->getLegByIndex(0)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // decrease the sell leg bid size so that it is bellow initial quantity
    sendBidMD(2, 14, 39);

    // buy leg quote cancelled
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    // increase the sell leg bid size so that it is more than initial quantity
    sendBidMD(2, 14, 40);

    // not quoting because the order cancel was not acked yet
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    // got the cancel ack and quoting again
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1140000000, spread->getLegByIndex(0)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // buy got filled
    sendFullFill(*spread->getLegByIndex(0)._quoteOrder, 2);

    // no quoting on buy
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_EQ(nullptr, spread->getLegByIndex(0)._quoteOrder);

    // hedging on sell
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_EQ(142500000, hedgeOrder->pr);
    ASSERT_EQ(6, hedgeOrder->qty);

    sendPartialFill(*hedgeOrder, 2);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    sendPartialFill(*hedgeOrder, 4);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);
}

TEST_F(SpreaderFixture, Cancel_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, false, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 15, 40);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1200000000, spread->getLegByIndex(0)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_EQ(152500000, hedgeOrder->pr);
    ASSERT_EQ(3, hedgeOrder->qty);
    ASSERT_EQ(1, spread->getLegByIndex(0).getQuoteQuantity());

    sendOrderAccepted(*hedgeOrder);

    createOtherRequest(SpreadMessageType::CancelSpreadOrder, 1);
    _testSpreader->onStratIdle();

    // cancelling buy quote order
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    // cancelling hedge sell order
    ASSERT_TRUE(hedgeOrder->is_cancelling);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingCancelled, spread->_state);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingCancelled, spread->_state);

    sendCancelled(*hedgeOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Cancelled, spread->_state);
}

TEST_F(SpreaderFixture, Pause_Resume_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, false, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 15, 40);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1200000000, spread->getLegByIndex(0)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_EQ(152500000, hedgeOrder->pr);
    ASSERT_EQ(3, hedgeOrder->qty);
    ASSERT_EQ(1, spread->getLegByIndex(0).getQuoteQuantity());

    sendOrderAccepted(*hedgeOrder);

    // pausing
    createOtherRequest(SpreadMessageType::PauseSpreadOrder, 1);
    _testSpreader->onStratIdle();

    // cancelling buy quote order
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    // cancelling hedge sell order
    ASSERT_TRUE(hedgeOrder->is_cancelling);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingPaused, spread->_state);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingPaused, spread->_state);

    sendCancelled(*hedgeOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Paused, spread->_state);

    // resuming
    createOtherRequest(SpreadMessageType::ResumeSpreadOrder, 1);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1200000000, spread->getLegByIndex(0)._quotedPrice);

    // hedging
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_EQ(152500000, hedgeOrder->pr);
    ASSERT_EQ(3, hedgeOrder->qty);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);
}

TEST_F(SpreaderFixture, Logout_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, false, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 15, 40);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1200000000, spread->getLegByIndex(0)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_EQ(152500000, hedgeOrder->pr);
    ASSERT_EQ(3, hedgeOrder->qty);
    ASSERT_EQ(1, spread->getLegByIndex(0).getQuoteQuantity());

    sendOrderAccepted(*hedgeOrder);

    // logging out
    createOtherRequest(SpreadMessageType::Logout, 1);
    _testSpreader->onStratIdle();

    ASSERT_EQ(TestSpreader::State::LoggingOut, _testSpreader->_state);

    // cancelling buy quote order
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    // cancelling hedge sell order
    ASSERT_TRUE(hedgeOrder->is_cancelling);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingCancelled, spread->_state);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingCancelled, spread->_state);

    sendCancelled(*hedgeOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Cancelled, spread->_state);

    ASSERT_EQ(TestSpreader::State::NotRunning, _testSpreader->_state);
}

TEST_F(SpreaderFixture, ClientDown_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, false, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 15, 40);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(1200000000, spread->getLegByIndex(0)._quotedPrice);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    om::order_t* hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_EQ(152500000, hedgeOrder->pr);
    ASSERT_EQ(3, hedgeOrder->qty);
    ASSERT_EQ(1, spread->getLegByIndex(0).getQuoteQuantity());

    sendOrderAccepted(*hedgeOrder);

    // client down
    createOtherRequest(SpreadMessageType::ClientDown, 1);
    _testSpreader->onStratIdle();

    ASSERT_EQ(TestSpreader::State::OnClientError, _testSpreader->_state);

    // cancelling buy quote order
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    // cancelling hedge sell order
    ASSERT_TRUE(hedgeOrder->is_cancelling);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingCancelled, spread->_state);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingCancelled, spread->_state);

    sendCancelled(*hedgeOrder);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Cancelled, spread->_state);

    ASSERT_EQ(TestSpreader::State::NotRunning, _testSpreader->_state);
}

TEST_F(SpreaderFixture, Price_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, false, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 15, 40);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_NEAR(120, toDollars(spread->getLegByIndex(0)._quotedPrice), 0.00001);

    sendAskMD(2, 16, 40);

    sendBidMD(1, 120, 40);
    sendAskMD(1, 121, 40);

    ASSERT_NEAR(10, toDollars(spread->getBidPrice()), 0.000001);
    ASSERT_NEAR(8.3333333, toDollars(spread->getAskPrice()), 0.000001);
}

TEST_F(SpreaderFixture, Price_Test_2) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           1, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -1, 40, 20, false, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    sendBidMD(1, 20, 40);
    sendAskMD(1, 22, 40);

    sendBidMD(2, 15, 40);
    sendAskMD(2, 16, 40);

    ASSERT_NEAR(5, toDollars(spread->getBidPrice()), 0.000001);
    ASSERT_NEAR(6, toDollars(spread->getAskPrice()), 0.000001);
}

TEST_F(SpreaderFixture, Quote_Sell_Leg_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, false, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, true, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => sell leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendAskMD(1, 120, 20);

    // now quoting sell leg
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);

    sendAskMD(1, 121, 40);

    // still quoting the same price since order was not acked yet
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    // now repriced
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15.25, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendAskMD(1, 118, 40);

    // modifies in flight
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    // partial fill - no hedging since not filled enough for 1 order on buy side
    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 1);
    ASSERT_EQ(0u, spread->getLegByIndex(0)._hedgeOrders.size());
    ASSERT_EQ(5, spread->getLegByIndex(1).getQuoteQuantity());

    // partial fill - no hedging since not filled enough for 1 order on buy side
    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 1);
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    ASSERT_EQ(4, spread->getLegByIndex(1).getQuoteQuantity());

    // partial fill - hedging - have enough
    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 1);
    ASSERT_EQ(3, spread->getLegByIndex(1).getQuoteQuantity());
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    om::order_t* hedgeOrder = spread->getLegByIndex(0)._hedgeOrders.begin()->_order;
    ASSERT_NEAR(117.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    // completing clip size
    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 3, false);

    // filled on the sell side => stop quoting
    ASSERT_EQ(0, spread->getLegByIndex(1).getQuoteQuantity());
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(1)._quotedPrice);

    ASSERT_EQ(2u, spread->getLegByIndex(0)._hedgeOrders.size());

    om::order_t* hedgeOrder2 = spread->getLegByIndex(0)._hedgeOrders[1]._order;
    ASSERT_NEAR(117.75, toDollars(hedgeOrder2->pr), 0.00001);
    ASSERT_EQ(1, hedgeOrder2->qty);

    sendOrderAccepted(*hedgeOrder);
    sendOrderAccepted(*hedgeOrder2);

    // size on the buy leg bid bellow trigger quantity
    sendAskMD(1, 118, 9);

    // hedge orders crossed
    ASSERT_NEAR(118, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(118, toDollars(hedgeOrder2->pr), 0.00001);

    sendFullFill(*hedgeOrder, 1);
    sendFullFill(*hedgeOrder2, 1);

    ASSERT_EQ(0u, spread->getLegByIndex(1)._hedgeOrders.size());

    // first clip was filled => reloaded
    ASSERT_EQ(4, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(12, spread->getLegByIndex(1)._targetQuantity);

    // but sell leg is not quoted since buy leg ask size < initial quantity
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(1)._quotedPrice);

    // increase the buy leg ask size so that it is more than initial quantity
    sendAskMD(1, 118, 20);

    // started quoting buy leg
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    // decrease the buy leg ask size so that it is bellow initial quantity
    sendAskMD(1, 118, 19);

    // sell leg quote cancelled
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(1)._quotedPrice);
    ASSERT_TRUE(spread->getLegByIndex(1)._quoteOrder->is_cancelling);

    // increase the buy leg ask size so that it is more than initial quantity
    sendAskMD(1, 118, 20);

    // not quoting because the order cancel was not acked yet
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(1)._quotedPrice);

    sendCancelled(*spread->getLegByIndex(1)._quoteOrder);

    // got the cancel ack and quoting again
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    // sell got filled
    sendFullFill(*spread->getLegByIndex(1)._quoteOrder, 6);

    // no quoting on sell
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(1)._quotedPrice);
    ASSERT_EQ(nullptr, spread->getLegByIndex(1)._quoteOrder);

    // hedging on buy
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hedgeOrder = spread->getLegByIndex(0)._hedgeOrders.begin()->_order;
    ASSERT_NEAR(117.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_EQ(2, hedgeOrder->qty);

    sendPartialFill(*hedgeOrder, 1);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    sendPartialFill(*hedgeOrder, 1);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);
}

TEST_F(SpreaderFixture, Quote_Both_Legs_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 1, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, true, -3, 1, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => no leg quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendAskMD(1, 120, 20);

    // now quoting sell leg
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    // but not buy leg
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);

    sendBidMD(2, 15, 40);

    // now quoting also buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // partial fill
    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 1);

    // not enough to hedge
    ASSERT_EQ(0u, spread->getLegByIndex(0)._hedgeOrders.size());
    ASSERT_EQ(0u, spread->getLegByIndex(1)._hedgeOrders.size());

    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(5, spread->getLegByIndex(1).getQuoteQuantity());
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendAskMD(1, 118, 20);

    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(5, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    // partial fill
    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 2);

    // now hedging on buy side
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    ASSERT_EQ(0u, spread->getLegByIndex(1)._hedgeOrders.size());

    om::order_t* hedgeOrder = spread->getLegByIndex(0)._hedgeOrders.begin()->_order;
    ASSERT_NEAR(117.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    // quote order was moved to hedge order
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);

    sendOrderModified(*hedgeOrder, 117.75, 1);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(5, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1).getQuoteQuantity());
    ASSERT_NEAR(14.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // buy hedging order filled
    sendFullFill(*hedgeOrder, 1);

    ASSERT_EQ(0u, spread->getLegByIndex(0)._hedgeOrders.size());
    ASSERT_EQ(0u, spread->getLegByIndex(1)._hedgeOrders.size());

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(5, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1).getQuoteQuantity());
    ASSERT_NEAR(14.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    // buy quote order filled
    sendFullFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(0u, spread->getLegByIndex(0)._hedgeOrders.size());
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());

    hedgeOrder = spread->getLegByIndex(1)._hedgeOrders.begin()->_order;
    ASSERT_NEAR(15.25, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_EQ(6, hedgeOrder->qty); // 3 out of it was already executed

    sendOrderModified(*hedgeOrder, 15.25, 6);

    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(0)._quotedPrice);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(hf::NULL_PRICE, spread->getLegByIndex(1)._quotedPrice);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    sendFullFill(*hedgeOrder, 3);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);
}

TEST_F(SpreaderFixture, Payup_sell_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 1, 2,
                           3, 1, 20, 10, true, 1, 0, 2, 5, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, true, -3, 1, 3, 6, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    sendAskMD(1, 120, 20);
    sendBidMD(2, 15, 40);

    // quoting both legs
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // fill
    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    // hedge
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    HedgeOrder* hOrder = &*spread->getLegByIndex(1)._hedgeOrders.begin();
    om::order_t* hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.25, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(14.25, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    sendOrderAccepted(*hedgeOrder);

    // size below trigger quantity
    sendBidMD(2, 15, 5);

    // hedge - do normal cross
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.0, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(14.25, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    // tick down
    sendBidMD(2, 14.75, 40);

    // move hedge
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.0, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(14.25, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    // tick down
    sendBidMD(2, 14.50, 40);

    // move hedge
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(14.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(14.25, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    // tick down
    sendBidMD(2, 14.25, 40);

    // use hammer
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(12.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(14.25, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    // tick down
    sendBidMD(2, 12.5, 40);

    // use hammer again
    ASSERT_EQ(1u, spread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(11.0, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(14.25, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);
}

TEST_F(SpreaderFixture, Payup_buy_Test) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 1, 2,
                           3, 1, 20, 10, true, 1, 0, 4, 5, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 40, 20, true, -3, 1, 3, 6, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT *spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    sendAskMD(1, 120, 20);
    sendBidMD(2, 15, 40);

    // quoting both legs
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(6, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // fill
    sendPartialFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    // hedge
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    HedgeOrder* hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    om::order_t* hedgeOrder = hOrder->_order;
    ASSERT_NEAR(119.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    sendOrderAccepted(*hedgeOrder);

    // size below trigger quantity
    sendAskMD(1, 120.0, 5);

    // hedge - do normal cross
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(120.0, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    // tick down
    sendAskMD(1, 120.25, 50);

    // move hedge
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(120.0, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    // tick down
    sendAskMD(1, 120.50, 50);

    // move hedge
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(120.25, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    // tick down
    sendAskMD(1, 121.0, 50);

    // use hammer
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(122.25, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    // tick up
    sendAskMD(1, 122.25, 50);

    // use hammer
    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(123.50, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);
}

TEST_F(SpreaderFixture, Lean_Avg) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 100, 20, false, -3, 1, 100, 0, LeanPricingType::WeightedAvg, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 1, 15, 40, 14.5, 100, 14, 200);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(118, toDollars(spread->getLegByIndex(0)._quotedPrice));
}

TEST_F(SpreaderFixture, TopOrSecond_Top) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 100, 20, false, -3, 1, 100, 0, LeanPricingType::TopOrSecond, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 1, 15, 40, 14.5, 100, 14, 200);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(117, toDollars(spread->getLegByIndex(0)._quotedPrice));
}

TEST_F(SpreaderFixture, TopOrSecond_Second) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 100, 20, false, -3, 1, 100, 0, LeanPricingType::TopOrSecond, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 1, 15, 400, 14.5, 100, 14, 200);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
}

TEST_F(SpreaderFixture, FullLevel) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 150, 20, false, -3, 1, 100, 0, LeanPricingType::FullLevel, InitiationQuantityType::Absolute, 0);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 1, 15, 40, 14.5, 100, 14, 200);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(114, toDollars(spread->getLegByIndex(0)._quotedPrice));
}

TEST_F(SpreaderFixture, RelativeInitiation) {
    createNewSpreadMessage(10, hf::SIDE::BUY, 0, 2, 2,
                           3, 1, 20, 10, true, 1, 0, 100, 0, LeanPricingType::TopLevel, InitiationQuantityType::Absolute, 0,
                           1, -2, 1000, 20, false, -3, 1, 100, 0, LeanPricingType::FullLevel, InitiationQuantityType::Relative, 15);
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    // no md => buy leg not quoted
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);

    sendBidMD(1, 120, 40);
    sendBidMD(2, 1, 15, 40, 14.5, 100, 14, 200);

    // now quoting buy leg
    ASSERT_EQ(2, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(117, toDollars(spread->getLegByIndex(0)._quotedPrice));
}

TEST_F(SpreaderFixture, Not_Acked_New_Order_Quote_Reprice) {
}

TEST_F(SpreaderFixture, Not_Acked_New_Order_Hedge_Reprice) {
}

TEST_F(SpreaderFixture, Not_Acked_Modify_Quote_Reprice) {
}

TEST_F(SpreaderFixture, Not_Acked_Modify_Hedge_Reprice) {
}

TEST_F(SpreaderFixture, Fill_hedge_cannot_move_quote_new_order_not_acked) {
}

TEST_F(SpreaderFixture, Fill_hedge_cannot_move_quote_modify_not_acked) {
}

TEST_F(SpreaderFixture, Hard_cancel_quote_new_order) {
}

TEST_F(SpreaderFixture, Hard_cancel_quote_modify) {
}

TEST_F(SpreaderFixture, Hard_cancel_hedge_new_order) {
}

TEST_F(SpreaderFixture, Hard_cancel_hedge_modify) {
}


}}