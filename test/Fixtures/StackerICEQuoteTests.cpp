//
// Created by cmilutinovic on 7/23/19.
//

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "StackerFixture.h"

using ::testing::_;

//For reference:
// void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0, int overfill = INT_MIN)


namespace {
    constexpr static auto rejectCode = om::toUType(om::OrderRejectReason::OrderNotActive);
}

namespace Python {

    TEST_F(StackerFixture, ICEQuoteTests_NO_MO_CO) {

        //Boilerplate init code, quoting 10@0.35
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
        ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
        const om::order_t& order =  *stacker->_topLevel->_orders[0];
        const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
        const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
        const OrderManager<TestIME_2>::Level* l33 = l34->after;

        //level, price, target, acked, flight, traded, [flighout], [overfill]
        assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

        onOrderAccepted(stacker, 10, order);
        assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

        //Modify
        stacker->quote<BID>(ToRhonePrice(0.34), 9, false);
        assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0, 10);
        assertState(l34, ToRhonePrice(0.34), 9, 0, 9, 0);

        //Modify Ack
        onOrderReplaced(stacker, 9, order);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0);
        assertState(l34, ToRhonePrice(0.34), 9, 9, 0, 0);

        //Cancel
        stacker->quote<BID>(ToRhonePrice(0.34), 0, false);
        assertState(l34, ToRhonePrice(0.34), 0, 9, -9, 0);

        //Cancel Ack
        onOrderCanceled(stacker, order);
        assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0);

        ASSERT_EQ(l34->_orders.size(), 0);
    }

    TEST_F(StackerFixture, ICEQuoteTests_NO_MO_CO_With_Partial) {
        //Same as NO_MO_CO test except we're going to get a partial fill before the Modify
        //Boilerplate init code, quoting 10@0.35
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
        ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
        const om::order_t& order =  *stacker->_topLevel->_orders[0];
        const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
        const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
        const OrderManager<TestIME_2>::Level* l33 = l34->after;

        //level, price, target, acked, flight, traded, [flighout], [overfill]
        assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

        onOrderAccepted(stacker, 10, order);
        assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

        //Partial Fill
        onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
        assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 3, 0);

        //Modify
        stacker->quote<BID>(ToRhonePrice(0.34), 9, false);
        assertState(l35, ToRhonePrice(0.35), 0, 7, -7, 0, 7);
        assertState(l34, ToRhonePrice(0.34), 9, 0, 9, 0);
        ASSERT_EQ(l34->_orders.size(), 1);
        ASSERT_EQ(l34->_orders[0]->qty, 9 + 3);

        //Modify Ack
        onOrderReplaced(stacker, 12, order);
        assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0);
        assertState(l34, ToRhonePrice(0.34), 9, 9, 0, 0);

        //Cancel
        stacker->quote<BID>(ToRhonePrice(0.34), 0, false);
        assertState(l34, ToRhonePrice(0.34), 0, 9, -9, 0);

        //Cancel Ack
        onOrderCanceled(stacker, order);
        assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0);

        ASSERT_EQ(l34->_orders.size(), 0);
    }

    TEST_F(StackerFixture, ICEQuoteTests_NO_MO_CO_With_PartialInFlight) {
        //Same as NO_MO_CO test except we're going to get a partial fill in flight with the modify
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
        ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
        const om::order_t& order =  *stacker->_topLevel->_orders[0];
        const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
        const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
        const OrderManager<TestIME_2>::Level* l33 = l34->after;

        //level, price, target, acked, flight, traded, [flighout], [overfill]
        assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

        onOrderAccepted(stacker, 10, order);
        assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

        //Modify
        stacker->quote<BID>(ToRhonePrice(0.34), 9, false);
        assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0, 10);
        assertState(l34, ToRhonePrice(0.34), 9, 0, 9, 0);

        //Partial Fill In Flight
        onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
        assertState(l35, ToRhonePrice(0.35), 0, 10, -7, 3, 7);
        assertState(l34, ToRhonePrice(0.34), 9, 0, 6, 0);

        //Modify Ack
        onOrderReplaced(stacker, 9, order);
        assertState(l35, ToRhonePrice(0.35), 0, 3, 0, 3);
        assertState(l34, ToRhonePrice(0.34), 9, 6, 3, 0); //Note a NO was sent
        ASSERT_EQ(l34->_orders.size(), 2);
        ASSERT_EQ(l34->_orders[1]->qty, 3);

        //Cancel
        stacker->quote<BID>(ToRhonePrice(0.34), 0, false);
        //Note we got a CO in flight with a NO here, which is allowed for ICE.
        //Also note that acked has decremented to 6 since that NO->CO is decrementing acked quantity.
        assertState(l35, ToRhonePrice(0.35), 0, 3, 0, 3);  //If we quote(qty=0@0.35) the ack/trade would net out
        assertState(l34, ToRhonePrice(0.34), 0, 6, -6, 0);

        //Cancel Ack
        onOrderCanceled(stacker, order);
        ASSERT_EQ(l34->_orders.size(), 1);
        assertState(l35, ToRhonePrice(0.35), 0, 3, 0, 3);
        assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 0, 0);

        //NO/CO in flight
        onOrderAccepted(stacker, 3, *l34->_orders[0]);
        assertState(l35, ToRhonePrice(0.35), 0, 3, 0, 3);
        assertState(l34, ToRhonePrice(0.34), 0, 3, -3, 0);

        //Second Cancel Ack
        onOrderCanceled(stacker, *l34->_orders[0]);
        assertState(l35, ToRhonePrice(0.35), 0, 3, 0, 3);
        assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0);
        ASSERT_EQ(l34->_orders.size(), 0);
    }

