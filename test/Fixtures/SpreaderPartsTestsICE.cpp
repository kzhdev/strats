//
// Created by azhitlenok on 10/2/17.
//
// ICE-like specific tests
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

//ICE - we should NOT wait for order ack
//ICE - we should wait for order modification, check is_modifying order flag

typedef SpreaderFixtureTyped<hf::VENUE::ICE, SpreaderType::Standalone, false, true> SpreaderFixtureICEStandalone;

/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* no ack
* Leg2 market moved
* Quote order changed
* leg2 market moved
* no changes
*/
template<>
void SpreaderFixtureTypedLight<false, true>::MarketMove_NoAck_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    {
        bookData[0].pr = -2.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    sendOrderModified(*spread->getLegByIndex(0)._quoteOrder, -27500000, -1);

    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
};


/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* no ack
* Leg2 market moved
* leg1 order modified
* ack
* no change
* leg2 market moved
* no change
* modifying ack
* leg1 order modified
* leg2 market moved
* leg1 order modified
*/
template<>
void SpreaderFixtureTypedLight<false, true>::MarketMove_NoAck_Ack_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    {
        bookData[0].pr = -2.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], RepriceQuote);

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying); //true from initial modifying with no ack

    //marked for delayed modification
    sendOrderModified(*spread->getLegByIndex(0)._quoteOrder, spread->getLegByIndex(0)._quoteOrder->pr, spread->getLegByIndex(0)._quoteOrder->qty);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -42500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], 0);

    {
        bookData[0].pr = -3.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -42500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], RepriceQuote);

    sendOrderModified(*spread->getLegByIndex(0)._quoteOrder, spread->getLegByIndex(0)._quoteOrder->pr, spread->getLegByIndex(0)._quoteOrder->qty);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -57500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], 0);

    sendOrderModified(*spread->getLegByIndex(0)._quoteOrder, spread->getLegByIndex(0)._quoteOrder->pr, spread->getLegByIndex(0)._quoteOrder->qty);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_active);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -57500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
};

/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* spread canceled
* order no changed
* ack
* order cancelling
* cancel ack
* state - Cancelled
*/
template<>
void SpreaderFixtureTypedLight<false, true>::MarketMove_UserCancelled_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    uint32_t messageId = 12345;
    bool sendReply;
    //AZ - we support the whole spread cancel only and don't support individual message maintenance
    spread->cancel(messageId, sendReply);

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingCancelled);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Cancelled);
}


/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* spread canceled
* order no changed
* ack
* order cancelling
* cancel ack
* state - Cancelled
*/
template<>
void SpreaderFixtureTypedLight<false, true>::MarketMove_UnsolicitedCancelled_Test() {

    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData); //move market

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder, 100); //unsolicited

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);

}

/*
* cancel after accepted
*/
template<>
void SpreaderFixtureTypedLight<false, true>::CancelModifying_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData); //move market

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);

    uint32_t messageId = 12345;
    bool sendReply;

    spread->cancel(messageId, sendReply);
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], CancelOrder);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingCancelled);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder); //unsolicited

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Cancelled);
}

/*
* cancel before accepted
*/
template<>
void SpreaderFixtureTypedLight<false, true>::CancelImmediate_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    uint32_t messageId = 12345;
    bool sendReply;

    spread->cancel(messageId, sendReply);

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], 0);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData); //move market

    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingCancelled);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder); //unsolicited

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Cancelled);
}

/*
* New order rejected
*/
template<>
void SpreaderFixtureTypedLight<false, true>::NO_Rejected_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);


    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], 0);

    sendOrderRejected(*spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Paused);
}


/*
* Modify order rejected
*/
template<>
void SpreaderFixtureTypedLight<false, true>::Modify_Rejected_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);


    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], 0);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendOrderModifiedRejected(*spread->getLegByIndex(0)._quoteOrder, 123);

    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_cancelling);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingPaused);

    sendCancelled(*spread->getLegByIndex(0)._quoteOrder);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
}

