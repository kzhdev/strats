//
// Created by cmilutinovic on 7/21/19.
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "StackerFixture.h"

using ::testing::_;

namespace {
    // Unsure whether this ought to be true or false (or even a mix).  But for now...
    // Used as third parameter for all stacker::quote functions below
    constexpr bool goThroughAllLevels = false;
}

namespace Python {


TEST_F(StackerFixture, OverfillNotZero) {
        //Trying to recreate a specific prod bug:
        // Modifies in flight 4@.35 -> 11@.34 -> 4@.35 (diff Price)
        // We get: OM (11)
        // OE (6) @ 0.34
        // OC //Overfilled by 2

        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::CME);
        stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

        stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
        ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
        const om::order_t& order =  *stacker->_topLevel->_orders[0];
        const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
        const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
        onOrderAccepted(stacker, 4, order);
        //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
        assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

        stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
        stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
        assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
        assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);

        onOrderReplaced(stacker, 11, order);
        assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
        assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);

        //Here comes the overfill
        onOrderExecuted(stacker, order, ToRhonePrice(0.34), 6, true);

        //                                   tar ack  fli tra flio
        assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  5);
        assertState(l35, ToRhonePrice(0.35),   4,  0,   -2,  0,  0, 2);

//        //OM event goes here
//        onOrderReplaced(stacker, 4, order);
//        //                                   tar ack  fli tra flio
//        assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
//        assertState(l35, ToRhonePrice(0.35),   4, -2,   4,  0,  0, 2);
//        ASSERT_EQ(l35->_orders.size(), 2); //Should have sent an order for qty=4, hence flight=4

        onOrderCanceled(stacker, order);
        //                                   tar ack  fli tra flio
        assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
        assertState(l35, ToRhonePrice(0.35),   4,  0,   0,  0,  0);
//        ASSERT_EQ(l35->_orders.size(), 1); //Should have sent an order for qty=4, hence flight=4

        //TODO: Do this test also /w CancelQuantityInChain involved
        //TODO: Should onOrderCanclled call syncOrders?
}

TEST_F(StackerFixture, OverfillNotZero_PingPongLevels) {

    //Just like above, but after the mod to the same level, we will do 2 more modifies and cycle again

    //Trying to recreate a specific prod bug:
    // Modifies in flight 4@.35 -> 11@.25 -> 4@.35 (diff Price)
    // We get: OM (11)
    // OE (6) @ 0.25
    // OC //Overfilled by 2

    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.34), 11, false); //new
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false); //new
    stacker->quote<BID>(ToRhonePrice(0.33), 1, false); //new

    //assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    //assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);
    assertState(l33, ToRhonePrice(0.33), 1, 0, 1, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 4, -4, 0, 12);

    onOrderReplaced(stacker, 11, order);
    //assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    //assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);
    assertState(l33, ToRhonePrice(0.33), 1,  0,  1,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 8);

    //Here comes the overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 6, true);

    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  5);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,  -2,  0,  0);
    //                                   tar ack  fli tra flio
    assertState(l33, ToRhonePrice(0.33),   1,  0,  -5,  0,   0, 5);
    assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  10);
    assertState(l35, ToRhonePrice(0.35),   0,  0,  0,  0,  -4);

    //OM event goes here
    onOrderReplaced(stacker, 4, order);
    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
    //assertState(l35, ToRhonePrice(0.35),   4, -2,   0,  0,  0);
    //                                   tar ack  fli tra flio
    assertState(l33, ToRhonePrice(0.33),   1,  0,  -4,  0,   0, 5);
    assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  5);
    assertState(l35, ToRhonePrice(0.35),   0, -2,   2,  0,  -4);
    ASSERT_EQ(l33->_orders.size(), 2); //Should have sent an order for qty=4, hence flight=4.
    ASSERT_EQ(l33->_orders[1]->qty, 1);

    onOrderCanceled(stacker, order);
    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,   0,  0,  0);

    const auto& checkFinalState = [this, &l33, &l34, &l35]() {
        //                                   tar ack  fli tra flio
        assertState(l33, ToRhonePrice(0.33), 1, 0, 1, 0, 0, 0);
        assertState(l34, ToRhonePrice(0.34), 0, 6, 0, 6, 0);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
        ASSERT_EQ(l33->_orders.size(), 1); //One order left since we just canceled the other one
        ASSERT_EQ(l33->_orders[0]->qty, 1);
    };
    checkFinalState();

    //These modify rejects should do nothing
    onOrderReplaceRejected(stacker,order, 2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order,  2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order, 2045);
    checkFinalState();

    //TODO: Do this test also /w CancelQuantityInChain involved
    //TODO: Should onOrderCanclled call syncOrders?
}


