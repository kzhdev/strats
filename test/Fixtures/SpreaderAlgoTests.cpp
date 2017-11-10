//
// Created by pkusch on 3/6/17.
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Spreader.h>
#include <CommonHelper.h>
#include "TestStrategy.h"
#include "SpreaderFixture.h"
#include <SpreaderFrontSide.hh>

using ::testing::_;

namespace oms { namespace spreader{

/* 1.
 * - create manual algo with cover
 * - fill 2 spread clips
 * - kick off cover
 * - cover 2 clips
 * - decay
 * - algos correctly removed
 *
 * 2.
 * - create SMA algo with cover
 * - test SMA trigger
 *    - multiple MD changes => spread prices => SMA changes
 * - spread triggered
 * - fill 1st spread clip
 * - cover 1st clip
 * - decay
 * - fill 2nd clip spread clip
 * - cover 2nd clip
 * - decay
 * - algos correctly removed
 *
 * 3.
 * Pause/Resume of spread
 *
 * 4.
 * Pause/Resume of cover spread
 *
 * 5.
 * Cancel spread
 *
 * 6.
 * Cancel cover spread
 *
 */



TEST_F(SpreaderFixture, ManualAlgoTest) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 2, 2);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 100, 0, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, false, -3, 1, 100, 0, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addManualEntryAlgo(message, 10);
    addNoExitAlgo(message);