/*
* Modify order rejected because of fill
*/
template<>
void SpreaderFixtureTypedLight<false, true>::Modify_RejectedFilled_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);


    {
        bookData[0].pr = -1.75;
        bookData[0].q = 164;
        bookData[0].o = 0;
    }

    legIndex = 1;
    change = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookData);
    sendBidMD(getSid(spread, legIndex), change, bookData);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -27500000);
    EXPECT_TRUE(spread->getLegByIndex(0)._quoteOrder->is_modifying);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->debug.ch[0], 0);

    auto *order = spread->getLegByIndex(0)._quoteOrder;

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendFullFill(*spread->getLegByIndex(0)._quoteOrder, spread->getLegByIndex(0)._quoteOrder->qty);

    sendOrderModifiedRejected(*order, GetOrderIsNotInBookCode());

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Running);
}

/*
* cancel rejected because of fill
*/
template<>
void SpreaderFixtureTypedLight<false, true>::CancelRejected_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    uint32_t messageId = 123;
    bool sendReply;
    spread->cancel(messageId, sendReply);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    auto *order = spread->getLegByIndex(0)._quoteOrder;

    sendFullFill(*spread->getLegByIndex(0)._quoteOrder, spread->getLegByIndex(0)._quoteOrder->qty);

    sendCancelRejected(*order, GetOrderIsNotInBookCode());

    EXPECT_EQ(spread->_state, decltype(spread->_state)::Cancelled);
}

/*
* cancel rejected because of fill
*/
template<>
void SpreaderFixtureTypedLight<false, true>::UnsolicitedCancelRejected_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
    hf::qty_t maxTicks = 10;
    bool skipCrossTest = false;

    createNewSpreadMessage(price, side, adjustor, clips, lotSize, maxTicks, skipCrossTest, legsData, nLegs, _venue);

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
    sendBidMD(getSid(spread, legIndex), change, bookData);

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
    EXPECT_FALSE(spread->getLegByIndex(0)._quoteOrder->is_acked);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->pr, -12500000);
    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 1);

    uint32_t messageId = 123;
    bool sendReply;
    spread->cancel(messageId, sendReply);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    auto *order = spread->getLegByIndex(0)._quoteOrder;

    sendCancelRejected(*order, GetOrderIsNotInBookCode() + 1);

    EXPECT_EQ(spread->_state, decltype(spread->_state)::BeingCancelled);

    //what message sets state to Cancelled?
}

/*
* Quote Order to hedge
*/
template<>
void SpreaderFixtureTypedLight<false, true>::QuoteToHedge_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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
    hf::qty_t lotSize = 1;
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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);

    sendFullFill(*spread->getLegByIndex(0)._quoteOrder, spread->getLegByIndex(0)._quoteOrder->qty);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quotedQuantity, 0);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 1u);
}


/*
* Hedge Modifying test
*/
template<>
void SpreaderFixtureTypedLight<false, true>::HedgeModifying_1_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    auto *order1 = spread->getLegByIndex(0)._quoteOrder;
    auto *order2 = spread->getLegByIndex(1)._quoteOrder;

    sendPartialFill(*order1, 2);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);

    //ok we expect to have hedge 2
    EXPECT_EQ(order2->qty, 2);

    EXPECT_TRUE(order2->is_modifying);

    //leg2 order was filled by 5
    sendPartialFill(*order2, 5);

    //leg1 is filled by 2
    //leg2 is filled by 5
    //I expected to have leg1 hedge 3
    //or leg1 hedge 5 & leg2 hedge 2

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 1u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 5);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);

    //leg2 order was filled by 3
    sendPartialFill(*order2, 3);

    sendPartialFill(*order1, 1);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 2u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 3);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 8);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[1]._order->qty, 3);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);

    {
        bookDataBuy[0].pr = -1.75;
        bookDataBuy[0].q = 34;
        bookDataBuy[0].o = 0;
    }

    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);


    {
        bookDataSell[0].pr = -.25;
        bookDataSell[0].q = 42;
        bookDataSell[0].o = 0;
    }

    legIndex = 1;
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder->qty, 7);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 2u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 3);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 8);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[1]._order->qty, 3);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    sendPartialFill(*spread->getLegByIndex(0)._quoteOrder, 1);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 3u);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[2]._order->qty, 1);

}

