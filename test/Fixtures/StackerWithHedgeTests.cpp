//
// Created by kzhao on 01/08/20.
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

TEST_F(StackerFixture, Hedge_No_Stack_test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 1));
    OrderManager<TestIME_2>::Level** hedge = &stacker->_hedge;
    assertLevelState(hedge, ToRhonePrice(.29), 10, 10, 0, 0, 0, 0, {1});

    ASSERT_TRUE(stacker->_bottomLevel == nullptr);

    OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
    assertLevelState(levels, ToRhonePrice(.29), 10, 10, 0, 0, 0, 0, {1});
    assertLevelState(levels, ToRhonePrice(.28), 0, 0, 0, 0, 0, 0, {});
    assertLevelState(levels, ToRhonePrice(.27), 0, 0, 0, 0, 0, 0, {});
}

TEST_F(StackerFixture, Hedge_Stack_Target_Test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 1));

    // Add stack
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));
    stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    onOrderAccepted(stacker, 40, *getOrder(stacker, 2));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 3));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 4));

    ASSERT_TRUE(stacker->_bottomLevel != nullptr);
    ASSERT_TRUE(stacker->_bottomLevel->_price == ToRhonePrice(0.27));

    OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
    assertLevelState(levels, ToRhonePrice(.29), 50, 50, 0, 0, 0, 0, {1, 2});
    assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {3});
    assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {4});
}

TEST_F(StackerFixture, Stack_Hedge_Target_Test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 3));

    OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
    assertLevelState(levels, ToRhonePrice(.29), 40, 40, 0, 0, 0, 0, {1});
    assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
    assertLevelState(levels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

    ASSERT_TRUE(stacker->_hedge == nullptr);

    //Adding a BID hedge
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 4));
    assertLevelState(levels, ToRhonePrice(.29), 50, 50, 0, 0, 0, 0, {1, 4});
}

TEST_F(StackerFixture, Move_Stack_To_Worse_Price_Test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 3));

    //Adding a BID hedge
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 4));

    // Move Stack Quote to worse price
    stacker->quote<BID>(ToRhonePrice(0.27), 40, false);

    ASSERT_TRUE(stacker->_hedge->_price == ToRhonePrice(0.29));

    OrderManager<TestIME_2>::Level** levels = &stacker->_hedge;
    assertLevelState(levels, ToRhonePrice(.29), 10, 50, -40, 0, 10, 0, {1});
    assertLevelState(levels, ToRhonePrice(.28), 0, 60, -60, 0, 60, 0, {});

    levels = &stacker->_topLevel;
    assertLevelState(levels, ToRhonePrice(.27), 40, 60, -20, 0, 0, 0, {3});
    assertLevelState(levels, ToRhonePrice(.26), 60, 0, 60, 0, 0, 0, {2});
    assertLevelState(levels, ToRhonePrice(.25), 60, 0, 60, 0, 0, 0, {4});

    onOrderReplaced(stacker, 40, *getOrder(stacker, 3));
    assertLevelState(levels, ToRhonePrice(.27), 40, 40, 0, 0, 0, 0, {3});

    onOrderReplaced(stacker, 60, *getOrder(stacker, 4));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 10, 40, -30, 0, 0, 0, {1});
    assertLevelState(levels, ToRhonePrice(.25), 60, 60, 0, 0, 0, 0, {4});

    onOrderReplaced(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 10, 10, 0, 0, 0, 0, {1});

    onOrderReplaced(stacker, 60, *getOrder(stacker, 2));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.28), 0, 0, 0, 0, 0, 0, {});
    assertLevelState(levels, ToRhonePrice(.26), 60, 60, 0, 0, 0, 0, {2});
}