TEST_F(StackerFixture, ICEQuoteTests_NO_MO_CO_With_FullFillInFlight) {
    //Same as NO_MO_CO test except we're going to get a partial fill in flight with the modify
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.34), 9, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0, 10);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 9, 0);

    //Full Fill In Flight
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 10, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10, 0);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 0, 0);

    //Modify Reject
    onOrderReplaceRejected(stacker, order,  2045);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10, 0);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 0, 0);

    //Let's make sure that 10/10 actually does cancel out on a new quote
    stacker->quote<BID>(ToRhonePrice(0.35), 2, false);
    assertState(l35, ToRhonePrice(0.35), 2, 0, 2, 0, 0);
    ASSERT_EQ(l35->_orders.size(), 1);
}

TEST_F(StackerFixture, ICEQuoteTests_NO_MO_CO_With_PartilFillThenFullFillInFlight) {
    //Same as NO_MO_CO test except we're going to get a partial fill in flight with the modify
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.34), 9, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0, 10);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 9, 0);

    //Partial Fill In Flight
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -7, 3, 7);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 6, 0);

    //Then a full fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 7, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10, 0);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 0, 0);

    //Modify Reject
    onOrderReplaceRejected(stacker, order,  2045);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10, 0);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 0, 0);

    //Let's make sure that 10/10 actually does cancel out on a new quote
    stacker->quote<BID>(ToRhonePrice(0.35), 2, false);
    assertState(l35, ToRhonePrice(0.35), 2, 0, 2, 0, 0);
    ASSERT_EQ(l35->_orders.size(), 1);
}

TEST_F(StackerFixture, ICEQuoteTests_NO_CO) {
    //Place a new order and cancel it.  Yes, that's it.
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Cancel
    stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0);

    //Cancel Ack
    onOrderCanceled(stacker, order);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0);
}

TEST_F(StackerFixture, ICEQuoteTests_NO_CO_PartialInFlight) {
    //Place a new order and cancel it.  With a partial fill in flight.
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Cancel
    stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0);

    //Partial Fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -7, 3);

    //Partial Fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -4, 6);

    //Cancel Ack
    onOrderCanceled(stacker, order);
    assertState(l35, ToRhonePrice(0.35), 0, 6, 0, 6);  //Will net out on next quote
}

TEST_F(StackerFixture, ICEQuoteTests_NO_CO_PartialThenFullInFlight) {
    //Place a new order and cancel it.  With a partial then a full fill in flight
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Cancel
    stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0);

    //Partial Fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -7, 3);

    //Partial Fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -4, 6);

    //Full fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 4, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10);

    //Cancel Ack
    onOrderCanceled(stacker, order);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10);  //Will net out on next quote
}

