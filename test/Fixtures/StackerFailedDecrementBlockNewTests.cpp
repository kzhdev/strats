//
// Created by cmilutinovic on 12/17/19.
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

    TEST_F(StackerFixture, FD_CME_NewOrder) {
        //Verify all the modifies in the chain, when acked, execute as expected
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        OrderManager<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        performKnownGoodModifyAcks(orders);

//        price=29000000 _target=40 _ack=40 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=3 40@29000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=28000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=1 60@28000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=27000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=2 60@27000000 active=1 acked=1 cancelling=0 modifying=0}

        hf::price_t bidPrice = ToRhonePrice(.29);
        hf::qty_t quoteQty = 40;

        //Create and modify down shouldn't be blocked
        //void assertLevelState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0, int overfill = INT_MIN) {
        stacker->quote<BID>(bidPrice, quoteQty+2, goThroughAllLevels);
        assertLevelState(levels, ToRhonePrice(.29), quoteQty+2, 40, 2, 0, 0, 0);

        //We might have an order in flight, but we have an acked order of qty=40 to decrement from so this shouldn't block
        stacker->quote<BID>(bidPrice, quoteQty-2, goThroughAllLevels);
        assertLevelState(levels, ToRhonePrice(.29), quoteQty-2, 40, -2, 0, 0, 0);
        ASSERT_EQ(findLevel(levels, ToRhonePrice(.29))->_orders.size(), 2u);
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Now we're going to quote with qty=1.  This should block because we can't mod from 2 to 1 without the ack
        stacker->quote<BID>(bidPrice, 1, goThroughAllLevels);

        assertLevelState(levels, ToRhonePrice(.29), 1, 40, -38, 0, 0, 0);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Now we're going to quote at .30, which should try to slack but can't
        stacker->quote<BID>(30000000, 40, false);

        assertLevelState(levels, ToRhonePrice(.30), 40, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, -38, 0, 0, 0, {3,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0, {2});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Let's ack the new that blocked the decrement.  We should be all good on blockers after that.  This will generate 2 new orders
        onOrderAccepted(stacker, 2, *getOrder(stacker, 4));
        assertLevelState(levels, ToRhonePrice(.30), 40, 0, 40, 0, 0, 0, {6});  //New order, oid=6
        assertLevelState(levels, ToRhonePrice(.29), 60, 42, 18, 0, 0, 0, {3,4,5}); //New order, oid=5 -- still a mod/cancel in flight
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0,{2});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Let's ack the modify
        onOrderReplaced(stacker, 36, *getOrder(stacker, 3));
        assertLevelState(levels, ToRhonePrice(.30), 40, 0, 40, 0, 0, 0, {6});
        assertLevelState(levels, ToRhonePrice(.29), 60, 38, 22, 0, 0, 0, {3,4,5});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0, {2});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Then let's ack the cancel
        onOrderCanceled(stacker, *getOrder(stacker, 3));
        assertLevelState(levels, ToRhonePrice(.30), 40, 0, 40, 0, 0, 0, {6});
        assertLevelState(levels, ToRhonePrice(.29), 60, 2, 58, 0, 0, 0, {4,5});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0, {2});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Then ack the news
        onOrderAccepted(stacker, 58, *getOrder(stacker, 5));
        assertLevelState(levels, ToRhonePrice(.30), 40, 0, 40, 0, 0, 0, {6});
        assertLevelState(levels, ToRhonePrice(.29), 60, 60, 0, 0, 0, 0, {4,5});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0, {2});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        onOrderAccepted(stacker, 40, *getOrder(stacker, 6));
        assertLevelState(levels, ToRhonePrice(.30), 40, 40, 0, 0, 0, 0, {6});
        assertLevelState(levels, ToRhonePrice(.29), 60, 60, 0, 0, 0, 0, {4,5});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0, {2});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    //Verify nothing happens when a level that is blocked gets requoted.  Really an aborted attempt to write a test with multiple blocked orders.
    TEST_F(StackerFixture, FD_CME_NewOrder_RequoteWithoutOrderDoesNothing) {

        //Boilerplate
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        OrderManager<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        performKnownGoodModifyAcks(orders);
//        price=29000000 _target=40 _ack=40 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=3 40@29000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=28000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=1 60@28000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=27000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=2 60@27000000 active=1 acked=1 cancelling=0 modifying=0}

        //Stealing this from the FD_CME_NewOrder test to get to the point where we have one order blocking, except the target is going to be 5 and not 1
        //so we can test same order blocking multiple times
        hf::price_t bidPrice = ToRhonePrice(.29);
        hf::qty_t quoteQty = 40;
        stacker->quote<BID>(bidPrice, quoteQty+5, goThroughAllLevels);
        assertLevelState(levels, ToRhonePrice(.29), quoteQty+5, 40, 5, 0, 0, 0);
        stacker->quote<BID>(bidPrice, quoteQty-5, goThroughAllLevels);
        assertLevelState(levels, ToRhonePrice(.29), quoteQty-5, 40, -5, 0, 0, 0);
        ASSERT_EQ(findLevel(levels, ToRhonePrice(.29))->_orders.size(), 2u);
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        stacker->quote<BID>(bidPrice, 4, goThroughAllLevels);
        assertLevelState(levels, ToRhonePrice(.29), 4, 40, -35, 0, 0, 0);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        stacker->quote<BID>(30000000, 40, false);
        assertLevelState(levels, ToRhonePrice(.30), 40, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, -35, 0, 0, 0, {3,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0, {2});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Since there is no order @ .30, quoting at 39@.30 should only change _target and nothing else
        stacker->quote<BID>(30000000, 39, false);
        assertLevelState(levels, ToRhonePrice(.30), 39, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, -35, 0, 0, 0, {3,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60,60, 0, 0, 0, 0, {2});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, FD_CME_NewOrder_MultipleBlockedOrdersAtDifferentLevels) {
        //Boilerplate
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        OrderManager<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        performKnownGoodModifyAcks(orders);
//        price=29000000 _target=40 _ack=40 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=3 40@29000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=28000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=1 60@28000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=27000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=2 60@27000000 active=1 acked=1 cancelling=0 modifying=0}

        //Increase Stack size to generate new orders, then decrease to below original quantity forcing it to modify down . . . if it could but it can't.
        stacker->updateQuoteSize(100, true);
        stacker->updateQuoteSize(20, true);
        assertLevelState(levels, ToRhonePrice(.29), 40, 40, 0, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.28), 20, 60, -20, 0, 0, 0, {1, 4});
        assertLevelState(levels, ToRhonePrice(.27), 20,60, -20, 0, 0, 0, {2, 5});
        ASSERT_EQ(2, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Increase the quote size, nothing should happen
        stacker->quote<BID>(29000000, 45, false);
        assertLevelState(levels, ToRhonePrice(.29), 45, 40, 0, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.28), 20, 60, -20, 0, 0, 0, {1, 4});
        assertLevelState(levels, ToRhonePrice(.27), 20,60, -20, 0, 0, 0, {2, 5});
        ASSERT_EQ(2, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Decrease the quote size, this should work
        stacker->quote<BID>(29000000, 10, false);
        assertLevelState(levels, ToRhonePrice(.29), 10, 40, -30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.28), 20, 60, -20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.27), 20,60, -20, 0, 0, 0, {2,5});
        ASSERT_EQ(2, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Now we're going to move the stack, and nothing should happen on the market.  Just _target at .30 should change and _target at .29 should change
        stacker->quote<BID>(30000000, 5, false);
        assertLevelState(levels, ToRhonePrice(.30), 5, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 20, 40, -30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.28), 20, 60, -20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.27), 20,60, -20, 0, 0, 0, {2,5});
        ASSERT_EQ(2, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //We have two orders to ack.  40@.28 (oid=4) and 40@0.27 (oid=5).  After we ack the first one it should be cancelled.  After we ack the second one
        //we should expect the new orders to go out
        onOrderAccepted(stacker, 40, *getOrder(stacker, 4));
        assertLevelState(levels, ToRhonePrice(.30), 5, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 20, 40, -30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.28), 20, 100, -80, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.27), 20,60, -20, 0, 0, 0, {2,5});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        ASSERT_TRUE(getOrder(stacker, 1)->is_cancelling);

        onOrderAccepted(stacker, 40, *getOrder(stacker, 5));
        assertLevelState(levels, ToRhonePrice(.30), 5, 0, 5, 0, 0, 0, {6});
        assertLevelState(levels, ToRhonePrice(.29), 20, 40, -20, 0, 0, 0, {3,7});
        assertLevelState(levels, ToRhonePrice(.28), 20, 100, -80, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.27), 20,100, -80, 0, 0, 0, {2,5});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        ASSERT_TRUE(getOrder(stacker, 2)->is_cancelling);

        //std::cout << to_string(levels) << std::endl;
    }

    //We're going to test the "LevelStealer==null" case here, i.e. all the logic is triggering at the same level.  This wants to modify down
    //at the end, i.e. nothing is really encouring the stacker to send a new.  This is more a test of core stacker logic than the prevention code.
    TEST_F(StackerFixture, FD_CME_NewOrder_SameLevelBlocked_ButOnlyWantsToModifyDown) {

        //Boilerplate
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        OrderManager<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        performKnownGoodModifyAcks(orders);
//        price=29000000 _target=40 _ack=40 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=3 40@29000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=28000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=1 60@28000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=27000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=2 60@27000000 active=1 acked=1 cancelling=0 modifying=0}

        //The plan is target qty to 50, 60, 70.  Then we will walk it back to 5 which should make 3 orders blocking, *HOWEVER* only one will be marked as blocking
        //(intentionally).  After every ack we'd expect the order to be cancelled, but another order to be made blocking.  Then after the final ack we get a
        //final cancel and a new order.
        stacker->quote<BID>(29000000, 50, false);
        stacker->quote<BID>(29000000, 60, false);
        stacker->quote<BID>(29000000, 70, false);
        stacker->quote<BID>(29000000, 5, false);

        assertLevelState(levels, ToRhonePrice(.29), 5, 40, -10, 0, 0, 0, {3,4,5,6});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        //Level 0.29 is a mess, everything is cancelling or waiting for an ack
//        {oid=4 10@29000000 active=1 acked=0 cancelling=0 modifying=0}
//        {oid=5 10@29000000 active=1 acked=0 cancelling=0 modifying=0}
//        {oid=6 10@29000000 active=1 acked=0 cancelling=0 modifying=0}
        ASSERT_TRUE(getOrder(stacker, 3)->is_cancelling);
        ASSERT_FALSE(getOrder(stacker, 4)->is_acked);
        ASSERT_FALSE(getOrder(stacker, 5)->is_acked);
        ASSERT_FALSE(getOrder(stacker, 6)->is_acked);
        //And only 1 order is marked as blocking even though three technically are
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Ack the first one
        onOrderAccepted(stacker, 10, *getOrder(stacker, 4));
        assertLevelState(levels, ToRhonePrice(.29), 5, 50, -30, 0, 0, 0, {3,4,5,6});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        ASSERT_TRUE(getOrder(stacker, 4)->is_cancelling);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Ack the second one
        onOrderAccepted(stacker, 10, *getOrder(stacker, 5));
        assertLevelState(levels, ToRhonePrice(.29), 5, 60, -50, 0, 0, 0, {3,4,5,6});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        ASSERT_TRUE(getOrder(stacker, 5)->is_cancelling);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Ack the third one, causing a new
        onOrderAccepted(stacker, 10, *getOrder(stacker, 6));
        assertLevelState(levels, ToRhonePrice(.29), 5, 70, -65, 0, 0, 0, {3,4,5,6});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        ASSERT_TRUE(getOrder(stacker, 6)->is_modifying);
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }


    //We're going to test the "LevelStealer==null" case here, i.e. all the logic is triggering at the same level.  This wants to *send a new*
    //at the end, which we should be preventing until the third order is acked.
    TEST_F(StackerFixture, FD_CME_NewOrder_SameLevelBlocked_WantsToSendNew) {

        //Boilerplate
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        OrderManager<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        performKnownGoodModifyAcks(orders);
//        price=29000000 _target=40 _ack=40 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=3 40@29000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=28000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=1 60@28000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=27000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=2 60@27000000 active=1 acked=1 cancelling=0 modifying=0}

        //The plan is target qty to 50, 60, 70.  Then we will walk it back to 5 which should make 3 orders blocking, *HOWEVER* only one will be marked as blocking
        //(intentionally).  After every ack we'd expect the order to be cancelled, but another order to be made blocking.  Then after the final ack we get a
        //final cancel and a new order.
        stacker->quote<BID>(29000000, 50, false);
        stacker->quote<BID>(29000000, 60, false);
        stacker->quote<BID>(29000000, 70, false);
        stacker->quote<BID>(29000000, 5, false);
        stacker->quote<BID>(29000000, 100, false); //100 > 70 means the stacker really wants to send a new here, but we're stopping it

        assertLevelState(levels, ToRhonePrice(.29), 100, 40, -10, 0, 0, 0, {3,4,5,6});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        //Level 0.29 is a mess, everything is cancelling or waiting for an ack
//        {oid=4 10@29000000 active=1 acked=0 cancelling=0 modifying=0}
//        {oid=5 10@29000000 active=1 acked=0 cancelling=0 modifying=0}
//        {oid=6 10@29000000 active=1 acked=0 cancelling=0 modifying=0}
        ASSERT_TRUE(getOrder(stacker, 3)->is_cancelling);
        ASSERT_FALSE(getOrder(stacker, 4)->is_acked);
        ASSERT_FALSE(getOrder(stacker, 5)->is_acked);
        ASSERT_FALSE(getOrder(stacker, 6)->is_acked);
        //And only 1 order is marked as blocking even though three technically are
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Ack the first one
        onOrderAccepted(stacker, 10, *getOrder(stacker, 4));
        assertLevelState(levels, ToRhonePrice(.29), 100, 50, -20, 0, 0, 0, {3,4,5,6});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        ASSERT_FALSE(getOrder(stacker, 4)->is_cancelling);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Ack the second one
        onOrderAccepted(stacker, 10, *getOrder(stacker, 5));
        assertLevelState(levels, ToRhonePrice(.29), 100, 60, -30, 0, 0, 0, {3,4,5,6});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        ASSERT_FALSE(getOrder(stacker, 5)->is_cancelling);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Ack the third one, causing a new
        onOrderAccepted(stacker, 10, *getOrder(stacker, 6));
        assertLevelState(levels, ToRhonePrice(.29), 100, 70, 30, 0, 0, 0, {3,4,5,6,7});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        //Note now that nothing is blocking us, we're fine being in a condition where we can get overfilled -- we haven't seen the cancel ack for oid=3 yet
        ASSERT_EQ(100, getOrder(stacker, 4)->qty + getOrder(stacker, 5)->qty + getOrder(stacker, 6)->qty + getOrder(stacker, 7)->qty);
        ASSERT_TRUE(getOrder(stacker, 3)->qty > 0);
        ASSERT_TRUE(getOrder(stacker, 3)->is_cancelling);
        //This is just asserting nothing is blocking us.
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    //Verify that a New Order reject sets the state correctly
    TEST_F(StackerFixture, FD_CME_NewOrderRejected) {

        //Very simple test to generate a new order reject, and make sure that we do not do anything (like requote) when we clear the flag
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        OrderManager<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        performKnownGoodModifyAcks(orders);
//        price=29000000 _target=40 _ack=40 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=3 40@29000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=28000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=1 60@28000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=27000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=2 60@27000000 active=1 acked=1 cancelling=0 modifying=0}
        stacker->quote<BID>(29000000, 50, false);
        stacker->quote<BID>(29000000, 5, false);
        assertLevelState(levels, ToRhonePrice(.29), 5, 40, -30, 0, 0, 0, {3,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        ASSERT_TRUE(getOrder(stacker, 3)->is_cancelling);
        ASSERT_FALSE(getOrder(stacker, 4)->is_acked);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));


        onOrderRejected(stacker, *getOrder(stacker, 4), 12345);
        assertLevelState(levels, ToRhonePrice(.29), 5, 40, -40, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {2});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    //Verify that if any order in the stealer cannot be cancelled, we will prevent sending a new order
    TEST_F(StackerFixture, FD_CME_NEW_ORDER_BLOCKED_DUE_TO_INFLIGHT_ORDER_IN_STEALER) {
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        OrderManager<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        performKnownGoodModifyAcks(orders);
//        price=29000000 _target=40 _ack=40 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=3 40@29000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=28000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=1 60@28000000 active=1 acked=1 cancelling=0 modifying=0}
//        price=27000000 _target=60 _ack=60 _flight=0 _traded=0 _flightout=0 _po=0 oc=1 orders:{oid=2 60@27000000 active=1 acked=1 cancelling=0 modifying=0}

        //Move the entire stack.  The data struture starts at .31, we just need room to do what we want to do
        stacker->quote<BID>(25000000, 50, false);
        assertLevelState(levels, ToRhonePrice(.29), 0, 40, -40, 0, 40, 0, {});
        assertLevelState(levels, ToRhonePrice(.28), 0, 60, -60, 0, 60, 0, {});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        assertLevelState(levels, ToRhonePrice(.26), 0, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.25), 50, 0, 50, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.24), 60, 0, 60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.23), 60, 0, 60, 0, 0, 0, {1});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        onOrderReplaced(stacker, 50, *getOrder(stacker, 3));
        onOrderReplaced(stacker, 60, *getOrder(stacker, 2));
        onOrderReplaced(stacker, 60, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.25), 50, 50, 0, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.24), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.23), 60, 60, 0, 0, 0, 0, {1});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Increase quote size to force new order @ .25
        stacker->quote<BID>(25000000, 55, false);
        assertLevelState(levels, ToRhonePrice(.25), 55, 50, 5, 0, 0, 0, {3,4});
        assertLevelState(levels, ToRhonePrice(.24), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.23), 60, 60, 0, 0, 0, 0, {1});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Move the entire stack.  This should be blocked since we have the NO in flight
        stacker->quote<BID>(31000000, 55, false);

        std::cout << to_string(levels) << std::endl;

        assertLevelState(levels, ToRhonePrice(.31), 55, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.30), 60, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 60, 0, 0, 0, 0, 0, {});

        assertLevelState(levels, ToRhonePrice(.25), 0, 50, -45, 0, 0, 0, {3,4});
        assertLevelState(levels, ToRhonePrice(.24), 0, 60, -60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.23), 0, 60, -60, 0, 0, 0, {1});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //This should be the order blocking the news
        onOrderAccepted(stacker, 5, *getOrder(stacker, 4));

        assertLevelState(levels, ToRhonePrice(.31), 55, 0, 55, 0, 0, 0, {5});
        assertLevelState(levels, ToRhonePrice(.30), 60, 0, 60, 0, 0, 0, {6});
        assertLevelState(levels, ToRhonePrice(.29), 60, 0, 60, 0, 0, 0, {7});

        assertLevelState(levels, ToRhonePrice(.25), 0, 55, -55, 0, 0, 0, {3,4});
        assertLevelState(levels, ToRhonePrice(.24), 0, 60, -60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.23), 0, 60, -60, 0, 0, 0, {1});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, FD_ICE_NewOrderModifyOrderDoesNotTriggerLogic) {
        //Making sure a NO doesn't block like it does on CME
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);

        stacker->initialize(iceContract, BID, config_60_2_0, _ime2);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
        OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
        //This is almost the same as the CME, except notice the OIDs are different
        assertLevelState(levels, ToRhonePrice(.29), 40, 0, 40, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 0, 60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 0, 60, 0, 0, 0, {3});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //For ICE we're willing to send a modify in-flight with a new
        stacker->quote<BID>(ToRhonePrice(0.29), 39, false);
        assertLevelState(levels, ToRhonePrice(.29), 39, 0, 39, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 0, 60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 0, 60, 0, 0, 0, {3});
        ASSERT_TRUE(getOrder(stacker, 1)->is_modifying);
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //But a second one does not
        stacker->quote<BID>(ToRhonePrice(0.29), 38, false);
        assertLevelState(levels, ToRhonePrice(.29), 38, 0, 39, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 0, 60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 0, 60, 0, 0, 0, {3});
        ASSERT_TRUE(getOrder(stacker, 1)->is_modifying);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //NO Ack should *not* unblock
        onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.29), 38, 40, -1, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 0, 60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 0, 60, 0, 0, 0, {3});
        ASSERT_TRUE(getOrder(stacker, 1)->is_modifying);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //But after the modify ack, should send the modify out
        onOrderReplaced(stacker, 39, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.29), 38, 39, -1, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 0, 60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 0, 60, 0, 0, 0, {3});
        ASSERT_TRUE(getOrder(stacker, 1)->is_modifying);
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, FD_ICE_ModifyModifyTriggersLogic) {
        //Very similar to FD_ICE_NewOrderModifyOrderDoesNotTriggerLogic.  We're going to have
        //a modify in flight with a modify, except we're going to start from the resting level having been acked.
        //we're also going to move levels to make things a little more complex

        //Boilerplate
        //Making sure a NO doesn't block like it does on CME
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);

        stacker->initialize(iceContract, BID, config_60_2_0, _ime2);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
        OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
        onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 3));
        assertLevelState(levels, ToRhonePrice(.29), 40, 40, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        stacker->quote<BID>(ToRhonePrice(.31), 30, false);
        assertLevelState(levels, ToRhonePrice(.31), 30, 0, 0, 0, 0, 0, {});  //Note order was not sent out
        assertLevelState(levels, ToRhonePrice(.30), 60, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 0, 60, -60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        onOrderReplaced(stacker, 30, *getOrder(stacker, 3));

        assertLevelState(levels, ToRhonePrice(.31), 30, 0, 30, 0, 0, 0, {6});  //Note order was sent out
        assertLevelState(levels, ToRhonePrice(.30), 60, 30, 30, 0, 0, 0, {3,5});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 0, 60, -60, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 0, 0, 0, 0, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, FD_ICE_ModifyWithPartialFillDoesNotUnblock) {
        //Boilerplate
        //Making sure a NO doesn't block like it does on CME
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_2_0, _ime2);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
        OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
        onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 3));
        assertLevelState(levels, ToRhonePrice(.29), 40, 40, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        //End Boilerplate

        //We're going to partially fill.  Stacker shouldn't react
        onOrderExecuted(stacker, *getOrder(stacker, 3), ToRhonePrice(.27), 3, true);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 27, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -57, 3, 57, 0, {});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Unblock, Stacker should react
        onOrderReplaced(stacker, 30, *getOrder(stacker, 3));
        assertLevelState(levels, ToRhonePrice(.30), 29, 27, 2, 0, 0, 0, {3,5});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 3, 0, 3, 0, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    //A partial fill doesn't generate a reaction, but a full fill should
    TEST_F(StackerFixture, FD_ICE_ModifyWithFillAsResultOfDecrementUnblocks) {

        //Boilerplate
        //Making sure a NO doesn't block like it does on CME
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_2_0, _ime2);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
        OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
        onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 3));
        assertLevelState(levels, ToRhonePrice(.29), 40, 40, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        //End Boilerplate

        //We're going to fully fill.  Stacker *should* react, but shouldn't requote since _target hasn't changed
        auto& o3 = *getOrder(stacker, 3);
        onOrderExecuted(stacker, o3, ToRhonePrice(.27), 60, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, 0, 60, 0, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Send the same quote back in, should have a normal stack i.e. new order at .30
        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 29, 0, 0, 0, {5});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, 0, 60, 0, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Everything should look sane after the MR.  This is literally a copy/paste of the asserts above.  Note ICE doesn't send a OC.
        onOrderReplaceRejected(stacker, o3, static_cast<int>(ice_rejects::UNKNOWN_ORDER));
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 29, 0, 0, 0, {5});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, 0, 60, 0, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, FD_ICE_Modify_Reject_Unblocks) {
        //To the above test generated a modify reject due to a fill in flight.  We're going to straight up generate a modify reject due to
        //something like SMP, i.e. not expected in normal operation but can happen.

        //Boilerplate
        //Making sure a NO doesn't block like it does on CME
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_2_0, _ime2);
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
        stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
        OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
        onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
        onOrderAccepted(stacker, 60, *getOrder(stacker, 3));
        assertLevelState(levels, ToRhonePrice(.29), 40, 40, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 30, 0, 0, 0, {3});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        //End Boilerplate

        //We're going to reject the modify on oid={3}.  Not we don't requote in response
        auto& oid3 = *getOrder(stacker, 3);
        onOrderReplaceRejected(stacker, oid3, 12345);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.29), 60, 40, 20, 0, 0, 0, {1,4});
        assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
        assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 0, 0, {3});
        ASSERT_TRUE((!oid3.is_modifying) && (oid3.is_cancelling));
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }

    //Fixes a bug where a CO that blocked a modify down appeared to never unblock.  Specifically:
    //NO1 -> OA, (qty up) => NO2, (quote down) => CO[order2], (quote down) => MO[order1], (quote down) => blocked, never unblocked
    TEST_F(StackerFixture, FD_ICE_Cancel_Unblocks_Correctly_BUGFIX) {
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_0_0, _ime2);
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;

        //Ack the NO
        onOrderAccepted(stacker, 30, *getOrder(stacker, 1));
        //Quote bigger
        stacker->quote<BID>(ToRhonePrice(.30), 32, false);
        //Then smaller, causing a cancel
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        ASSERT_TRUE(getOrder(stacker, 2)->is_cancelling);
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        //Then smaller, causing a modify
        stacker->quote<BID>(ToRhonePrice(.30), 28, false);
        ASSERT_TRUE(getOrder(stacker, 1)->is_modifying);
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        //Then smaller, which will block
        stacker->quote<BID>(ToRhonePrice(.30), 26, false);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Ack, then Cancel Ack
        onOrderAccepted(stacker, 2, *getOrder(stacker, 2));
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));
        onOrderCanceled(stacker, *getOrder(stacker, 2));
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Then modify ack
        onOrderReplaced(stacker, 28, *getOrder(stacker, 1));
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //We should have modified to 26
        assertLevelState(levels, ToRhonePrice(.30), 26, 28, -2, 0, 0, 0, {1});

        //Ack it, then requote to 30.  Should see a new order go out.
        onOrderReplaced(stacker, 26, *getOrder(stacker, 1));
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);

        assertLevelState(levels, ToRhonePrice(.30), 30, 26, 4, 0, 0, 0, {1,3});

        //std::cout << to_string(levels) << std::endl;
    }

    //Fixes a bug where a OC because of a modify down didn't unblock
    TEST_F(StackerFixture, FD_ICE_CANCEL_DUE_TO_MODIFY_DOWN_UNBLOCKS) {
        OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::ICE);
        stacker->initialize(iceContract, BID, config_60_0_0, _ime2);
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;

        //Ack the NO
        onOrderAccepted(stacker, 30, *getOrder(stacker, 1));
        //Quote Smaller
        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        //Then smaller, blocking
        stacker->quote<BID>(ToRhonePrice(.30), 28, false);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Fill a quantity that will result in a cancel
        onOrderExecuted(stacker, *getOrder(stacker, 1), ToRhonePrice(.30), 29, true);
        ASSERT_EQ(1, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //Unsolicited cancel due to modify down
        onOrderCanceled(stacker, *getOrder(stacker, 1));

        //Verify no longer blocking
        ASSERT_EQ(0, getAndAssertNumOrdersBlockingDecrement(stacker, levels));

        //std::cout << to_string(levels) << std::endl;
    }
}