TEST_F(StackerFixture, OverfillNotZero_PingPongLevels_WithFinalCancel) {

    //This is a copy and paste of the above test, except we're going to end the chain with a quote at qty=0 to make sure cancels are handled properly
    //with overfills

    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.33), 1, false);
    stacker->quote<BID>(ToRhonePrice(0.33), 0, false); //new

    //assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    //assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);
    assertState(l33, ToRhonePrice(0.33), 0, 0, 0, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 4, -4, 0, 12);

    onOrderReplaced(stacker, 11, order);
    //assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    //assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);
    assertState(l33, ToRhonePrice(0.33), 0,  0,  0,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 8);

    //Here comes the overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 6, true);

    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  5);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,  -2,  0,  0);
    //                                   tar ack  fli tra flio
    assertState(l33, ToRhonePrice(0.33),   0,  0,  0,  0,   0, 5);
    assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  10);
    assertState(l35, ToRhonePrice(0.35),   0,  0,  0,  0,  -4);

    onOrderCanceled(stacker, order);  //Modify to 4, eqty=6
    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,   0,  0,  0);

    const auto& checkFinalState = [this, &l33, &l34, &l35]() {
        //                                   tar ack  fli tra flio
        assertState(l33, ToRhonePrice(0.33), 0, 0, 0, 0, 0, 0);
        assertState(l34, ToRhonePrice(0.34), 0, 6, 0, 6, 0);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
        ASSERT_EQ(l33->_orders.size(), 0); //One order left since we just canceled the other one
    };
    checkFinalState();

    //These modify rejects should do nothing
    onOrderReplaceRejected(stacker,order, 2045);
    checkFinalState();
    onOrderReplaceRejected(stacker,order, 2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order,  2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order, 2045);
    checkFinalState();

    //The whole reason for this test -- this better do the right thing!
    onOrderCancelRejected(stacker, order, 2045);
    checkFinalState();

    //These TODO's are Copy & Pasted from above

    //TODO: Do this test also /w CancelQuantityInChain involved
    //TODO: Should onOrderCanclled call syncOrders?  --No, but onOrderModified should
}

TEST_F(StackerFixture, OverfillNotZero_PingPongLevels_WithFullFill) {

    //Same test as OverfillNotZero_PingPongLevels, except with a full fill instead of partial

    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.34), 11, false); //new
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false); //new
    stacker->quote<BID>(ToRhonePrice(0.33), 1, false); //new

    //assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    //assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);
    assertState(l33, ToRhonePrice(0.33), 1, 0, 1, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 4, -4, 0, 12);

    onOrderReplaced(stacker, 11, order);
    //assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    //assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);
    assertState(l33, ToRhonePrice(0.33), 1,  0,  1,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 8);

    //Here comes the full fill, which is an overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 11, false);
    assertState(l33, ToRhonePrice(0.33),   1,  0,  0,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34),   0, 11,  0,  11,  0);
    assertState(l35, ToRhonePrice(0.35),   0,  0,  0,  0,  0);

    onOrderReplaceRejected(stacker,order, 2045);
    const auto& checkFinalState = [this, &l33, &l34, &l35]() {
        //                                   tar ack  fli tra flio
        assertState(l33, ToRhonePrice(0.33), 1, 0, 0, 0, 0, 0);
        assertState(l34, ToRhonePrice(0.34), 0, 11, 0, 11, 0);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
    };
    checkFinalState();

    //These modify rejects should do nothing
    onOrderReplaceRejected(stacker,order, 2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order,  2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order, 2045);
    checkFinalState();
}