TEST_F(StackerFixture, ICEQuoteTests_NO_CO_PartialThenFullInFlightWithIntermediaryQuote) {
    //Place a new order and cancel it.  With a partial then a full fill in flight
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Cancel
    stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0);

    //Partial Fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -7, 3);

    //Partial Fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 3, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -4, 6);

    //We're going to quote here to see what happens
    stacker->quote<BID>(ToRhonePrice(0.35), 9, false);
    assertState(l35, ToRhonePrice(0.35), 9, 4, 5, 0);  //Ack/Traded netted out, then _flight increased by 9

    //Full fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 4, false);
    assertState(l35, ToRhonePrice(0.35), 9, 4, 9, 4);

    //Cancel Reject
    onOrderCancelRejected(stacker, order, rejectCode);
    assertState(l35, ToRhonePrice(0.35), 9, 4, 9, 4);  //Will net out on next quote
}

TEST_F(StackerFixture, ICEQuoteTests_NO_CO_FullFillInFlight) {
    //Place a new order and cancel it.  With a partial then a full fill in flight
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Cancel
    stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0);

    //Full fill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 10, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10);

    //Cancel Reject
    onOrderCancelRejected(stacker, order, rejectCode);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10);  //Will net out on next quote
}

TEST_F(StackerFixture, ICEQuoteTests_NO_MO_OverfillInFlight_PartialFill_DiffLevel) {
    //Place a new order and modify it to a new price level.  With a partial fill that overfills.
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.34), 7, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0, 10);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 7, 0);

    //Partial Fill In Flight that overfill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 8, true);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -2, 8, 2);
    assertState(l34, ToRhonePrice(0.34), 7, 0, -1, 0, 0, 1);

    //Cancel due to overfill
    onOrderCanceled(stacker, order);
    assertState(l35, ToRhonePrice(0.35), 0, 8, 0, 8, 0);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 0, 0, 0, 0);
}

TEST_F(StackerFixture, ICEQuoteTests_NO_MO_OverfillInFlight_FullFill_DiffLevel) {
    //Place a new order and modify it to a new price level.  With a full fill that overfills.
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.34), 7, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, -10, 0, 10);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 7, 0);

    //Full Fill In Flight that overfill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 10, false);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10, 0);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 0, 0, 0, 0);

    //Modify Reject
    onOrderReplaceRejected(stacker, order, rejectCode);
    assertState(l35, ToRhonePrice(0.35), 0, 10, 0, 10, 0);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 0, 0, 0, 0);
}

TEST_F(StackerFixture, ICEQuoteTests_NO_MO_OverfillInFlight_PartialFill_SameLevel) {
    //Place a new order and modify it to the same price level.  With a partial fill that overfills.
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.35), 7, false);
    assertState(l35, ToRhonePrice(0.35), 7, 10, -3, 0);

    //Partial Fill In Flight that overfill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 8, true);
    assertState(l35, ToRhonePrice(0.35), 7, 10, -3, 8, 0, 1);

    //Cancel due to overfill
    onOrderCanceled(stacker, order);
    assertState(l35, ToRhonePrice(0.35), 7, 8, 0, 8, 0);
}

TEST_F(StackerFixture, ICEQuoteTests_NO_MO_OverfillInFlight_FullFill_SameLevel) {
    //Place a new order and modify it to a new price level.  With a full fill that overfills.
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.35), 7, false);
    assertState(l35, ToRhonePrice(0.35), 7, 10, -3, 0);

    //Full Fill In Flight that overfill
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 10, false);
    assertState(l35, ToRhonePrice(0.35), 7, 10, 0, 10, 0);

    //Modify Reject
    onOrderReplaceRejected(stacker, order, rejectCode);
    assertState(l35, ToRhonePrice(0.35), 7, 10, 0, 10, 0);
}

TEST_F(StackerFixture, ICE_NO_SMP) {
    //Place a new order, which will have a SMP reject
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    //On ICE, this is a NO Reject
    onOrderRejected(stacker, order, -1);
    assertState(l35, ToRhonePrice(0.35), 10, 0, 0, 0);
}

TEST_F(StackerFixture, ICE_NO_SMP_With_Modify_In_Flight_SameLevel) {
    //Place a new order, with a modify in flight, which will have a SMP reject
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    //Send a modify
    stacker->quote<BID>(ToRhonePrice(0.35), 7, false);
    assertState(l35, ToRhonePrice(0.35), 7, 0, 7, 0);

    //Reject
    onOrderRejected(stacker, order, -1);
    assertState(l35, ToRhonePrice(0.35), 7, 0, 0, 0);

    //Modify Reject
    onOrderReplaceRejected(stacker, order, rejectCode);
    assertState(l35, ToRhonePrice(0.35), 7, 0, 0, 0, 0);

}