TEST_F(StackerFixture, Move_Stack_To_Better_Price_Test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 3));

    //Adding a BID hedge
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 4));

    // Move Stack Quote to worse price
    stacker->quote<BID>(ToRhonePrice(0.30), 40, false);

    ASSERT_TRUE(stacker->_hedge->_price == ToRhonePrice(0.29));

    OrderManager<TestIME_2>::Level** levels = &stacker->_topLevel;
    assertLevelState(levels, ToRhonePrice(0.3), 40, 0, 40, 0, 0, 0, {3});
    assertLevelState(levels, ToRhonePrice(.27), 0, 60, -60, 0, 60, 0, {});
    assertLevelState(levels, ToRhonePrice(.29), 70, 50, 20, 0, 0, 0, {1, 4, 5});
    assertLevelState(levels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});

    onOrderReplaced(stacker, 40, *getOrder(stacker, 3));
    assertLevelState(levels, ToRhonePrice(0.3), 40, 40, 0, 0, 0, 0, {3});
    assertLevelState(levels, ToRhonePrice(.27), 0, 0, 0, 0, 0, 0, {});

    onOrderAccepted(stacker, 20, *getOrder(stacker, 5));
    assertLevelState(levels, ToRhonePrice(.29), 70, 70, 0, 0, 0, 0, {1, 4, 5});

    stacker->quote<BID>(ToRhonePrice(0.34), 40, false);
    ASSERT_TRUE(stacker->_hedge->_price == ToRhonePrice(0.29));

    assertLevelState(levels, ToRhonePrice(.34), 40, 0, 40, 0, 0, 0, {3});
    assertLevelState(levels, ToRhonePrice(.33), 60, 0, 60, 0, 0, 0, {5});
    assertLevelState(levels, ToRhonePrice(.32), 60, 0, 60, 0, 0, 0, {4});
    assertLevelState(levels, ToRhonePrice(.31), 0, 0, 0, 0, 0, 0, {});
    assertLevelState(levels, ToRhonePrice(.30), 0, 40, -40, 0, 40, 0, {});
    assertLevelState(levels, ToRhonePrice(.29), 10, 70, -60, 0, 30, 0, {1});
    assertLevelState(levels, ToRhonePrice(.28), 0, 60, -60, 0, 0, 0, {2});

    onOrderReplaced(stacker, 40, *getOrder(stacker, 3));
    assertLevelState(levels, ToRhonePrice(.34), 40, 40, 0, 0, 0, 0, {3});
    assertLevelState(levels, ToRhonePrice(.30), 0, 0, 0, 0, 0, 0, {});

    onOrderReplaced(stacker, 60, *getOrder(stacker, 5));
    assertLevelState(levels, ToRhonePrice(.33), 60, 60, 0, 0, 0, 0, {5});
    assertLevelState(levels, ToRhonePrice(.29), 10, 50, -40, 0, 10, 0, {1});

    onOrderReplaced(stacker, 60, *getOrder(stacker, 4));
    assertLevelState(levels, ToRhonePrice(.32), 60, 60, 0, 0, 0, 0, {4});
    assertLevelState(levels, ToRhonePrice(.29), 10, 40, -30, 0, 0, 0, {1});

    onOrderReplaced(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(levels, ToRhonePrice(.29), 10, 10, 0, 0, 0, 0, {1});

    onOrderCanceled(stacker, *getOrder(stacker, 2));
    assertLevelState(levels, ToRhonePrice(.28), 0, 0, 0, 0, 0, 0, {});
}

TEST_F(StackerFixture, Move_Hedge_To_Better_Price_Test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 3));

    //Adding a BID hedge
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 4));

    // Move Stack Quote to worse price
    stacker->hedge<BID>(ToRhonePrice(0.31), 10);

    OrderManager<TestIME_2>::Level** levels = &stacker->_hedge;
    assertLevelState(levels, ToRhonePrice(.31), 10, 0, 10, 0, 0, 0, {4});
    assertLevelState(levels, ToRhonePrice(.29), 40, 50, -10, 0, 10, 0, {1});
}

TEST_F(StackerFixture, Move_Stack_To_Worse_Price_Move_Hedge_To_Better_Price_Test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 3));

    //Adding a BID hedge
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 4));

    // Move Stack Quote to worse price
    stacker->quote<BID>(ToRhonePrice(0.28), 40, false);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.28), 40, 60, -20, 0, 0, 0, {2});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 10, 50, -40, 0, 10, 0, {1});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.26), 60, 0, 60, 0, 0, 0, {4});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

    stacker->hedge<BID>(ToRhonePrice(0.31), 10);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 10, 0, 10, 0, 0, 0, {1});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 50, -50, 0, 20, 0, {});

    onOrderReplaced(stacker, 40, *getOrder(stacker, 2));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.28), 40, 40, 0, 0, 0, 0, {2});

    onOrderReplaced(stacker, 60, *getOrder(stacker, 4));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 40, -40, 0, 10, 0, {});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.26), 60, 60, 0, 0, 0, 0, {4});

    onOrderReplaced(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 10, -10, 0, 10, 0, {});

    onOrderReplaced(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 0, 0, 0, 0, {});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 10, 10, 0, 0, 0, 0, {1});
}

