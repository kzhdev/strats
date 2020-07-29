//
// Created by cmilutinovic on 9/2/20.
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

    //On Eurex a cancel can be in flight with a new order
    TEST_F(StackerFixture, Eurex_New_Cancel_In_flight) {

        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;

        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});

        stacker->quote<BID>(ToRhonePrice(.30), 0, false);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {1});
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_cancelling);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_active);

        //If we quote, we should send a new order since order#1 is dead
        stacker->quote<BID>(ToRhonePrice(.30), 1, false);
        assertLevelState(levels, ToRhonePrice(.30), 1, 0, 1, 0, 0, 0, {1, 2});

        onOrderAccepted(stacker, 30, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.30), 1, 30, -29, 0, 0, 0, {1, 2});
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_cancelling);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_active);

        //We want it to send another new order and not try to modify the first order
        stacker->quote<BID>(ToRhonePrice(.30), 2, false);
        assertLevelState(levels, ToRhonePrice(.30), 2, 30, -28, 0, 0, 0, {1, 2, 3});

        //State should just be the two new orders at this point
        onOrderCanceled(stacker, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.30), 2, 0, 2, 0, 0, 0, {2, 3});

        //std::cout << to_string(levels) << std::endl;
    }

    //Make sure that we can send a modify in flight with a new order
    TEST_F(StackerFixture, Eurex_New_Modify_In_flight) {

        //Boilerplate
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;

        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});

        //Begin test
        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 29, 0, 0, 0, {1});

        //Ack the NO
        onOrderAccepted(stacker, 30, *getOrder(stacker, 1));
        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        assertLevelState(levels, ToRhonePrice(.30), 29, 30, -1, 0, 0, 0, {1});

        //Ack the Modify
        onOrderReplaced(stacker, 29, *getOrder(stacker, 1));
        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        ASSERT_FALSE(getActionFlags(stacker, 1)->is_modifying);
        assertLevelState(levels, ToRhonePrice(.30), 29, 29, 0, 0, 0, 0, {1});

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, Eurex_New_Modify_Cancel_In_flight) {
        //Boilerplate
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;

        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});

        //Begin test
        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 29, 0, 0, 0, {1});

        stacker->quote<BID>(ToRhonePrice(.30), 0, false);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_cancelling);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {1});

        //Make sure a quote sends a new order
        stacker->quote<BID>(ToRhonePrice(.30), 1, true);
        assertLevelState(levels, ToRhonePrice(.30), 1, 0, 1, 0, 0, 0, {1, 2});

        //Ack the order
        onOrderAccepted(stacker, 30, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.30), 1, 30, -29, 0, 0, 0, {1, 2});
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_cancelling);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_active);

        //Ack the modify
        onOrderReplaced(stacker, 29, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.30), 1, 29, -28, 0, 0, 0, {1, 2});
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_cancelling);
        ASSERT_FALSE(getActionFlags(stacker, 1)->is_modifying);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_active);

        //Ack the cancel
        onOrderCanceled(stacker, *getOrder(stacker, 1));
        assertLevelState(levels, ToRhonePrice(.30), 1, 0, 1, 0, 0, 0, {2});

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, Eurex_New_Modify_Diff_Level_In_flight) {
        //Boilerplate
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;

        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});

        //Send new and modify
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        stacker->quote<BID>(ToRhonePrice(.31), 5, false);
        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 30, 0, {});
        assertLevelState(levels, ToRhonePrice(.31), 5, 0, 5, 0, 0, 0, {1});

        //Ack the NO
        onOrderAccepted(stacker, 30, *getOrder(stacker, 1));
        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        assertLevelState(levels, ToRhonePrice(.30), 0, 30, -30, 0, 30, 0, {});
        assertLevelState(levels, ToRhonePrice(.31), 5, 0, 5, 0, 0, 0, {1});

        //Ack the MO
        onOrderReplaced(stacker, 5, *getOrder(stacker, 1));
        ASSERT_TRUE(stacker->canModifyOrCancelOrder(*getOrder(stacker, 1)));
        ASSERT_FALSE(getActionFlags(stacker, 1)->is_modifying);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.31), 5, 5, 0, 0, 0, 0, {1});

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, Eurex_New_Modify_With_Reject_Same_Level) {
        //Boilerplate
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        //Test
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});
        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 29, 0, 0, 0, {1});

        onOrderRejected(stacker, *getOrder(stacker, 1), 99);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 0, 0, 0, 0, {});

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 0, 0, 0, 0, {});

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, Eurex_New_Modify_Cancel_With_Reject_Same_Level) {
        //This is identical to Eurex_New_Modify_With_Reject_Same_Level except we will add a cancel in-flight with the modify in-flight
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});
        stacker->quote<BID>(ToRhonePrice(.30), 29, false);
        assertLevelState(levels, ToRhonePrice(.30), 29, 0, 29, 0, 0, 0, {1});
        //New Action Here!
        stacker->quote<BID>(ToRhonePrice(.30), 0, false);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {1});
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_active);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_modifying);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_cancelling);

        auto isUnchanged = [this, &levels]() -> bool {
            assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {}); //can throw
            return true;
        };

        //No message should have any effect on the level state
        onOrderRejected(stacker, *getOrder(stacker, 1), 99);
        ASSERT_TRUE(isUnchanged());
        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        ASSERT_TRUE(isUnchanged());
        onOrderCancelRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        ASSERT_TRUE(isUnchanged());

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, Eurex_New_Cancel_With_Reject) {
        //Boilerplate
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});
        stacker->quote<BID>(ToRhonePrice(.30), 0, false);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {1});

        ASSERT_TRUE(getActionFlags(stacker, 1)->is_active);
        ASSERT_FALSE(getActionFlags(stacker, 1)->is_modifying);
        ASSERT_TRUE(getActionFlags(stacker, 1)->is_cancelling);

        //Level state the same for both tests below.
        onOrderRejected(stacker, *getOrder(stacker, 1), 99);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});
        onOrderCancelRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});
    }

    TEST_F(StackerFixture, Eurex_New_Modify_With_Reject_Diff_Level) {
        //Boilerplate
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        //Test
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});
        stacker->quote<BID>(ToRhonePrice(.31), 28, false);

        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 30, 0, {});
        assertLevelState(levels, ToRhonePrice(.31), 28, 0, 28, 0, 0, 0, {1});

        onOrderRejected(stacker, *getOrder(stacker, 1), 99);
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.31), 28, 0, 0, 0, 0, 0, {});

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        //Nothing should have changed
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});
        assertLevelState(levels, ToRhonePrice(.31), 28, 0, 0, 0, 0, 0, {});

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, Eurex_New_Modify_With_Reject_Diff_Level_Complex_Path) {
        //We're going to have many modifes in flight except just one, from 30->31->30->32
        //Boilerplate
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        //Test
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});

        stacker->quote<BID>(ToRhonePrice(.31), 28, false);
        assertLevelState(levels, ToRhonePrice(.31), 28, 0, 28, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 30, 0, {});

        stacker->quote<BID>(ToRhonePrice(.30), 28, false);
        assertLevelState(levels, ToRhonePrice(.30), 28, 0, 28, 0, 30, 0, {1});

        stacker->quote<BID>(ToRhonePrice(.32), 28, false);
        assertLevelState(levels, ToRhonePrice(.32), 28, 0, 28, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.31), 0, 0, 0, 0, 28, 0, {});
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 58, 0, {});

        //Send the no Reject and Modify Rejects
        onOrderRejected(stacker, *getOrder(stacker, 1), 99);
        auto assertSteadyState = [this, &levels]() -> bool {
            //Can throw
            assertLevelState(levels, ToRhonePrice(.32), 28, 0, 0, 0, 0, 0, {});
            assertLevelState(levels, ToRhonePrice(.31), 0, 0, 0, 0, 0, 0, {});
            assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});
            return true;
        };
        assertSteadyState();

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertSteadyState();

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertSteadyState();

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertSteadyState();

        //std::cout << to_string(levels) << std::endl;
    }

    TEST_F(StackerFixture, Eurex_New_Modify_Cancel_With_Reject_Diff_Level_Complex_Path) {
        //This is almost a copy & paste of the previous test. We're going to have many modifies in flight from 30->31->30->32
        //just like the Eurex_New_Modify_With_Reject_Diff_Level_Complex_Path test.  Except we're going to put a cancel at the end of the chain

        //Boilerplate, cop and pasted from the above test except . . .
        OrderManager<TestIME_2> *stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::EUREX);
        stacker->initialize(eurexContract, BID, config_60_0_0, _ime2, true);
        stacker->updateNumberOfLevels<BID>(0, true);
        stacker->updateMaxSlackSize<BID>(0);

        //Test
        stacker->quote<BID>(ToRhonePrice(.30), 30, false);
        OrderManager<TestIME_2>::Level **levels = &stacker->_topLevel;
        assertLevelState(levels, ToRhonePrice(.30), 30, 0, 30, 0, 0, 0, {1});

        stacker->quote<BID>(ToRhonePrice(.31), 28, false);
        assertLevelState(levels, ToRhonePrice(.31), 28, 0, 28, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 30, 0, {});

        stacker->quote<BID>(ToRhonePrice(.30), 28, false);
        assertLevelState(levels, ToRhonePrice(.30), 28, 0, 28, 0, 30, 0, {1});

        stacker->quote<BID>(ToRhonePrice(.32), 28, false);
        assertLevelState(levels, ToRhonePrice(.32), 28, 0, 28, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.31), 0, 0, 0, 0, 28, 0, {});
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 58, 0, {});

        /// . . . we're going to stick a cancel at the end of the chain
        stacker->quote<BID>(ToRhonePrice(.32), 0, false);
        assertLevelState(levels, ToRhonePrice(.32), 0, 0, 0, 0, 0, 0, {1});
        assertLevelState(levels, ToRhonePrice(.31), 0, 0, 0, 0, 28, 0, {});
        assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 58, 0, {});
        std::cout << to_string(levels) << std::endl;

        //Send the no Reject and Modify Rejects
        onOrderRejected(stacker, *getOrder(stacker, 1), 99);
        auto assertSteadyState = [this, &levels]() -> bool {
            //Can throw
            assertLevelState(levels, ToRhonePrice(.32), 0, 0, 0, 0, 0, 0, {});
            assertLevelState(levels, ToRhonePrice(.31), 0, 0, 0, 0, 0, 0, {});
            assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});
            return true;
        };
        assertSteadyState();

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertSteadyState();

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertSteadyState();

        onOrderReplaceRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);
        assertSteadyState();

        onOrderCancelRejected(stacker, *getOrder(stacker, 1), EUREX_ORDER_NOT_FOUND);

        //std::cout << to_string(levels) << std::endl;
    }
}