    _testAlgoManager->processSpreadOrder(message, 1, 0, -1);
    _testSpreader->onStratIdle();
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(static_cast<size_t>(1), _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(1u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(1, _testAlgoManager->_spreadAlgos.begin()->first);

    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_exitAlgos.size());
}

TEST_F(SpreaderFixture, ManualAlgo_Cover_Test) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 2, 1);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 4, 5, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, true, -3, 1, 3, 6, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addManualEntryAlgo(message, toRhonePrice(10));
    addCoverExitAlgo(message, true, false, toRhonePrice(1.0), toRhonePrice(0.1), 1);

    _testAlgoManager->processSpreadOrder(message, 1, 0, 2);
    _testSpreader->onStratIdle();
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(2u, _testAlgoManager->_spreadAlgos.size());

    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    // Work original spread
    sendAskMD(1, 120, 20);
    sendBidMD(2, 15, 40);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::BUY, spread->getLegByIndex(0)._quoteOrder->side);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::SELL, spread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // Fill original spread
    sendFullFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    HedgeOrder* hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    om::order_t* hedgeOrder = hOrder->_order;
    ASSERT_NEAR(119.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 119.75, 1);

    sendFullFill(*hedgeOrder, 1);

    // Retrieve original clip done and send cover spread order
    _testSpreaderFrontSide->processOutgoingMessages();
    _testSpreader->onStratIdle();

    ASSERT_EQ(2u, _testSpreader->_spreadsByOrderId.size());
    TestSpreader::SyntheticSpreadT* coverSpread = _testSpreader->_spreadsByOrderId.find(2)->second;

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._targetQuantity);

    // Work cover spread
    sendBidMD(1, 119, 100);
    sendAskMD(2, 16, 100);

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(129, toDollars(coverSpread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::SELL, coverSpread->getLegByIndex(0)._quoteOrder->side);

    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.25, toDollars(coverSpread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::BUY, coverSpread->getLegByIndex(1)._quoteOrder->side);

    // original spread is not working
    ASSERT_EQ(0, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(0, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._targetQuantity);

    sendOrderAccepted(*coverSpread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*coverSpread->getLegByIndex(0)._quoteOrder);

    // Fill cover spread
    sendFullFill(*coverSpread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, coverSpread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*coverSpread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(16.75, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 15.75, 3);

    sendFullFill(*hedgeOrder, 3);

    _testSpreaderFrontSide->processOutgoingMessages();
    _testSpreader->onStratIdle();

    // Work original spread - 2nd clip
    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);
    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // Fill original spread - 2nd clip
    sendFullFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(119.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 119.75, 1);

    sendFullFill(*hedgeOrder, 1);

    // Original spread is done
    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);

    // Retrieve original clip done and send cover spread order
    _testSpreaderFrontSide->processOutgoingMessages();
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    // Work cover spread = 2nd clip
    ASSERT_EQ(2, coverSpread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, coverSpread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(129, toDollars(coverSpread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::SELL, coverSpread->getLegByIndex(0)._quoteOrder->side);

    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.25, toDollars(coverSpread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::BUY, coverSpread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*coverSpread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*coverSpread->getLegByIndex(0)._quoteOrder);

    // Fill cover spread
    sendFullFill(*coverSpread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, coverSpread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*coverSpread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(16.75, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 15.75, 3);

    sendFullFill(*hedgeOrder, 3);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, coverSpread->_state);

    // Retrieve original clip done and send cover spread order
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(0u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_exitAlgos.size());
}

TEST_F(SpreaderFixture, SMAAlgo_Cover_Test) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 1, 1);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 4, 5, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, true, -3, 1, 3, 6, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addSMAEntryAlgo(message, 0.1, 3, 1);
    addCoverExitAlgo(message, true, false, toRhonePrice(1.0), toRhonePrice(0.1), 1);

    _testAlgoManager->processSpreadOrder(message, 1, 0, 2);
    _testSpreader->onStratIdle();
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(2u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    sendBidMDToAlgo(1, 119, 40);
    sendAskMDToAlgo(1, 120, 40);

    sendBidMDToAlgo(2, 15, 40);
    sendAskMDToAlgo(2, 16, 40);

    TestAlgoManager::SMAEntryAlgoT* entryAlgo = _testAlgoManager->_entryAlgos[0];
    ASSERT_NEAR(8.83333, toDollars(entryAlgo->getLastPrice()), 0.00001);

    // First price for SMA
    entryAlgo->doSMA();
    ASSERT_NEAR(8.83333, toDollars(entryAlgo->getSMA()), 0.00001);

    _testSpreader->onStratIdle();
    ASSERT_EQ(0u, _testSpreader->_spreadsByOrderId.size());

    // Second price for SMA
    sendBidMDToAlgo(1, 115, 20);
    ASSERT_NEAR(8.166666, toDollars(entryAlgo->getLastPrice()), 0.00001);

    // Spread triggered: SMA=8.83333 => Wanted=8.73333 > Price=8.16666
    _testSpreader->onStratIdle();
    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    // Work original spread
    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;
    ASSERT_NEAR(8.733333, toDollars(spread->_price), 0.00001);

    sendAskMD(1, 120, 20);
    sendBidMD(2, 15, 40);

    ASSERT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(116, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::BUY, spread->getLegByIndex(0)._quoteOrder->side);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::SELL, spread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // Calculate SMA using prior price
    entryAlgo->doSMA();

    entryAlgo->doSMA();
    ASSERT_NEAR(8.388888, toDollars(entryAlgo->getSMA()), 0.00001);

    // Reprice spread
    _testSpreader->onStratIdle();

    ASSERT_NEAR(8.288888, toDollars(spread->_price), 0.00001);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(114.75, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::BUY, spread->getLegByIndex(0)._quoteOrder->side);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(16, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::SELL, spread->getLegByIndex(1)._quoteOrder->side);

    // Fill original spread
    sendFullFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    HedgeOrder* hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    om::order_t* hedgeOrder = hOrder->_order;
    ASSERT_NEAR(119.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 119.75, 1);

    sendFullFill(*hedgeOrder, 1);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);

    // Retrieve original clip done and send cover spread order
    _testSpreaderFrontSide->processOutgoingMessages();
    _testSpreader->onStratIdle();

    ASSERT_EQ(1u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    ASSERT_EQ(2u, _testSpreader->_spreadsByOrderId.size());
    TestSpreader::SyntheticSpreadT* coverSpread = _testSpreader->_spreadsByOrderId.find(2)->second;
    ASSERT_NEAR(8.28888, toDollars(spread->_price), 0.00001);

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._targetQuantity);

    sendBidMD(1, 119, 100);
    sendAskMD(2, 16, 100);

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(124, toDollars(coverSpread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::SELL, coverSpread->getLegByIndex(0)._quoteOrder->side);

    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(coverSpread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::BUY, coverSpread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*coverSpread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*coverSpread->getLegByIndex(0)._quoteOrder);

    sendFullFill(*coverSpread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, coverSpread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*coverSpread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(16.75, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 15.75, 3);

    sendFullFill(*hedgeOrder, 3);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, coverSpread->_state);

    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(0u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_exitAlgos.size());
}

TEST_F(SpreaderFixture, SMAAlgo_Cover_Cancel_NotTriggeredSMA_Test) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 1, 1);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 4, 5, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, true, -3, 1, 3, 6, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addSMAEntryAlgo(message, 0.5, 3, 1);
    addCoverExitAlgo(message, true, false, toRhonePrice(1.0), toRhonePrice(0.1), 1);

    _testAlgoManager->processSpreadOrder(message, 1, 0, 2);
    _testSpreader->onStratIdle();
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(2u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    sendBidMDToAlgo(1, 119, 40);
    sendAskMDToAlgo(1, 120, 40);

    sendBidMDToAlgo(2, 15, 40);
    sendAskMDToAlgo(2, 16, 40);

    TestAlgoManager::SMAEntryAlgoT *entryAlgo = _testAlgoManager->_entryAlgos[0];
    ASSERT_NEAR(8.833333, toDollars(entryAlgo->getLastPrice()), 0.00001);

    // First price for SMA
    entryAlgo->doSMA();
    ASSERT_NEAR(8.833333, toDollars(entryAlgo->getSMA()), 0.00001);

    _testSpreader->onStratIdle();
    ASSERT_EQ(0u, _testSpreader->_spreadsByOrderId.size());

    createOtherRequest(SpreadMessageType::CancelSpreadOrder, 1);

    ASSERT_EQ(0u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_exitAlgos.size());
}