TEST_F(StackerFixture, ICE_NO_SMP_With_Modify_In_Flight_DiffLevel) {
    //Place a new order, with a modify in flight, which will have a SMP reject
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    //Send a modify
    stacker->quote<BID>(ToRhonePrice(0.34), 7, false);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 10);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 7, 0);

    //Reject
    onOrderRejected(stacker, order, -1);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 0, 0);

    //Modify Reject
    onOrderReplaceRejected(stacker, order, rejectCode);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 7, 0, 0, 0);
}

TEST_F(StackerFixture, ICE_NO_SMP_With_Cancel_In_Flight) {
    //Place a new order, with a modify in flight, which will have a SMP reject
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    //Send a Cancel
    stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);

    //Reject
    onOrderRejected(stacker, order, -1);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);

    //Cancel Reject
    onOrderCancelRejected(stacker, order, rejectCode);
    assertState(l35, ToRhonePrice(0.35), 0, 0, 0, 0, 0);
}

void StackerFixture::ice_mo_smp_diff_level(ice_mo_smp_test_mode mode) {
    //Place a new order, with a modify in flight, which will have a SMP reject
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;
    const OrderManager<TestIME_2>::Level* l33 = l34->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //A fill just to make this test more interesting
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 2, true);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 2);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.34), 9, false);
    assertState(l35, ToRhonePrice(0.35), 0, 8, -8, 0, 8);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 9, 0);

    //SMP Reject -- Will result in cancel
    onOrderReplaceRejected(stacker, order, -1, "Trade not allowed - Counterparty is from your company or your own order. Offending Orders : 12345678");
    assertState(l35, ToRhonePrice(0.35), 0, 8, -8, 0, 0);
    assertState(l34, ToRhonePrice(0.34), 9, 0, 0, 0);

    //Then . . .

    switch(mode) {
        case ice_mo_smp_test_mode::MO_SAME: {
            stacker->quote<BID>(ToRhonePrice(0.35), 4, false);
            assertState(l35, ToRhonePrice(0.35), 4, 8, -4, 0, 0);
            assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 0);
            assertState(l33, ToRhonePrice(0.33), 0, 0, 0, 0, 0);
        }
            break;
        case ice_mo_smp_test_mode::MO_DIFF: {
            stacker->quote<BID>(ToRhonePrice(0.34), 4, false);
            assertState(l35, ToRhonePrice(0.35), 0, 8, -8, 0, 0);
            assertState(l34, ToRhonePrice(0.34), 4, 0, 4, 0, 0);
            assertState(l33, ToRhonePrice(0.33), 0, 0, 0, 0, 0);
        }
            break;
        case ice_mo_smp_test_mode::CO: {
            stacker->quote<BID>(ToRhonePrice(0.34), 0, false);
            assertState(l35, ToRhonePrice(0.35), 0, 8, -8, 0, 0);
            assertState(l34, ToRhonePrice(0.34), 0, 0, 0, 0, 0);
            assertState(l33, ToRhonePrice(0.33), 0, 0, 0, 0, 0);
        }
            break;
        case ice_mo_smp_test_mode::MO_DIFF_THEN_REJECT_CO: {
            //First we cancel (same as case above)
            stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
            assertState(l35, ToRhonePrice(0.35), 0, 8, -8, 0, 0);
            //But the cancel is going to be rejected.  Not sure what we want to do, we probably don't want to cancel
            //again right away if it was a halt.
            onOrderCancelRejected(stacker, order, -1);
            ASSERT_TRUE(order.action_flags.is_cancelling);  //It's expected to fail this test right now
        }
            break;
        case ice_mo_smp_test_mode::NOTHING:
            break;
    }
}