TEST_F(StackerFixture, OverfillNotZero_PingPongLevels_WithPartialFillThenFullFill) {

    //Same test as OverfillNotZero_PingPongLevels_WithFullFill, except with a partial then a full fill

    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.34), 11, false); //new
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false); //new
    stacker->quote<BID>(ToRhonePrice(0.33), 1, false); //new

    //assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    //assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);
    assertState(l33, ToRhonePrice(0.33), 1, 0, 1, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 4, -4, 0, 12);

    onOrderReplaced(stacker, 11, order);
    //assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    //assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);
    assertState(l33, ToRhonePrice(0.33), 1,  0,  1,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 8);

    //Here comes the partial fill, which is an overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 6, true);
    assertState(l33, ToRhonePrice(0.33),   1,  0,  -5,  0,   0, 5);
    assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  10);
    assertState(l35, ToRhonePrice(0.35),   0,  0,  0,  0,  -4);

    //Here comes the full fill, which is an overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 5, false);
    assertState(l33, ToRhonePrice(0.33),   1,  0,  0,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34),   0, 11,  0,  11,  0);
    assertState(l35, ToRhonePrice(0.35),   0,  0,  0,  0,  0);

    onOrderReplaceRejected(stacker,order, 2045);
    const auto& checkFinalState = [this, &l33, &l34, &l35]() {
        //                                   tar ack  fli tra flio
        assertState(l33, ToRhonePrice(0.33), 1, 0, 0, 0, 0, 0);
        assertState(l34, ToRhonePrice(0.34), 0, 11, 0, 11, 0);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
    };
    checkFinalState();

    //These modify rejects should do nothing
    onOrderReplaceRejected(stacker,order, 2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order,  2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order, 2045);
    checkFinalState();
}






//
//TEST_F(StackerFixture, OverfillNotZero_PingPongLevels_ModDownOnLastLevel_PartialFill) {
//
//
//    //Similar OverfillNotZero_PingPongLevels, except with a Modify Down that crosses the overfill qty.
//    //We also will have a partial fill before the overfill
//
//    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
//    _ime2->setVenue(hf::VENUE::CME);
//    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2);
//    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
//
//    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
//    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
//    const om::order_t& order =  *stacker->_topLevel->_orders[0];
//    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
//    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
//    const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
//    onOrderAccepted(stacker, 4, order);
//    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
//    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);
//
//    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
//    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
//    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
//    stacker->quote<BID>(ToRhonePrice(0.34), 2, false);
//    stacker->quote<BID>(ToRhonePrice(0.34), 12, false);
//    stacker->quote<BID>(ToRhonePrice(0.34), 14, false);
//
//    assertState(l34, ToRhonePrice(0.34), 14, 0, 14, 0, 11);
//    assertState(l35, ToRhonePrice(0.35), 0, 4, -4, 0, 8);
//
//    onOrderReplaced(stacker, 11, order);
//    assertState(l34, ToRhonePrice(0.34), 14, 11, 3, 0, 11);
//    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 4);
//
//    //Here comes a partial fill with a quantity that won't trigger the overfill
//    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 1, true);
//    assertState(l34, ToRhonePrice(0.34), 14, 11, 3, 1, 10);
//    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 3);
//
//    //Here is a fill that will generate an overfill
//    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 3, true);
//    assertState(l34, ToRhonePrice(0.34), 14, 11, 3, 4, 7, 1);
//    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
//
//    onOrderReplaced(stacker, 4, order);
//    assertState(l34, ToRhonePrice(0.34), 14, 4, 10, 4, 0, 1);
//    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
//
//    onOrderReplaced(stacker, 11, order);
//    assertState(l34, ToRhonePrice(0.34), 14, 11, 3, 4, 0, 1);
//    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
//
//    //Overfilled!
//    onOrderReplaced(stacker, 2, order);
//    assertState(l34, ToRhonePrice(0.34), 14, 2, 12, 4, 0, 1);
//    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
//
//    onOrderCanceled(stacker, order);
//    assertState(l34, ToRhonePrice(0.34), 14, 2, 3, 4, 0, 0);
//    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
//
//    const auto& checkFinalState = [this, &l34, &l35]() {
//        //                                   tar ack  fli tra flio
//        assertState(l34, ToRhonePrice(0.34), 0, 6, 0, 6, 0);
//        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
//    };
//    checkFinalState();
//    onOrderReplaceRejected(stacker,order, 2045);
//    checkFinalState();
//    onOrderReplaceRejected(stacker,order, 2045);
//    checkFinalState();
//
//}
//
//
//TEST_F(StackerFixture, OverfillNotZero_PingPongLevels_ModDownOnLastLevel_FullFill) {
//        ASSERT_EQ(true, false);
//}
//
//