TEST_F(SpreaderFixture, SMAAlgo_Cover_Cancel_TriggeredSMA_Test) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 1, 1);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 4, 5, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, true, -3, 1, 3, 6, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addSMAEntryAlgo(message, 0.1, 3, 1);
    addCoverExitAlgo(message, true, false, toRhonePrice(1.0), toRhonePrice(0.1), 1);

    _testAlgoManager->processSpreadOrder(message, 1, 0, 2);
    _testSpreader->onStratIdle();
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(2u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    sendBidMDToAlgo(1, 119, 40);
    sendAskMDToAlgo(1, 120, 40);

    sendBidMDToAlgo(2, 15, 40);
    sendAskMDToAlgo(2, 16, 40);

    TestAlgoManager::SMAEntryAlgoT *entryAlgo = _testAlgoManager->_entryAlgos[0];
    ASSERT_NEAR(8.833333, toDollars(entryAlgo->getLastPrice()), 0.00001);

    // First price for SMA
    entryAlgo->doSMA();
    ASSERT_NEAR(8.833333, toDollars(entryAlgo->getSMA()), 0.00001);

    _testSpreader->onStratIdle();
    ASSERT_EQ(0u, _testSpreader->_spreadsByOrderId.size());

    // Second price for SMA
    sendBidMDToAlgo(1, 115, 20);
    ASSERT_NEAR(8.166666, toDollars(entryAlgo->getLastPrice()), 0.00001);

    // Spread triggered: SMA=8.83333 => Wanted=8.73333 > Price=8.16666
    _testSpreader->onStratIdle();
    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    // Work original spread
    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;
    ASSERT_NEAR(8.733333, toDollars(spread->_price), 0.00001);

    sendAskMD(1, 120, 20);
    sendBidMD(2, 15, 40);

    ASSERT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(116, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::BUY, spread->getLegByIndex(0)._quoteOrder->side);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::SELL, spread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    // Cancel original spread
    createOtherRequest(SpreadMessageType::CancelSpreadOrder, 1);
    _testSpreader->onStratIdle();

    sendCancelled(*spread->getLegByIndex(1)._quoteOrder, 0);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::BeingCancelled, spread->_state);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendCancelled(*spread->getLegByIndex(0)._quoteOrder, 0);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Cancelled, spread->_state);

    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(0u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_exitAlgos.size());
}