TEST_F(StackerFixture, ICE_Move_Stack_To_Worse_Price_Move_Hedge_To_Better_Price_Test) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    onOrderAccepted(stacker, 40, *getOrder(stacker, 1));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 2));
    onOrderAccepted(stacker, 60, *getOrder(stacker, 3));

    //Adding a BID hedge
    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    onOrderAccepted(stacker, 10, *getOrder(stacker, 4));

    // Move Stack Quote to worse price
    stacker->quote<BID>(ToRhonePrice(0.28), 40, false);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.28), 40, 60, -20, 0, 0, 0, {2});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 10, 50, -40, 0, 10, 0, {1});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.26), 60, 0, 60, 0, 0, 0, {4});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

    stacker->hedge<BID>(ToRhonePrice(0.31), 10);
    // order #1 modify in-flight. Can't steal. Place a new order
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 10, 0, 10, 0, 0, 0, {5});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 50, -40, 0, 10, 0, {1});

    onOrderReplaced(stacker, 40, *getOrder(stacker, 2));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.28), 40, 40, 0, 0, 0, 0, {2});

    onOrderReplaced(stacker, 60, *getOrder(stacker, 4));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 40, -30, 0, 0, 0, {1});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.26), 60, 60, 0, 0, 0, 0, {4});

    onOrderReplaced(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 10, -10, 0, 0, 0, {1});

    onOrderAccepted(stacker, 10, *getOrder(stacker, 5));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 10, 10, 0, 0, 0, 0, {5});

    onOrderCanceled(stacker, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 0, 0, 0, 0, {});
}

TEST_F(StackerFixture, CME_Change_Hedge_While_InFlight) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::CME);
    stacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 10, 0, 10, 0, 0, 0, {1});

    // Change hedge while in-flight
    stacker->hedge<BID>(ToRhonePrice(0.30), 10);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.30), 10, 0, 10, 0, 0, 0, {2});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 10, 0, 0, 0, {1});

    stacker->hedge<BID>(ToRhonePrice(0.31), 15);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 15, 0, 15, 0, 0, 0, {3});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.30), 0, 0, 10, 0, 0, 0, {2});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 10, 0, 0, 0, {1});

    onOrderAccepted(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 10, -10, 0, 0, 0, {1});

    onOrderAccepted(stacker, 10, *getOrder(stacker, 2));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.30), 0, 10, -10, 0, 0, 0, {2});

    om::order_t* oid3 = getOrder(stacker, 3);
    ASSERT_TRUE(oid3 != nullptr);
    onOrderAccepted(stacker, 15, *oid3);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 15, 15, 0, 0, 0, 0, {3});

    onOrderCanceled(stacker, *getOrder(stacker, 1));
    onOrderCanceled(stacker, *getOrder(stacker, 2));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 0, 0, 0, 0, {});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.3), 0, 0, 0, 0, 0, 0, {});
}

TEST_F(StackerFixture, ICE_Change_Hedge_While_InFlight) {
    //Boilerplate
    OrderManager<TestIME_2>* stacker = _ime2->getBidStacker();
    _ime2->setVenue(hf::VENUE::ICE);
    stacker->initialize(iceContract, BID, config_60_2_0, _ime2, true);
    initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

    stacker->hedge<BID>(ToRhonePrice(0.29), 10);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 10, 0, 10, 0, 0, 0, {1});

    // Change hedge while in-flight
    stacker->hedge<BID>(ToRhonePrice(0.30), 10);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.30), 10, 0, 10, 0, 0, 0, {1});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 0, 0, 10, 0, {});

    stacker->hedge<BID>(ToRhonePrice(0.31), 15);
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 15, 0, 15, 0, 0, 0, {2});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.30), 0, 0, 10, 0, 0, 0, {1});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 0, 0, 10, 0, {});

    onOrderAccepted(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 10, -10, 0, 10, 0, {});

    onOrderReplaced(stacker, 10, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.29), 0, 0, 0, 0, 0, 0, {});
    assertLevelState(&stacker->_hedge, ToRhonePrice(.30), 0, 10, -10, 0, 0, 0, {1});


    onOrderAccepted(stacker, 15, *getOrder(stacker, 2));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.31), 15, 15, 0, 0, 0, 0, {2});

    onOrderCanceled(stacker, *getOrder(stacker, 1));
    assertLevelState(&stacker->_hedge, ToRhonePrice(.3), 0, 0, 0, 0, 0, 0, {});
}