//
//TEST_F(StackerFixture, ReproduceAssertOnOverfillWithSamePriceLevelModifyQtyDownInChain) {
//
//        //The following series of modifies produced na assert failure (BOOST_ASSERT(priorWorkingQty > 0); in foreachNodOnFullFill) which I believe to be an error
//
//
//
//
//
////This is a copy and paste of the above test, except we're going to end the chain with a quote at qty=0 to make sure cancels are handled properly
////with overfills
//
//OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
//_ime2->setVenue(hf::VENUE::CME);
//stacker->initialize(cmeContract, BID, config_60_0_0, _ime2);
//initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
//
//stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
//ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
//const om::order_t& order =  *stacker->_topLevel->_orders[0];
//const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
//const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
//const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
//onOrderAccepted(stacker, 4, order);
////void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
//assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);
//
//stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
//stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
//stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
//stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
//stacker->quote<BID>(ToRhonePrice(0.33), 1, false);
//stacker->quote<BID>(ToRhonePrice(0.33), 0, false); //new
//
//
//}

////////// Modify Down -> Cancel w/o ack
//////////////////////////////////////////////////////////////////////

/*
 * The non-corrected versions assumed an overfill would result in a modify ack, then a cancel.  In reality
 * they result in just a cancel.  The stacker can handle both -- even though theoretically the first will
 * never happen except MAYBE if a SMP triggers both SMP reject and overfill if modify successful.
 *
 * Note the three assets in "foreachNodeOnFullFill" in Stacker.h should never be false if
 * we never get a modify ack for an overfill.
 */

TEST_F(StackerFixture, OverfillNotZero_Corrected) {
    //Trying to recreate a specific prod bug:
    // Modifies in flight 4@.35 -> 11@.25 -> 4@.35 (diff Price)
    // We get: OM (11)
    // OE (6) @ 0.25
    // OC //Overfilled by 2

    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);

    onOrderReplaced(stacker, 11, order);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);

    //Here comes the overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 6, true);

    //                                   tar ack  fli tra flio
    assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  5);
    assertState(l35, ToRhonePrice(0.35),   4,  0,   -2,  0,  0, 2);

    //Removed OM event
    onOrderCanceled(stacker, order);
    //                                   tar ack  fli tra flio
    assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
    assertState(l35, ToRhonePrice(0.35),   4,  0,   0,  0,  0);


    onOrderReplaceRejected(stacker, order, 2045);
    //Same asserts, nothing should have changed
    //                                   tar ack  fli tra flio
    assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
    assertState(l35, ToRhonePrice(0.35),   4,  0,   0,  0,  0);

    //TODO: Do this test also /w CancelQuantityInChain involved
    //TODO: Should onOrderCanclled call syncOrders?

    int i = 0;

}

TEST_F(StackerFixture, Overfill_LastPartialFillLessThanTarget) {
    //Trying to recreate a specific prod bug:
    // Modifies in flight 4@.35 -> 11@.25 -> 4@.35 (diff Price)
    // We get: OM (11)
    // OE (6) @ 0.25
    // OC //Overfilled by 2

    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 2, false);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    assertState(l35, ToRhonePrice(0.35), 2, 4, -2, 0, 4);

    onOrderReplaced(stacker, 11, order);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    assertState(l35, ToRhonePrice(0.35), 2, 0, 2, 0, 0);

    //Here comes the overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 10, true);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -1, 10, 1);
    assertState(l35, ToRhonePrice(0.35), 2, 0, -8, 0, 0, 8);

    //And we end up with a full fill, where event.fillqty < target quantity
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 1, false);
    assertState(l34, ToRhonePrice(0.34), 0, 11, 0, 11, 0);
    assertState(l35, ToRhonePrice(0.35), 2, 0, 0, 0, 0, 0);

    onOrderReplaceRejected(stacker, order, 2045);
    onOrderReplaceRejected(stacker, order, 2045);
    onOrderReplaceRejected(stacker, order, 2045);

    //Same assert
    assertState(l34, ToRhonePrice(0.34), 0, 11, 0, 11, 0);
    assertState(l35, ToRhonePrice(0.35), 2, 0, 0, 0, 0, 0);
}