TEST_F(SpreaderFixture, SMAAlgo_Cover_Cancel_Triggered_Filled_SMA_Test) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 2, 1);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 4, 5, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, true, -3, 1, 3, 6, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addSMAEntryAlgo(message, 0.1, 3, 1);
    addCoverExitAlgo(message, true, false, toRhonePrice(1.0), toRhonePrice(0.1), 1);

    _testAlgoManager->processSpreadOrder(message, 1, 0, 2);
    _testSpreader->onStratIdle();
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(2u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    sendBidMDToAlgo(1, 119, 40);
    sendAskMDToAlgo(1, 120, 40);

    sendBidMDToAlgo(2, 15, 40);
    sendAskMDToAlgo(2, 16, 40);

    TestAlgoManager::SMAEntryAlgoT *entryAlgo = _testAlgoManager->_entryAlgos[0];
    ASSERT_NEAR(8.833333, toDollars(entryAlgo->getLastPrice()), 0.00001);

    // First price for SMA
    entryAlgo->doSMA();
    ASSERT_NEAR(8.833333, toDollars(entryAlgo->getSMA()), 0.00001);

    _testSpreader->onStratIdle();
    ASSERT_EQ(0u, _testSpreader->_spreadsByOrderId.size());

    // Second price for SMA
    sendBidMDToAlgo(1, 115, 20);
    ASSERT_NEAR(8.166666, toDollars(entryAlgo->getLastPrice()), 0.00001);

    // Spread triggered: SMA=8.83333 => Wanted=8.73333 > Price=8.16666
    _testSpreader->onStratIdle();
    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    // Work original spread
    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;
    ASSERT_NEAR(8.733333, toDollars(spread->_price), 0.00001);

    sendAskMD(1, 120, 20);
    sendBidMD(2, 15, 40);

    ASSERT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(116, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::BUY, spread->getLegByIndex(0)._quoteOrder->side);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15.75, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::SELL, spread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // Fill original spread
    sendFullFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    HedgeOrder* hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    om::order_t* hedgeOrder = hOrder->_order;
    ASSERT_NEAR(119.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 119.75, 1);

    sendFullFill(*hedgeOrder, 1);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, spread->_state);

    // Retrieve original clip done and send cover spread order
    _testSpreaderFrontSide->processOutgoingMessages();
    _testSpreader->onStratIdle();

    ASSERT_EQ(1, _testSpreaderFrontSide->_clientAcceptor->_coverOrder._original);
    ASSERT_EQ(2, _testSpreaderFrontSide->_clientAcceptor->_coverOrder._cover);
    _testSpreaderFrontSide->_clientAcceptor->_coverOrder.reset();

    ASSERT_EQ(2u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    ASSERT_EQ(2u, _testSpreader->_spreadsByOrderId.size());
    TestSpreader::SyntheticSpreadT* coverSpread = _testSpreader->_spreadsByOrderId.find(2)->second;
    ASSERT_NEAR(8.733333, toDollars(spread->_price), 0.00001);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, coverSpread->_state);

    // Cancel original spread
    createOtherRequest(SpreadMessageType::CancelSpreadOrder, 1);
    _testSpreader->onStratIdle();

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Cancelled, spread->_state);

    // Cover now running by itself
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(1u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Running, coverSpread->_state);

    ASSERT_EQ(2, coverSpread->_totalQuantity);

    // Will set the total quantity to 1
    _testSpreader->onStratIdle();

    ASSERT_EQ(1, coverSpread->_totalQuantity);

    sendBidMD(1, 119, 100);
    sendAskMD(2, 16, 100);

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(125.25, toDollars(coverSpread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::SELL, coverSpread->getLegByIndex(0)._quoteOrder->side);

    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.75, toDollars(coverSpread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::BUY, coverSpread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*coverSpread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*coverSpread->getLegByIndex(0)._quoteOrder);

    sendFullFill(*coverSpread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, coverSpread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*coverSpread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(16.75, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 15.75, 3);

    sendFullFill(*hedgeOrder, 3);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, coverSpread->_state);

    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(0u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_exitAlgos.size());
}