TEST_F(StackerFixture, Hedge_On_Other_Side_Moved_Back) {
    _ime2->setVenue(hf::VENUE::CME);
    OrderManager<TestIME_2>* bidStacker = _ime2->getBidStacker();
    bidStacker->initialize(cmeContract, BID, config_60_2_0, _ime2, true);

    OrderManager<TestIME_2>* askStacker = _ime2->getAskStacker();
    askStacker->initialize(cmeContract, ASK, config_60_2_0, _ime2, true);

    // Setup stack on both side
    bidStacker->quote<BID>(ToRhonePrice(0.29), 40, false);
    askStacker->quote<ASK>(ToRhonePrice(0.3), 40, false);

    onOrderAccepted(bidStacker, 40, *getOrder(bidStacker, 1));
    onOrderAccepted(bidStacker, 60, *getOrder(bidStacker, 2));
    onOrderAccepted(bidStacker, 60, *getOrder(bidStacker, 3));

    onOrderAccepted(askStacker, 40, *getOrder(askStacker, 4));
    onOrderAccepted(askStacker, 60, *getOrder(askStacker, 5));
    onOrderAccepted(askStacker, 60, *getOrder(askStacker, 6));

    auto topBid = bidStacker->_topLevel;
    OrderManager<TestIME_2>::Level** bidLevels = &topBid;
    assertLevelState(bidLevels, ToRhonePrice(.29), 40, 40, 0, 0, 0, 0, {1});
    assertLevelState(bidLevels, ToRhonePrice(.28), 60, 60, 0, 0, 0, 0, {2});
    assertLevelState(bidLevels, ToRhonePrice(.27), 60, 60, 0, 0, 0, 0, {3});

    OrderManager<TestIME_2>::Level** askLevels = &askStacker->_topLevel;
    assertLevelState(askLevels, ToRhonePrice(.3), 40, 40, 0, 0, 0, 0, {4});
    assertLevelState(askLevels, ToRhonePrice(.31), 60, 60, 0, 0, 0, 0, {5});
    assertLevelState(askLevels, ToRhonePrice(.32), 60, 60, 0, 0, 0, 0, {6});

    // Move bid side stack back
    bidStacker->quote<BID>(ToRhonePrice(0.27), 40, false);
    assertLevelState(bidLevels, ToRhonePrice(.27), 40, 60, -20, 0, 0, 0, {3});
    assertLevelState(bidLevels, ToRhonePrice(.25), 60, 0, 60, 0, 0, 0, {1});
    assertLevelState(bidLevels, ToRhonePrice(.29), 0, 40, -40, 0, 40, 0, {});
    assertLevelState(bidLevels, ToRhonePrice(.26), 60, 0, 60, 0, 0, 0, {2});
    assertLevelState(bidLevels, ToRhonePrice(.28), 0, 60, -60, 0, 60, 0, {});

    // Partially Fill bid side top level to make it more fun
    onOrderExecuted(bidStacker, *getOrder(bidStacker, 1), ToRhonePrice(0.29), 6, true);
    assertLevelState(bidLevels, ToRhonePrice(.29), 0, 40, -34, 6, 34, 0, {});

    // Add hedge for the fill
    askStacker->hedge<ASK>(ToRhonePrice(.28), 6);
    // Bid side order move is in-flight, hedge order should be placed
    ASSERT_TRUE(askStacker->_hedge->_usedByOtherStack);
    assertLevelState(&askStacker->_hedge, ToRhonePrice(.28), 6, 0, 6, 0, 0, 0, {7});

    onOrderReplaced(bidStacker, 40, *getOrder(bidStacker, 3));
    assertLevelState(bidLevels, ToRhonePrice(.27), 40, 40, 0, 0, 0, 0, {3});

    onOrderReplaced(bidStacker, 60, *getOrder(bidStacker, 1));
    assertLevelState(bidLevels, ToRhonePrice(.29), 0, 6, 0, 6, 0, 0, {});
    // Since order 1 partially filled 6 lot, a new order should be placed to make up the 6 lot
    assertLevelState(bidLevels, ToRhonePrice(.25), 60, 54, 6, 0, 0, 0, {1, 8});

    onOrderReplaced(bidStacker, 60, *getOrder(bidStacker, 2));
    ASSERT_FALSE(askStacker->_hedge->_usedByOtherStack);
    ASSERT_FALSE(findLevel(bidLevels, ToRhonePrice(0.28))->_usedByOtherStack);
    assertLevelState(bidLevels, ToRhonePrice(.28), 0, 0, 0, 0, 0, 0, {});
    assertLevelState(bidLevels, ToRhonePrice(.26), 60, 60, 0, 0, 0, 0, {2});

    onOrderAccepted(askStacker, 6, *getOrder(askStacker, 7));
    assertLevelState(&askStacker->_hedge, ToRhonePrice(.28), 6, 6, 0, 0, 0, 0, {7});

    onOrderAccepted(bidStacker, 6, *getOrder(bidStacker, 8));
    assertLevelState(bidLevels, ToRhonePrice(.25), 60, 60, 0, 0, 0, 0, {1, 8});
}

}