TEST_F(StackerFixture, OverfillNotZero_PingPongLevels_Corrected) {

    //Just like above, but after the mod to the same level, we will do 2 more modifies and cycle again

    //Trying to recreate a specific prod bug:
    // Modifies in flight 4@.35 -> 11@.25 -> 4@.35 (diff Price)
    // We get: OM (11)
    // OE (6) @ 0.25
    // OC //Overfilled by 2


    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.34), 11, false); //new
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false); //new
    stacker->quote<BID>(ToRhonePrice(0.33), 1, false); //new

    //assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    //assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);
    assertState(l33, ToRhonePrice(0.33), 1, 0, 1, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 4, -4, 0, 12);

    onOrderReplaced(stacker, 11, order);
    //assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    //assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);
    assertState(l33, ToRhonePrice(0.33), 1,  0,  1,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 8);

    //Here comes the overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 6, true);

    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  5);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,  -2,  0,  0);
    //                                   tar ack  fli tra flio
    assertState(l33, ToRhonePrice(0.33),   1,  0,  -5,  0,   0, 5);
    assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  10);
    assertState(l35, ToRhonePrice(0.35),   0,  0,  0,  0,  -4);

    onOrderCanceled(stacker, order);
    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,   0,  0,  0);

    const auto& checkFinalState = [this, &l33, &l34, &l35]() {
        //                                   tar ack  fli tra flio
        assertState(l33, ToRhonePrice(0.33), 1, 0, 0, 0, 0, 0);  //Without the modify, we don't have that 1 order in flight anymore
        assertState(l34, ToRhonePrice(0.34), 0, 6, 0, 6, 0);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
        //ASSERT_EQ(l33->_orders.size(), 1); //Without the modify, we don't have the sync() on the modify that got us here
        //ASSERT_EQ(l33->_orders[0]->qty, 1);
    };
    checkFinalState();

    //These modify rejects should do nothing
    onOrderReplaceRejected(stacker,order, 2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order,  2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order, 2045);
    checkFinalState();

    //TODO: Do this test also /w CancelQuantityInChain involved
    //TODO: Should onOrderCanclled call syncOrders?
}

TEST_F(StackerFixture, OverfillNotZero_PingPongLevels_WithFinalCancel_Corrected) {

    //This is a copy and paste of the above test, except we're going to end the chain with a quote at qty=0 to make sure cancels are handled properly
    //with overfills

    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = stacker->_topLevel->after->after;
    onOrderAccepted(stacker, 4, order);
    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0) {
    assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0);

    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.34), 11, false);
    stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
    stacker->quote<BID>(ToRhonePrice(0.33), 1, false);
    stacker->quote<BID>(ToRhonePrice(0.33), 0, false); //new

    //assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 11); //The +/- flight balance exactly, FO is critical here
    //assertState(l35, ToRhonePrice(0.35), 4, 4, 0, 0, 4);
    assertState(l33, ToRhonePrice(0.33), 0, 0, 0, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 4, -4, 0, 12);

    onOrderReplaced(stacker, 11, order);
    //assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 11);
    //assertState(l35, ToRhonePrice(0.35), 4, 0, 4, 0, 0);
    assertState(l33, ToRhonePrice(0.33), 0,  0,  0,  0,   0, 0);
    assertState(l34, ToRhonePrice(0.34), 0, 11, -11, 0, 22);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 8);

    //Here comes the overfill
    onOrderExecuted(stacker, order, ToRhonePrice(0.34), 6, true);

    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  5);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,  -2,  0,  0);
    //                                   tar ack  fli tra flio
    assertState(l33, ToRhonePrice(0.33),   0,  0,  0,  0,   0, 5);
    assertState(l34, ToRhonePrice(0.34),   0, 11,  -5,  6,  10);
    assertState(l35, ToRhonePrice(0.35),   0,  0,  0,  0,  -4);

    onOrderCanceled(stacker, order);
    //                                   tar ack  fli tra flio
    //assertState(l34, ToRhonePrice(0.34),   0,  6,   0,  6,  0);
    //assertState(l35, ToRhonePrice(0.35),   4,  0,   0,  0,  0);

    const auto& checkFinalState = [this, &l33, &l34, &l35]() {
        //                                   tar ack  fli tra flio
        assertState(l33, ToRhonePrice(0.33), 0, 0, 0, 0, 0, 0);
        assertState(l34, ToRhonePrice(0.34), 0, 6, 0, 6, 0);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
        ASSERT_EQ(l33->_orders.size(), 0); //One order left since we just canceled the other one
    };
    checkFinalState();

    //These modify rejects should do nothing
    onOrderReplaceRejected(stacker,order, 2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order,  2045);
    checkFinalState();
    onOrderReplaceRejected(stacker, order, 2045);
    checkFinalState();

    //The whole reason for this test -- this better do the right thing!
    onOrderCancelRejected(stacker, order, 2045);
    checkFinalState();

    //These TODO's are Copy & Pasted from above

    //TODO: Do this test also /w CancelQuantityInChain involved
    //TODO: Should onOrderCanclled call syncOrders?  --No, but onOrderModified should
}



} //namespace Python