TEST_F(SpreaderFixture, SMACalcTest) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 2, 1);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 4, 5, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, true, -3, 1, 3, 6, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addSMAEntryAlgo(message, 0.5, 3, 1);
    addCoverExitAlgo(message, true, false, toRhonePrice(1.0), toRhonePrice(0.1), 1);

    _testAlgoManager->processSpreadOrder(message, 1, 0, 2);
    ASSERT_EQ(1u, _testAlgoManager->_entryAlgos.size());

    TestAlgoManager::SMAEntryAlgoT* entryAlgo = _testAlgoManager->_entryAlgos[0];

    entryAlgo->setLastPrice(toRhonePrice(10));
    entryAlgo->updateSMA();
    ASSERT_NEAR(toRhonePrice(10), entryAlgo->getSMA(), 0.00001);

    entryAlgo->setLastPrice(toRhonePrice(8));
    entryAlgo->updateSMA();
    ASSERT_NEAR(toRhonePrice(9), entryAlgo->getSMA(), 0.00001);

    entryAlgo->setLastPrice(toRhonePrice(7));
    entryAlgo->updateSMA();
    ASSERT_NEAR(toRhonePrice(8.33333333), entryAlgo->getSMA(), 0.00001);

    entryAlgo->setLastPrice(toRhonePrice(6));
    entryAlgo->updateSMA();
    ASSERT_NEAR(toRhonePrice(7), entryAlgo->getSMA(), 0.00001);
}