/*
* Hedge Modifying test
*/
template<>
void SpreaderFixtureTypedLight<false, true>::HedgeModifying_2_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    auto *order1 = spread->getLegByIndex(0)._quoteOrder;
    auto *order2 = spread->getLegByIndex(1)._quoteOrder;

    sendPartialFill(*order1, 2);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);

    //ok we expect to have hedge 2
    EXPECT_EQ(order2->qty, 2);

    EXPECT_TRUE(order2->is_modifying);

    //leg2 order was filled by 5
    sendPartialFill(*order2, 5);

    //leg1 is filled by 2
    //leg2 is filled by 5
    //I expected to have leg1 hedge 3
    //or leg1 hedge 5 & leg2 hedge 2

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 1u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 5);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);

    //leg2 order was filled by 3
    sendPartialFill(*order2, 3);

    sendPartialFill(*order1, 1);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 2u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 3);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 8);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[1]._order->qty, 3);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);

    {
        bookDataBuy[0].pr = -1.75;
        bookDataBuy[0].q = 34;
        bookDataBuy[0].o = 0;
    }

    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);


    {
        bookDataSell[0].pr = -.25;
        bookDataSell[0].q = 42;
        bookDataSell[0].o = 0;
    }

    legIndex = 1;
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder->qty, 7);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 2u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 3);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 8);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[1]._order->qty, 3);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);

    sendOrderModified(*spread->getLegByIndex(1)._hedgeOrders[0]._order, spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, spread->getLegByIndex(1)._hedgeOrders[0]._order->qty);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 2u);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);

}

/*
* Hedge Modifying test
*/
template<>
void SpreaderFixtureTypedLight<false, true>::HedgeModifying_3_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

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

/*
* Hedge Modifying test
* hedge order should be modified but it waits till modify ack
*/
template<>
void SpreaderFixtureTypedLight<false, true>::HedgeModifying_11_Test() {
    uint nLegs = 2;

    std::vector<hf::symbol_info_t> symbols;
    hf::symbol_info_t symbolLeg1;
    symbolLeg1.gid = 0;
    symbolLeg1.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg1);
    symbols.push_back(symbolLeg1);

    hf::symbol_info_t symbolLeg2;
    symbolLeg2.gid = 1;
    symbolLeg2.tick_size = .25 * 10000000;
    setEnabled(_venue, symbolLeg2);
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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

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

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    sendOrderAccepted(*spread->getLegByIndex(0)._quoteOrder);
    sendOrderAccepted(*spread->getLegByIndex(1)._quoteOrder);

    auto *order1 = spread->getLegByIndex(0)._quoteOrder;
    auto *order2 = spread->getLegByIndex(1)._quoteOrder;

    sendPartialFill(*order1, 2);

    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);

    //ok we expect to have hedge 2
    EXPECT_EQ(order2->qty, 2);

    EXPECT_TRUE(order2->is_modifying);

    //leg2 order was filled by 5
    sendPartialFill(*order2, 5);

    //leg1 is filled by 2
    //leg2 is filled by 5
    //I expected to have leg1 hedge 3
    //or leg1 hedge 5 & leg2 hedge 2

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 1u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 1u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 5);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);

    //leg2 order was filled by 3
    sendPartialFill(*order2, 3);

    sendPartialFill(*order1, 1);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 2u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 3);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 8);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[1]._order->qty, 3);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);

    {
        bookDataBuy[0].pr = -1.75;
        bookDataBuy[0].q = 34;
        bookDataBuy[0].o = 0;
    }

    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);


    {
        bookDataSell[0].pr = -.25;
        bookDataSell[0].q = 42;
        bookDataSell[0].o = 0;
    }

    legIndex = 1;
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_NE(spread->getLegByIndex(0)._quoteOrder, nullptr);
    EXPECT_NE(spread->getLegByIndex(1)._quoteOrder, nullptr);

    EXPECT_EQ(spread->getLegByIndex(0)._quoteOrder->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._quoteOrder->qty, 7);

    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders.size(), 2u);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders.size(), 2u);

    EXPECT_EQ(spread->getLegByIndex(0)._filledQuantity, 3);
    EXPECT_EQ(spread->getLegByIndex(1)._filledQuantity, 8);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[0]._order->qty, 5);
    EXPECT_EQ(spread->getLegByIndex(0)._hedgeOrders[1]._order->qty, 3);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->qty, 2);
    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[1]._order->qty, 1);

    {
        bookDataBuy[0].pr = -2.75;
        bookDataBuy[0].q = 34;
        bookDataBuy[0].o = 0;
    }

    legIndex = 1;
    changeBuy = getBookChangeLevel(spread, legIndex, hf::SIDE::BUY, bookDataBuy);


    {
        bookDataSell[0].pr = -1.25;
        bookDataSell[0].q = 42;
        bookDataSell[0].o = 0;
    }

    legIndex = 1;
    changeSell = getBookChangeLevel(spread, legIndex, hf::SIDE::SELL, bookDataSell);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);

    sendAskBidMD(getSid(spread, legIndex), changeSell, changeBuy, bookDataSell, bookDataBuy);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -5000000);

    sendOrderModified(*spread->getLegByIndex(1)._hedgeOrders[0]._order, spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, spread->getLegByIndex(1)._hedgeOrders[0]._order->qty);

    EXPECT_EQ(spread->getLegByIndex(1)._hedgeOrders[0]._order->pr, -27500000);

}