void StackerFixture::ice_mo_smp_same_level(StackerFixture::ice_mo_smp_test_mode mode) {
    //Place a new order, with a modify in flight, which will have a SMP reject
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.35), 10, false);
    ASSERT_EQ(stacker->_topLevel->_orders.size(), 1);
    const om::order_t& order =  *stacker->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* l35 = stacker->_topLevel;
    const OrderManager<TestIME_2>::Level* l34 = stacker->_topLevel->after;

    //level, price, target, acked, flight, traded, [flighout], [overfill]
    assertState(l35, ToRhonePrice(0.35), 10, 0, 10, 0);

    onOrderAccepted(stacker, 10, order);
    assertState(l35, ToRhonePrice(0.35), 10, 10, 0, 0);

    //Modify
    stacker->quote<BID>(ToRhonePrice(0.35), 7, false);
    assertState(l35, ToRhonePrice(0.35), 7, 10, -3, 0);

    //A fill just to make this test more interesting
    onOrderExecuted(stacker, order, ToRhonePrice(.35), 2, true);
    assertState(l35, ToRhonePrice(0.35), 7, 10, -3, 2);

    //SMP Reject
    onOrderReplaceRejected(stacker, order, -1, "Trade not allowed - Counterparty is from your company or your own order. Offending Orders : 12345678");
    assertState(l35, ToRhonePrice(0.35), 7, 10, 0, 2);

    //Then . . .

    switch(mode) {
        case ice_mo_smp_test_mode::MO_SAME: {
            stacker->quote<BID>(ToRhonePrice(0.35), 6, false);
            assertState(l35, ToRhonePrice(0.35), 6, 8, -2, 0);
        }
            break;
        case ice_mo_smp_test_mode::MO_DIFF: {
            stacker->quote<BID>(ToRhonePrice(0.34), 6, false);
            assertState(l35, ToRhonePrice(0.35), 0, 8, -8, 0, 8);
            assertState(l34, ToRhonePrice(0.34), 6, 0, 6, 0, 0);
        }
            break;
        case ice_mo_smp_test_mode::CO: {
            stacker->quote<BID>(ToRhonePrice(0.35), 0, false);
            assertState(l35, ToRhonePrice(0.35), 0, 8, -8, 0, 0);
        }
            break;
        case ice_mo_smp_test_mode::MO_DIFF_THEN_REJECT_CO : {
            ASSERT_EQ(true, false); //Test makes no sense for same level
        }
            break;
        case ice_mo_smp_test_mode::NOTHING:
            break;
    }
}

TEST_F(StackerFixture, ICE_MO_SMP_DIFF_LEVEL) {
    ice_mo_smp_diff_level(StackerFixture::ice_mo_smp_test_mode::NOTHING);
}

TEST_F(StackerFixture, ICE_MO_SMP_DIFF_LEVEL_Then_MO_SAME) {
    ice_mo_smp_diff_level(StackerFixture::ice_mo_smp_test_mode::MO_SAME);
}

TEST_F(StackerFixture, ICE_MO_SMP_DIFF_LEVEL_Then_MO_DIFF) {
    ice_mo_smp_diff_level(StackerFixture::ice_mo_smp_test_mode::MO_DIFF);
}

TEST_F(StackerFixture, ICE_MO_SMP_DIFF_LEVEL_Then_CO) {
    ice_mo_smp_diff_level(StackerFixture::ice_mo_smp_test_mode::CO);
}

TEST_F(StackerFixture, ICE_MO_SMP_DIFF_LEVEL_Then_MO_DIFF_THEN_REJECT_CO) {
    ice_mo_smp_diff_level(StackerFixture::ice_mo_smp_test_mode::MO_DIFF_THEN_REJECT_CO);
}

TEST_F(StackerFixture, ICE_MO_SMP_SAME_LEVEL) {
    ice_mo_smp_same_level(StackerFixture::ice_mo_smp_test_mode::NOTHING);
}

TEST_F(StackerFixture, ICE_MO_SMP_SAME_LEVEL_Then_MO_SAME) {
    ice_mo_smp_same_level(StackerFixture::ice_mo_smp_test_mode::MO_SAME);
}

TEST_F(StackerFixture, ICE_MO_SMP_SAME_LEVEL_Then_MO_DIFF) {
    ice_mo_smp_same_level(StackerFixture::ice_mo_smp_test_mode::MO_DIFF);
}

TEST_F(StackerFixture, ICE_MO_SMP_SAME_LEVEL_Then_CO) {
    ice_mo_smp_same_level(StackerFixture::ice_mo_smp_test_mode::CO);
}

}