TEST_F(SpreaderFixture, ManualAlgo_Cover_NoT_by_Clip_Test) {
    proto::NewSpreadOrder message = createSpreadOrder(hf::SIDE::BUY, 0, 2, 1);
    addLeg(message, 3, 1, 20, 10, true, 1, 0, 4, 5, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addLeg(message, 1, -2, 40, 20, true, -3, 1, 3, 6, proto::LeanPricingType::TopLevel, proto::InitiationQuantityType::Absolute, 0);
    addManualEntryAlgo(message, toRhonePrice(10));
    addCoverExitAlgo(message, false, false, toRhonePrice(1.0), toRhonePrice(0.1), 1);

    _testAlgoManager->processSpreadOrder(message, 1, 0, 2);
    _testSpreader->onStratIdle();
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(1u, _testSpreader->_spreadsByOrderId.size());

    TestSpreader::SyntheticSpreadT* spread = _testSpreader->_spreadsByOrderId.find(1)->second;

    ASSERT_EQ(1, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(2u, _testAlgoManager->_spreadAlgos.size());

    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(1u, _testAlgoManager->_exitAlgos.size());

    // Work original spread
    sendAskMD(1, 120, 20);
    sendBidMD(2, 15, 40);

    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(120, toDollars(spread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::BUY, spread->getLegByIndex(0)._quoteOrder->side);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(15, toDollars(spread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::SELL, spread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // Fill original spread clip 1
    sendFullFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    HedgeOrder* hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    om::order_t* hedgeOrder = hOrder->_order;
    ASSERT_NEAR(119.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 119.75, 1);

    sendFullFill(*hedgeOrder, 1);

    // Work original spread - 2nd clip
    ASSERT_EQ(2, spread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, spread->getLegByIndex(1)._targetQuantity);
    ASSERT_EQ(1, spread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(3, spread->getLegByIndex(1)._quotedQuantity);

    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    // Fill original spread - 2nd clip
    sendFullFill(*spread->getLegByIndex(1)._quoteOrder, 3);

    ASSERT_EQ(1u, spread->getLegByIndex(0)._hedgeOrders.size());
    hOrder = &*spread->getLegByIndex(0)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(119.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(121.0, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(1, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 119.75, 1);

    sendFullFill(*hedgeOrder, 1);

    // Original spread is done
    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, spread->_state);

    // Retrieve original clip done and send cover spread order
    _testSpreaderFrontSide->processOutgoingMessages();
    _testSpreader->onStratIdle();

    ASSERT_EQ(2u, _testSpreader->_spreadsByOrderId.size());
    TestSpreader::SyntheticSpreadT* coverSpread = _testSpreader->_spreadsByOrderId.find(2)->second;

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._targetQuantity);

    // Work cover spread
    sendBidMD(1, 119, 100);
    sendAskMD(2, 16, 100);

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(129, toDollars(coverSpread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::SELL, coverSpread->getLegByIndex(0)._quoteOrder->side);

    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.25, toDollars(coverSpread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::BUY, coverSpread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*coverSpread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*coverSpread->getLegByIndex(0)._quoteOrder);

    // Fill cover spread
    sendFullFill(*coverSpread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, coverSpread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*coverSpread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(16.75, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 15.75, 3);

    sendFullFill(*hedgeOrder, 3);

    // Work cover spread = 2nd clip
    ASSERT_EQ(2, coverSpread->getLegByIndex(0)._targetQuantity);
    ASSERT_EQ(6, coverSpread->getLegByIndex(1)._targetQuantity);

    ASSERT_EQ(1, coverSpread->getLegByIndex(0)._quotedQuantity);
    ASSERT_EQ(129, toDollars(coverSpread->getLegByIndex(0)._quotedPrice));
    ASSERT_EQ(hf::SIDE::SELL, coverSpread->getLegByIndex(0)._quoteOrder->side);

    ASSERT_EQ(3, coverSpread->getLegByIndex(1)._quotedQuantity);
    ASSERT_NEAR(14.25, toDollars(coverSpread->getLegByIndex(1)._quotedPrice), 0.00001);
    ASSERT_EQ(hf::SIDE::BUY, coverSpread->getLegByIndex(1)._quoteOrder->side);

    sendOrderAccepted(*coverSpread->getLegByIndex(1)._quoteOrder);
    sendOrderAccepted(*coverSpread->getLegByIndex(0)._quoteOrder);

    // Fill cover spread
    sendFullFill(*coverSpread->getLegByIndex(0)._quoteOrder, 1);

    ASSERT_EQ(1u, coverSpread->getLegByIndex(1)._hedgeOrders.size());
    hOrder = &*coverSpread->getLegByIndex(1)._hedgeOrders.begin();
    hedgeOrder = hOrder->_order;
    ASSERT_NEAR(15.75, toDollars(hedgeOrder->pr), 0.00001);
    ASSERT_NEAR(16.75, toDollars(hOrder->_priceToDoLastCross), 0.00001);
    ASSERT_EQ(3, hedgeOrder->qty);

    sendOrderModified(*hedgeOrder, 15.75, 3);

    sendFullFill(*hedgeOrder, 3);

    ASSERT_EQ(TestSpreader::SyntheticSpreadT::State::Finished, coverSpread->_state);

    // Retrieve original clip done and send cover spread order
    _testSpreaderFrontSide->processOutgoingMessages();

    ASSERT_EQ(0u, _testAlgoManager->_spreadAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_entryAlgos.size());
    ASSERT_EQ(0u, _testAlgoManager->_exitAlgos.size());
}

}}