/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* no ack
* Leg2 market moved
* Quote order changed
* leg2 market moved
* no changes
*/
TEST_F(SpreaderFixtureICEStandalone, MarketMove_NoAck_Test) {
    MarketMove_NoAck_Test();
}

/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* no ack
* Leg2 market moved
* leg1 order modified
* ack
* no change
* leg2 market moved
* no change
* modifying ack
* leg1 order modified
* leg2 market moved
* leg1 order modified
*/
TEST_F(SpreaderFixtureICEStandalone, MarketMove_NoAck_Ack_Test) {
    MarketMove_NoAck_Ack_Test();
}


/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* spread canceled
* order no changed
* ack
* order cancelling
* cancel ack
* state - Cancelled
*/
TEST_F(SpreaderFixtureICEStandalone, MarketMove_UserCancelled_Test) {
    MarketMove_UserCancelled_Test();
}

/*
* 2 legs
* Bid on leg 2 - leg1 quoted
* spread canceled
* order no changed
* ack
* order cancelling
* cancel ack
* state - Cancelled
*/
TEST_F(SpreaderFixtureICEStandalone, MarketMove_UnsolicitedCancelled_Test) {
    MarketMove_UnsolicitedCancelled_Test();
}

/*
* cancel after accepted
*/
TEST_F(SpreaderFixtureICEStandalone, CancelModifying_Test) {
    CancelModifying_Test();
}

/*
* cancel before accepted
*/
TEST_F(SpreaderFixtureICEStandalone, CancelImmediate_Test) {
    CancelImmediate_Test();
}

/*
* New Order rejected
*/
TEST_F(SpreaderFixtureICEStandalone, NO_Rejected_Test) {
    NO_Rejected_Test();
}

/*
* Modify Order rejected
*/
TEST_F(SpreaderFixtureICEStandalone, Modify_Rejected_Test) {
    Modify_Rejected_Test();
}

/*
* Modify Order rejected because of fill
*/
TEST_F(SpreaderFixtureICEStandalone, Modify_RejectedFilled_Test) {
    Modify_RejectedFilled_Test();
}

/*
* cancel rejected because of fill
*/
TEST_F(SpreaderFixtureICEStandalone, CancelRejected_Test) {
    CancelRejected_Test();
}


/*
* cancel rejected because of something
*/
TEST_F(SpreaderFixtureICEStandalone, UnsolicitedCancelRejected_Test) {
    UnsolicitedCancelRejected_Test();
}

TEST_F(SpreaderFixtureICEStandalone, QuoteToHedje_Test) {
    QuoteToHedge_Test();
}

TEST_F(SpreaderFixtureICEStandalone, HedgeModifying_1_Test) {
    HedgeModifying_1_Test();
}

TEST_F(SpreaderFixtureICEStandalone, HedgeModifying_2_Test) {
    HedgeModifying_2_Test();
}

TEST_F(SpreaderFixtureICEStandalone, HedgeModifying_3_Test) {
    HedgeModifying_3_Test();
}

TEST_F(SpreaderFixtureICEStandalone, HedgeModifying_11_Test) {
    HedgeModifying_11_Test();
}

}
}