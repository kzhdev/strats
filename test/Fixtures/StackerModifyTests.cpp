//
// Created by cmilutinovic on 6/24/19.
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

    void StackerFixture::setupCMEModifyChain(std::unordered_map<om::order_id_t, om::order_t*>& orders, Stacker<TestIME_2>::Level**& level) {
        orders = std::unordered_map<om::order_id_t, om::order_t*>{};

        //TODO -- The commented out asserts appear we don't mark a level as free until we get the ack.  We could though!  Maybe?

        hf::price_t tickSize = ToRhonePrice(0.01);
        uint32_t nLevels = 2;
        hf::qty_t stackSize = 60;
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();
        _ime2->setVenue(hf::VENUE::CME);
        stacker->initialize(1, BID, tickSize, stackSize, nLevels, _ime2, "name", hf::VENUE::CME, 2, false);
        //We need an ask stacker for askSideThinksLevelIsUsed() to function
        initAskStackerLevels(ToRhonePrice(.10), ToRhonePrice(.50));

        hf::price_t bidPrice = ToRhonePrice(.31);
        const hf::price_t originalBidPrice = bidPrice;
        hf::qty_t quoteSize = 40;
        stacker->quote<BID>(bidPrice, quoteSize, goThroughAllLevels);
        Stacker<TestIME_2>::Level* topLevel = stacker->_topLevel;
        assertState(topLevel, bidPrice, 40, 0, 40, 0);

        level = new Stacker<TestIME_2>::Level*[22];
        level[0] = topLevel;
        for(int i = 0; i < 20; ++i) {
            level[i+1] = level[i]->after;
        }

        ASSERT_TRUE(askSideThinksLevelIsUsed(level[0]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[1]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[2]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[3]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[4]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[5]));

        //Assert level=40, level 2-3=60
        const auto& assertStack = [this, quoteSize, stackSize, nLevels, bidPrice, &level](Stacker<TestIME_2>::Level* level) {
            ASSERT_EQ(bidPrice, level[0]->_price);
            ASSERT_EQ(quoteSize, level[0]->_target);
            ASSERT_EQ(1, level[0]->_orders.size());
            for(int i = 0; i < (int)nLevels - 1; ++i) {
                ASSERT_EQ(stackSize, level[i + 1]->_target);
                ASSERT_EQ(1, level[i + 1]->_orders.size());
            }
        };
        assertStack(level[0]);

        orders[1] = level[0]->_orders[0];
        orders[2] = level[1]->_orders[0];
        orders[3] = level[2]->_orders[0];
        ASSERT_TRUE(std::all_of(orders.begin(), orders.end(), [](const auto& orderPair) { return orderPair.first == orderPair.second->oid; }));

        ASSERT_FALSE(level[0]->canModifyOrCancelOrder(*level[0]->_orders[0]));
        onOrderAccepted(stacker, quoteSize, *topLevel->_orders[0]);
        assertState(level[0], level[0]->_price, quoteSize, quoteSize, 0, 0, 0);
        ASSERT_TRUE(level[0]->canModifyOrCancelOrder(*level[0]->_orders[0]));
        for(int i = 0; i < nLevels; ++i) {
            onOrderAccepted(stacker, stackSize, *(topLevel+i+1)->_orders[0]);
        }

        //Move level 1, should move order from front to back and modify down inside level
        auto* previousBidPriceLevel = level[0];
        bidPrice = bidPrice - tickSize;
        stacker->quote<BID>(bidPrice, quoteSize, goThroughAllLevels);
        Stacker<TestIME_2>::Level* baseLevel = level[1];
        assertState(previousBidPriceLevel, previousBidPriceLevel->_price, 0, 40, -40, 0, 40);
        assertState(baseLevel, baseLevel->_price, quoteSize, stackSize, -20, 0);
        assertState(baseLevel+nLevels, (baseLevel+nLevels)->_price, stackSize, 0, stackSize, 0, 0);
        ASSERT_EQ((baseLevel+nLevels)->_orders[0]->oid, 1);
        ASSERT_TRUE(!(baseLevel+nLevels)->isLevelFree());
        ASSERT_EQ(previousBidPriceLevel->_orders.size(), 0);
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[1]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[2]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
        //ASSERT_FALSE(askSideThinksLevelIsUsed(level[0]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[4]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[5]));

        //Now we want to generate modifies in flight.  We're going to modify the quantity down twice, then move the stack again
        stacker->quote<BID>(bidPrice, quoteSize - 1, goThroughAllLevels);
        assertState(baseLevel, baseLevel->_price, quoteSize - 1, stackSize, (quoteSize-stackSize) - 1, 0, 0);
        stacker->quote<BID>(bidPrice, quoteSize - 2, goThroughAllLevels);
        assertState(baseLevel, baseLevel->_price, quoteSize - 2, stackSize, (quoteSize-stackSize) - 2, 0, 0);
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[1]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[2]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
        //ASSERT_FALSE(askSideThinksLevelIsUsed(level[0]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[4]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[5]));

        bidPrice = bidPrice - tickSize;
        baseLevel++;
        stacker->quote<BID>(bidPrice, quoteSize, goThroughAllLevels);
        assertState(baseLevel-1, (baseLevel-1)->_price, 0, stackSize, -stackSize, 0, quoteSize-2);
        assertState(baseLevel, baseLevel->_price, quoteSize, stackSize, (quoteSize-stackSize), 0, 0);
        auto* topOfStack = baseLevel + 2;
        assertState(topOfStack, topOfStack->_price, stackSize, 0, 60, 0, 0);
        ASSERT_EQ(topOfStack->_orders[0]->oid, 2);

        //Our state should now look like this:
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -60     38
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0

        //First OID is 1, not 0
        //First quote (Moves Stack .31->.30):
        //oid2 -> 60@.30 -> 40@.30
        //oid1 -> 40@.31 -> 60@.28
        //Second Quote (Change Quote Qty 40@.30->39@.30):
        //oid2 -> 40@.30 -> 39@.30
        //Third Quote (Change Quote Qty 39@.30->38@.30):
        //oid2 -> 39@.30 -> 38@.30
        //Fourth Quote (Moves Stack .30->.29):
        //oid3 -> 60@.29 -> 40@.29
        //oid2 -> 38@.30 -> 60@.27

        //We'll assert that
        assertState(level[0], ToRhonePrice(.31), 0, 40, -40, 0, 40); //Note the "traded" column before flightout
        assertState(level[1], ToRhonePrice(.30), 0, 60, -60, 0, 38);
        assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 0, 60, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 60, 0);
        assertState(level[5], ToRhonePrice(.26), 0, 0, 0, 0);
        ASSERT_EQ(level[2]->_orders[0]->oid, 3);
        ASSERT_EQ(level[3]->_orders[0]->oid, 1);
        ASSERT_EQ(level[4]->_orders[0]->oid, 2);

        ASSERT_TRUE(askSideThinksLevelIsUsed(level[2]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[4]));
        //ASSERT_FALSE(askSideThinksLevelIsUsed(level[0]));
        //ASSERT_FALSE(askSideThinksLevelIsUsed(level[1]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[5]));
    }

    bool StackerFixture::askSideThinksLevelIsUsed(Stacker<TestIME_2>::Level* level) {
        Stacker<TestIME_2>* otherStacker = _ime2->getAskStacker();
        return _ime2->getLevel(otherStacker, level->_price)->_usedByOtherStack;
    }

    void StackerFixture::initAskStackerLevels(hf::price_t startPrice, hf::price_t endPrice) {
        Stacker<TestIME_2>* otherStacker = _ime2->getAskStacker();
        Stacker<TestIME_2>::Level* currentLevel = otherStacker->_stackerStart = otherStacker->_topLevel = new Stacker<TestIME_2>::Level{};
        currentLevel->_price = endPrice;
        currentLevel->_stacker = otherStacker;
        while(startPrice != endPrice) {
            currentLevel->after = new Stacker<TestIME_2>::Level{};
            currentLevel = currentLevel->after;
            endPrice = currentLevel->_price = endPrice - ToRhonePrice(.01);
            currentLevel->_stacker = otherStacker;
        }
    }

    void StackerFixture::performKnownGoodModifyAcks(std::unordered_map<om::order_id_t, om::order_t*>& orders) {
        //Perform the acks as in SimpleModifyTest, which will ack all the modifies sent in setupCMEModifyChain()
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        //For reference, here is the modify chain for Order2:
        //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]

        onOrderReplaced(stacker, 40, *orders[2]);
        onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
        onOrderReplaced(stacker, 39, *orders[2]);
        onOrderReplaced(stacker, 38, *orders[2]);
        onOrderReplaced(stacker, 40, *orders[3]);
        onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]);
    }


    TEST_F(StackerFixture, CMEModifyChainBaseTest) {
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** levels;
        setupCMEModifyChain(orders, levels);
    }


    TEST_F(StackerFixture, SimpleModifyTest) {

        //Verify all the modifies in the chain, when acked, execute as expected
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        //Starting State:
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -60     38
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0

        //First quote (Moves Stack .31->.30):
        //oid2 -> 60@.30 -> 40@.30
        onOrderReplaced(stacker, 40, *orders[2]);
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       40   -40     38
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //3 mod in flight
        //26    0
        assertState(level[1], ToRhonePrice(.30), 0, 40, -40, 0, 38);

        //oid1 -> 40@.31 -> 60@.28
        onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
        //price _target _ack _flight _flightout oid
        //31    0       0   0     0
        //30    0       40   -40     38
        //29    40      60   -20     0      3  //1 mod in flight
        //28    60      60   0      0       1  //0 mod in flight
        //27    60       0   60      0      2  //3 mod in flight
        //26    0
        assertState(level[0], ToRhonePrice(.31), 0, 0, 0, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        ASSERT_TRUE(level[0]->isLevelFree());
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[0]));

        //Second Quote (Change Quote Qty 40@.30->39@.30):
        //oid2 -> 40@.30 -> 39@.30
        onOrderReplaced(stacker, 39, *orders[2]);
        //price _target _ack _flight _flightout oid
        //31    0       0   0     0
        //30    0       39   -39     38
        //29    40      60   -20     0      3  //1 mod in flight
        //28    60      60   0      0       1  //0 mod in flight
        //27    60       0   60      0      2  //2 mod in flight
        //26    0
        assertState(level[1], ToRhonePrice(.30), 0, 39, -39, 0, 38);

        //Third Quote (Change Quote Qty 39@.30->38@.30):
        //oid2 -> 39@.30 -> 38@.30
        onOrderReplaced(stacker, 38, *orders[2]);
        //price _target _ack _flight _flightout oid
        //31    0       0   0     0
        //30    0       38   -38     38
        //29    40      60   -20     0     3  //1 mod in flight
        //28    60      60   0      0      1  //0 mod in flight
        //27    60       0   60      0     2  //1 mod in flight
        //26    0
        assertState(level[1], ToRhonePrice(.30), 0, 38, -38, 0, 38);

        //Fourth Quote (Moves Stack .30->.29):
        onOrderReplaced(stacker, 40, *orders[3]);
        //price _target _ack _flight _flightout oid
        //31    0       0   0     0
        //30    0       38   -38     38
        //29    40      40   0       0     3  //0 mod in flight
        //28    60      60   0      0      1  //0 mod in flight
        //27    60       0   60      0     2  //1 mod in flight
        //26    0
        assertState(level[2], ToRhonePrice(.29), 40, 40, 0, 0);


        onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]);
        //Our state should now look like this:
        //price _target _ack _flight  oid
        //31    0        0   0
        //30    0        0   0
        //29    40       40   0     3
        //28    60       60   0     1
        //27    60       60   0     2
        //26    0

        //We'll assert that
        assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
        assertState(level[1], ToRhonePrice(.30), 0,  0, 0, 0, 0);
        assertState(level[2], ToRhonePrice(.29), 40, 40, 0, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 60, 0, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);
        ASSERT_EQ(level[2]->_orders[0], orders[3]);
        ASSERT_EQ(level[3]->_orders[0], orders[1]);
        ASSERT_EQ(level[4]->_orders[0], orders[2]);
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[1]));
    }

    TEST_F(StackerFixture, SimpleModifyChainWithTargetZero) {
        //We're going to set the target as zero on the last level, and verify that the right thing happens
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        stacker->quote<BID>(ToRhonePrice(0.29), 0, goThroughAllLevels);

        //Our state should now look like this:
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -60     38
        //29    0      60   -60     0          3  //2 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0
        assertState(level[2], ToRhonePrice(0.29), 0, 60, -60, 0, 0);

        performKnownGoodModifyAcks(orders);
        //29    0       40   -40     3
        assertState(level[2], ToRhonePrice(.29), 0, 40, -40, 0, 0);
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
        onOrderCanceled(stacker, *orders[3]);
        assertState(level[2], ToRhonePrice(.29), 0, 0, 0, 0, 0);
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[2]));
    }

TEST_F(StackerFixture, SimpleModifyChainWithTargetZeroThenNonZero) {
        //We're going to set the target as zero on the last level, and verify that the right thing happens
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        stacker->quote<BID>(ToRhonePrice(0.29), 0, goThroughAllLevels);
        stacker->quote<BID>(ToRhonePrice(0.29), 10, goThroughAllLevels);

        //Our state should now look like this:
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -60     38
        //29    10      60   -50     0          3  //2 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0
        assertState(level[2], ToRhonePrice(0.29), 10, 60, -50, 0, 0);

        performKnownGoodModifyAcks(orders);
        //29    0       40   -40     3
        assertState(level[2], ToRhonePrice(.29), 10, 40, -30, 0, 0);
        ASSERT_EQ(level[2]->_orders.size(), 2);
        onOrderCanceled(stacker, *orders[3]);
        assertState(level[2], ToRhonePrice(.29), 10, 0, 10, 0, 0);
        ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
        onOrderAccepted(stacker, 10, *level[2]->_orders[0]);
        assertState(level[2], ToRhonePrice(.29), 10, 10, 0, 0, 0);
    }

TEST_F(StackerFixture, SimpleModifyChainWithPartilFill) {
    //The simply modify chain looks like this:
    //First OID is 1, not 0
    //First quote (Moves Stack .31->.30):
    //oid2 -> 60@.30 -> 40@.30
    //oid1 -> 40@.31 -> 60@.28
    //Second Quote (Change Quote Qty 40@.30->39@.30):
    //oid2 -> 40@.30 -> 39@.30
    //Third Quote (Change Quote Qty 39@.30->38@.30):
    //oid2 -> 39@.30 -> 38@.30
    //Fourth Quote (Moves Stack .30->.29):
    //oid3 -> 60@.29 -> 40@.29
    //oid2 -> 38@.30 -> 60@.27

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //Starting State:
    //price _target _ack _flight _flightout oid
    //31    0       40   -40     40
    //30    0       60   -60     38
    //29    40      60   -20     0          3  //1 mod in flight
    //28    60       0   60      0          1  //1 mod in flight
    //27    60       0   60      0          2  //4 mod in flight
    //26    0


    //We're going to generate a fill on oid=2 where it is resting 10@(0.30)
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 10, true);

    //New state should "undo" the fill
    //31    0       40   -40     40
    //30    0       60   -58     36   10
    //29    40      60   -20     0          3  //1 mod in flight
    //28    60       0   60      0          1  //1 mod in flight
    //27    60       0   50      0          2  //4 mod in flight
    //26    0

    assertState(level[0], ToRhonePrice(.31), 0, 40, -40, 0, 40); //Note the "traded" column before flightout
    assertState(level[1], ToRhonePrice(.30), 0, 60, -50, 10, 28);
    assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0);
    assertState(level[3], ToRhonePrice(.28), 60, 0, 60, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 0, 50, 0);
    assertState(level[5], ToRhonePrice(.26), 0, 0, 0, 0);

    //TODO: NOTE THE 50!!!!!  We don't call syncOrders() on fill because _target isn't update until after the fill,
    //however maybe we should for levels that are not the fill level?
    performKnownGoodModifyAcks(orders);
    //State should be the same as before, except 50 at level[4] with 10 in flight (and 2 orders)
    //This order was added at onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]), since we call syncOrder() on its own level
    assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
    assertState(level[1], ToRhonePrice(.30), 0,  10, 0, 10, 0);
    assertState(level[2], ToRhonePrice(.29), 40, 40, 0, 0, 0);
    assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 50, 10, 0, 0);
    assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

    ASSERT_EQ(level[4]->_orders.size(), 2);
    ASSERT_EQ(level[4]->_orders[1]->qty, 10);

    ASSERT_TRUE(askSideThinksLevelIsUsed(level[2]));
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4]));

    ASSERT_FALSE(askSideThinksLevelIsUsed(level[0]));
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1]));
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[5]));

    //Let's ack that 10 lot and see what happens
    onOrderAccepted(stacker, 10, *level[4]->_orders[1]);
    assertState(level[4], ToRhonePrice(.27), 60, 60, 0, 0, 0);
}


TEST_F(StackerFixture, SimpleModifyChainWithFullFill) {

        //We're going to fully fill order 2
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        //Starting State:
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -60     38
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0


        //We're going to generate a fill on oid=2 where it is resting 60@(0.30)
        onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 60, true);

        //New state should "undo" the fill
        //31    0       40   -40     40
        //30    0       60    0     0   60
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0

        assertState(level[0], ToRhonePrice(.31), 0, 40, -40, 0, 40); //Note the "traded" column before flightout
        assertState(level[1], ToRhonePrice(.30), 0, 60, 0, 60, 0);
        assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 0, 60, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0, 0, 0, 0);


        //See comments -- not sure when we should deal with this intermediate values
        //        ASSERT_TRUE(askSideThinksLevelIsUsed(level[2]));
        //        ASSERT_TRUE(askSideThinksLevelIsUsed(level[3]));
        //        ASSERT_TRUE(askSideThinksLevelIsUsed(level[4]));
        //        ASSERT_FALSE(askSideThinksLevelIsUsed(level[0]));
        //        ASSERT_FALSE(askSideThinksLevelIsUsed(level[1]));
        //        ASSERT_FALSE(askSideThinksLevelIsUsed(level[5]));


        onOrderReplaceRejected(stacker, *orders[2], 2045);
        onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
        onOrderReplaceRejected(stacker, *orders[2], 2045);
        onOrderReplaceRejected(stacker, *orders[2], 2045);
        onOrderReplaced(stacker, 40, *orders[3]);
        onOrderReplaceRejected(stacker, *orders[2], 2045);

        //We should be in a good state after this
        assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
        assertState(level[1], ToRhonePrice(.30), 0,  60, 0, 60, 0);
        assertState(level[2], ToRhonePrice(.29), 40, 40, 0, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

        //TODO: that .27 level looks awfully wrong, because we haven't synchronized() to deal with the full fill.  When!?

        //TODO: For the heck of it, let's quote at .30 and see what happens
    }


TEST_F(StackerFixture, SimpleModifyChainWithPartialFillThenFullFill) {
    //This test is extremely similar to SimpleModifyChainWithFullFill, however we have a partial fill then a full fill

    //Everything up to the fill is going to be copied & pasted from SimpleModifyChainWithPrtialFillThenFullFill(...)
    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //Starting State:
    //price _target _ack _flight _flightout oid
    //31    0       40   -40     40
    //30    0       60   -60     38
    //29    40      60   -20     0          3  //1 mod in flight
    //28    60       0   60      0          1  //1 mod in flight
    //27    60       0   60      0          2  //4 mod in flight
    //26    0

    //Now we're going to generate a partial fill, then a full fill.
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 2, true);
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 58, true);

    //We should be in the same state as the FullFill test (without the partial) before and after the rejects

    assertState(level[0], ToRhonePrice(.31), 0, 40, -40, 0, 40); //Note the "traded" column before flightout
    assertState(level[1], ToRhonePrice(.30), 0, 60, 0, 60, 0);
    assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0);
    assertState(level[3], ToRhonePrice(.28), 60, 0, 60, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0);
    assertState(level[5], ToRhonePrice(.26), 0, 0, 0, 0);

    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaced(stacker, 40, *orders[3]);
    onOrderReplaceRejected(stacker, *orders[2], 2045);

    //We should be in the same state as the FullFill test (without the partial)
    assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
    assertState(level[1], ToRhonePrice(.30), 0,  60, 0, 60, 0);
    assertState(level[2], ToRhonePrice(.29), 40, 40, 0, 0, 0);
    assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
    assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

    //TODO: figure out if level should be free
}

TEST_F(StackerFixture, CancelDueToDoOverfillOnModifyDown_ZeroLeaves) {
        //We're going to do the standard setup, but modify down s.t. the modify from 39 to 38 is going to result in a cancel

        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        //Chain
        onOrderReplaced(stacker, 40, *orders[2]);
        onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
        //price _target _ack _flight _flightout oid
        //31    0       0   0     0
        //30    0       40   -40     38
        //29    40      60   -20     0      3  //1 mod in flight
        //28    60      60   0      0       1  //0 mod in flight
        //27    60       0   60      0      2  //3 mod in flight
        //26    0

        //Fill
        onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 38, true);
        //price _target _ack _flight _flightout oid
        //31    0       0   0     0
        //30    0       40   -2     0
        //29    40      60   -20     0      3  //1 mod in flight
        //28    60      60   0      0       1  //0 mod in flight
        //27    60       0   22      0       2  //2 mod in flight
        //26    0
        assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
        assertState(level[1], ToRhonePrice(.30), 0,  40, -2, 38, 0);
        assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 22, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

        //TODO, performance enhancement, if flightout touches 0 we can do interesting things

        //Chain Continues
        onOrderReplaced(stacker, 39, *orders[2]);
        assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
        assertState(level[1], ToRhonePrice(.30), 0,  39, -1, 38, 0); //We don't touch flightout
        assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 22, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

        //This is going to be the last ack before unsoc. cancel
        onOrderReplaced(stacker, 38, *orders[2]);
        assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
        assertState(level[1], ToRhonePrice(.30), 0,  38, 0, 38, 0);
        assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 22, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

        //This is kind of an easy test, because we're *exactly* fully filled and not overfilled
        onOrderCanceled(stacker, *orders[2]);
        assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
        assertState(level[1], ToRhonePrice(.30), 0,  38, 0, 38, 0);
        assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

        //We should be in the same state after the two modify rejects come in
        onOrderReplaceRejected(stacker, *orders[2], 2045);
        onOrderReplaceRejected(stacker, *orders[2], 2045);
        assertState(level[0], ToRhonePrice(.31), 0,  0, 0, 0, 0);
        assertState(level[1], ToRhonePrice(.30), 0,  38, 0, 38, 0);
        assertState(level[2], ToRhonePrice(.29), 40, 60, -20, 0, 0);
        assertState(level[3], ToRhonePrice(.28), 60, 60, 0, 0, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
        assertState(level[5], ToRhonePrice(.26), 0,  0, 0, 0, 0);

}

TEST_F(StackerFixture, CancelDueToDoOverfillOnModifyDown_ForceSameLevelAdjustment_ZeroLeaves) {
        //We're going to fill s.t. the first modify down is fully filled, to force the Stacker to handle
        //adjusting the quantity when the level doesn't change

        //For reference, here is the modify chain for Order2:
        //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]

        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        //Starting State:
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -60     38
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0

        //Order2 is currently 60@30
        onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 39, true);
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -21     -1    fil=39
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   21      0          2  //3 mod in flight
        //26    0
        assertState(level[1], ToRhonePrice(.30), 0, 60, -21, 39, -1);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 21, 0, 0);

        //The next ack is still fine, we filled 39 and are modding to 40
        onOrderReplaced(stacker, 40, *orders[2]);
        //31    0       40   -40     40
        //30    0       40   -1     -1         fil=39
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   21      0          2  //3 mod in flight
        //26    0
        assertState(level[1], ToRhonePrice(.30), 0, 40, -1, 39, -1);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 21, 0, 0);

        //This ack is going to result in a modify ack then a cancel
        onOrderReplaced(stacker, 39, *orders[2]);
        //31    0       40   -40     40
        //30    0       39   0      -1         fil=39
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   21      0          2  //3 mod in flight
        //26    0
        assertState(level[1], ToRhonePrice(.30), 0, 39, 0, 39, -1);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 21, 0, 0);

        //Now we're going to get a cancel because we're fully filled.  Should
        //end up in the same state as CancelDueToDoOverfillOnModifyDown_ZeroLeaves test
        onOrderCanceled(stacker, *orders[2]);
        assertState(level[1], ToRhonePrice(.30), 0,  39, 0, 39, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);

        //We should be in the same state after the two modify rejects come in
        onOrderReplaceRejected(stacker, *orders[2], 2045);
        onOrderReplaceRejected(stacker, *orders[2], 2045);
        assertState(level[1], ToRhonePrice(.30), 0,  39, 0, 39, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
}

TEST_F(StackerFixture, CancelDueToOverFillWhenChaningLevel_NonZeroLeaves){
        //On the initial modify down, we're going to greatly overfill the order
        //For reference, here is the modify chain for Order2:
        //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]

        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        //Starting State:
        //price _target _ack _flight _flightout oid
        //31    0       40   -40     40
        //30    0       60   -60     38
        //29    40      60   -20     0          3  //1 mod in flight
        //28    60       0   60      0          1  //1 mod in flight
        //27    60       0   60      0          2  //4 mod in flight
        //26    0

        //Order2 is currently 60@30
        onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 51, true);

        //price _target _ack _flight _flightout oid
        //30    0       60   -21     -1    fil=39
        //27    60       0   21      0          2  //3 mod in flight
        assertState(level[1], ToRhonePrice(.30), 0, 60, -9, 51, -13);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 9, 0, 0);

        //Now we're going to get a cancel because we're fully filled.  Should
        //end up in the same state as CancelDueToDoOverfillOnModifyDown_ZeroLeaves test
        onOrderReplaced(stacker, 40, *orders[2]);
        onOrderCanceled(stacker, *orders[2]);
        assertState(level[1], ToRhonePrice(.30), 0,  51, 0, 51, 0);
        assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
}

TEST_F(StackerFixture, CancelDueToOverFillWhenNotLevel_NonZeroLeaves){
        //We really want an overfill test where the prior modify isn't on the same level.
        //Unfortunately the only qty modify we have on the same level

        //Actually . . . isn't this exactly the same as CancelDueToDoOverfillOnModifyDown_ZeroLeaves?
}

TEST_F(StackerFixture, SMPRejectOnModifySameModifiesAfterReject) {
    //On the initial modify down, we're going to greatly overfill the order
    //For reference, here is the modify chain for Order2:
    //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //We're going to generate a SMP reject on 38@30->60@.27
    //For reference, the chain:
    //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]

    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);
    onOrderReplaced(stacker, 38, *orders[2]);
    onOrderReplaced(stacker, 40, *orders[3]);

    //On the CME this generates a OM + Unsocilited Cancel
    onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]);
    onOrderCanceled(stacker, *orders[2]);

    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4])); //px=.27

    assertState(level[1], ToRhonePrice(.30), 0, 0, 0, 0, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);

    onOrderReplaceRejected(stacker, *orders[2], 2045);  //Note this is same level
    //Nothing should have changed
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4])); //px=.27
    assertState(level[1], ToRhonePrice(.30), 0, 0, 0, 0, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
}

TEST_F(StackerFixture, SMPRejectOnModifyDiffLevelModifiesAfterReject) {

   //We're going to have to force order[2] to move again to really test what we want to test
   std::unordered_map<om::order_id_t, om::order_t*> orders;
   Stacker<TestIME_2>::Level** level;
   setupCMEModifyChain(orders, level);
   Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

   stacker->quote<BID>(ToRhonePrice(0.26), 100, goThroughAllLevels);
   stacker->quote<BID>(ToRhonePrice(0.23), 100, goThroughAllLevels);
   stacker->quote<BID>(ToRhonePrice(0.20), 100, goThroughAllLevels);

   onOrderReplaced(stacker, 40, *orders[2]);
   onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
   onOrderReplaced(stacker, 39, *orders[2]);
   onOrderReplaced(stacker, 38, *orders[2]);
   onOrderReplaced(stacker, 40, *orders[3]);

   assertState(level[1], ToRhonePrice(.30), 0, 38, -38, 0, 38);
   assertState(level[4], ToRhonePrice(.27), 0, 0, 0, 0, 60);
   assertState(level[6], ToRhonePrice(.25), 0, 0, 0, 0, 60);
   assertState(level[10], ToRhonePrice(.21), 0, 0, 0, 0, 60);
   assertState(level[12], ToRhonePrice(.19), 60, 0, 60, 0, 0);

   onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]);
   onOrderCanceled(stacker, *orders[2]);

   assertState(level[1], ToRhonePrice(.30), 0, 0, 0, 0, 0);
   assertState(level[4], ToRhonePrice(.27), 0, 0, 0, 0, 0);
   assertState(level[6], ToRhonePrice(.25), 0, 0, 0, 0, 0);
   assertState(level[10], ToRhonePrice(.21), 0, 0, 0, 0, 0);
   assertState(level[12], ToRhonePrice(.19), 60, 0, 0, 0, 0);
}

TEST_F(StackerFixture, SMPRejectOnModifyComplexPath) {

    //This is going to be very similar to SMPRejectOnModifyDiffLevelModifiesAfterReject however the path is
    //going to be a lot more complex

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //Lots more changes here to get to the same destination
    stacker->quote<BID>(ToRhonePrice(0.26), 100, goThroughAllLevels);
    stacker->updateQuoteSize(61, true);
    stacker->updateQuoteSize(66, true);
    stacker->quote<BID>(ToRhonePrice(0.26), 102, goThroughAllLevels);
    stacker->quote<BID>(ToRhonePrice(0.26), 100, goThroughAllLevels);
    stacker->quote<BID>(ToRhonePrice(0.23), 100, goThroughAllLevels);
    stacker->updateQuoteSize(40, true);
    stacker->updateQuoteSize(39, true);
    stacker->quote<BID>(ToRhonePrice(0.20), 100, goThroughAllLevels);
    stacker->updateQuoteSize(60, true);

    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);
    onOrderReplaced(stacker, 38, *orders[2]);
    onOrderReplaced(stacker, 40, *orders[3]);

    //These asserts *WERE NOT CALCULATED* but were taken directly from what the stacker did.
    //If there is a bugfix in the future it would be expected for these to not be true anymore.
    //These are not going to be the same as the previous since quote/level quantities are going to,
    //at minimum, change flightout
    assertState(level[1], ToRhonePrice(.30), 0, 38, -38, 0, 38);
    assertState(level[4], ToRhonePrice(.27), 0, 0, 0, 0, 60);
    assertState(level[6], ToRhonePrice(.25), 0, 0, 6, 0, 60);
    ASSERT_EQ(level[6]->_orders.size(), 2); //Where the 6 comes from
    assertState(level[10], ToRhonePrice(.21), 0, 0, 0, 0, 39);
    assertState(level[12], ToRhonePrice(.19), 60, 0, 60, 0, 0);

    onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]);
    onOrderCanceled(stacker, *orders[2]);

    assertState(level[1], ToRhonePrice(.30), 0, 0, 0, 0, 0);
    assertState(level[4], ToRhonePrice(.27), 0, 0, 0, 0, 0);
    assertState(level[6], ToRhonePrice(.25), 0, 0, 6, 0, 0);
    ASSERT_EQ(level[6]->_orders[0]->qty + level[6]->_orders[1]->qty, 6);
    assertState(level[10], ToRhonePrice(.21), 0, 0, 0, 0, 0);
    assertState(level[12], ToRhonePrice(.19), 60, 0, 21, 0, 0);
    ASSERT_EQ(level[12]->_orders[0]->qty, 21);
}


//SMP modify reject with fill
TEST_F(StackerFixture, SimpleCancel) {
        //We're going to shrink stack size by 1 to force a cancel
        std::unordered_map<om::order_id_t, om::order_t*> orders;
        Stacker<TestIME_2>::Level** level;
        setupCMEModifyChain(orders, level);
        Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

        performKnownGoodModifyAcks(orders);

        stacker->updateNumberOfLevels<BID>(1, true);

        //Last Level:
        //27    60       0   -60      0
        assertState(level[4], ToRhonePrice(0.27), 0, 60, -60, 0, 0);
        onOrderCanceled(stacker, *orders[2]);
        assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);
        ASSERT_FALSE(askSideThinksLevelIsUsed(level[4]));
}

TEST_F(StackerFixture, CancelRejectDueToFullFillOnSameLevel) {
    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //We're going to repeat the simple case, except fully fill order[2] at the end of the chain.  This will generate a cancel reject.
    performKnownGoodModifyAcks(orders);
    stacker->updateNumberOfLevels<BID>(1, true);

    //Last Level:
    //27    60       0   -60      0
    assertState(level[4], ToRhonePrice(0.27), 0, 60, -60, 0, 0);
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4]));
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.27), 60, false);
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4]));
    // ?
    onOrderCancelRejected(stacker, *orders[2], 2045);
    assertState(level[4], ToRhonePrice(0.27), 0, 60, 0, 60, 0);
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4]));
}

TEST_F(StackerFixture, CancelRejectDueToPartialFillOnSameLevel) {
    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //We're going to repeat the simple case, except fully fill order[2] at the end of the chain.  This will generate a cancel reject.
    performKnownGoodModifyAcks(orders);
    stacker->updateNumberOfLevels<BID>(1, true);

    //Last Level:
    //27    60       0   -60      0
    assertState(level[4], ToRhonePrice(0.27), 0, 60, -60, 0, 0);
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4]));
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.27), 57, false);
    assertState(level[4], ToRhonePrice(0.27), 0, 60, -3, 57, 0);
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4]));
    //
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.27), 3, true);
    onOrderCancelRejected(stacker, *orders[2], 2045);
    assertState(level[4], ToRhonePrice(0.27), 0, 60, 0, 60, 0);
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4]));
}


TEST_F(StackerFixture, CancelRejectDueToFullFillOnDiffLevel) {

    //Very similar to CancelRejectDueToFullFillOnSameLevel, except we're going to change the stack size *before* doing the chain of confirmations
    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //We're going to repeat the simple case, except fully fill order[2] at the end of the chain.  This will generate a cancel reject.
    stacker->updateNumberOfLevels<BID>(1, true);

    //For reference, the chain:
    //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27] -> [0@0]
    //We're going to fully fill after 38@30.
    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);
    onOrderReplaced(stacker, 38, *orders[2]);

    //Full fill here
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4])); //px=.27
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 38, false);

    assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);

    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4])); //px=.27

    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderCancelRejected(stacker, *orders[2], 2045);

    assertState(level[1], ToRhonePrice(0.30), 0, 38, 0, 38, 0);
    assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);
}

TEST_F(StackerFixture, CancelRejectDueToPartialFillOnDiffLevel) {
    //Very similar to above test, expect a partial fill then a full fill instead of two full fills.
    //Very similar to CancelRejectDueToFullFillOnSameLevel, except we're going to change the stack size *before* doing the chain of confirmations
    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //We're going to repeat the simple case, except fully fill order[2] at the end of the chain.  This will generate a cancel reject.
    stacker->updateNumberOfLevels<BID>(1, true);
    assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);

    //For reference, the chain:
    //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27] -> [0@0]
    //We're going to fully fill after 38@30.
    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);

    //Partial Fill
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4])); //px=.27
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 31, true);
    assertState(level[1], ToRhonePrice(0.30), 0, 39, -8, 31, 7);
    assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4])); //px=.27

    //Modify Ack
    onOrderReplaced(stacker, 38, *orders[2]);
    assertState(level[1], ToRhonePrice(0.30), 0, 38, -7, 31, 7);
    assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_TRUE(askSideThinksLevelIsUsed(level[4])); //px=.27

    //Full Fill
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(0.30), 7, false);
    assertState(level[1], ToRhonePrice(0.30), 0, 38, 0, 38, 0);
    assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4])); //px=.27

    //Modify Cancel
    //Cancel Reject
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderCancelRejected(stacker, *orders[2], 2045);

    assertState(level[1], ToRhonePrice(0.30), 0, 38, 0, 38, 0);
    assertState(level[4], ToRhonePrice(0.27), 0, 0, 0, 0, 0);
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4])); //px=.27
}

TEST_F(StackerFixture, CancelAfterUnoslicitedCancelDueToModifyDown) {
    //We're going to have a cancel request, but modify down is going to generate an unsolicited cancel

    // Cut & Paste from previous test
    //On the initial modify down, we're going to greatly overfill the order
    //For reference, here is the modify chain for Order2:
    //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    //Except this isn't cut & paste
    stacker->updateNumberOfLevels<BID>(1, true);

    //Starting State:
    //price _target _ack _flight _flightout oid
    //31    0       40   -40     40
    //30    0       60   -60     38
    //29    40      60   -20     0          3  //1 mod in flight
    //28    60       0   60      0          1  //1 mod in flight
    //27    0       0   60      0          2  //4 mod in flight
    //26    0

    //Order2 is currently 60@30
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 51, false);

    //price _target _ack _flight _flightout oid
    //30    0       60   -21     -1    fil=39
    //27    60       0   21      0          2  //3 mod in flight
    assertState(level[1], ToRhonePrice(.30), 0, 60, -9, 51, -13);
    assertState(level[4], ToRhonePrice(.27), 0, 0, 0, 0, 0);

    //Now we're going to get a cancel because we're fully filled.  Should
    //end up in the same state as CancelDueToDoOverfillOnModifyDown_ZeroLeaves test
    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderCanceled(stacker, *orders[2]);
    assertState(level[1], ToRhonePrice(.30), 0,  51, 0, 51, 0);
    assertState(level[4], ToRhonePrice(.27), 0, 0, 0, 0, 0);
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4])); //px=.27

    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderCancelRejected(stacker, *orders[2], 2045);

    assertState(level[1], ToRhonePrice(.30), 0,  51, 0, 51, 0);
    assertState(level[4], ToRhonePrice(.27), 0, 0, 0, 0, 0);
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[1])); //px=.30
    ASSERT_FALSE(askSideThinksLevelIsUsed(level[4])); //px=.27

}

//Trying to reproduce some things we saw in cert, quick and dirty

TEST_F(StackerFixture, PartialFillThenModifyThenFullFillThenModifyReject_CertRepo) {
     //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]
     //We're going to partially fill at while resting 60@0.30, modify ack to 40,
     //modify ack to 39, then fully fill.

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 17, true);

    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);

    onOrderExecuted(stacker, *orders[2], ToRhonePrice(0.28), 22, false);

    onOrderReplaceRejected(stacker, *orders[2], 2045);
    onOrderReplaceRejected(stacker, *orders[2], 2045);

    assertState(level[1], ToRhonePrice(.30), 0,  39, 0, 39, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
}


TEST_F(StackerFixture, PartialFillThenModifyThenFullFillThenModifyRejectDiffLevel_CertRepo) {
    //[60@.30] -> [40@.30] -> [39@.30] -> [38@.30] -> [60@.27]
    //Very similar to above test, but we're going to generate the full fill on the modify to a different level in flight
    //We're going to partially fill at while resting 60@0.30, modify ack to 40,
    //modify ack to 39, modify ack to 38, then fully fill.

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 17, true);

    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);
    onOrderReplaced(stacker, 38, *orders[2]);

    onOrderExecuted(stacker, *orders[2], ToRhonePrice(0.28), 21, false);
    onOrderReplaceRejected(stacker, *orders[2], 2045);

    assertState(level[1], ToRhonePrice(.30), 0,  38, 0, 38, 0);
    assertState(level[4], ToRhonePrice(.27), 60, 0, 0, 0, 0);
}

TEST_F(StackerFixture, FillAndQuoteWithNegativeWorkingQuantityAndTraded) {
    //We want to generate a fill with _flight < 0, traded > 0, target > 0

    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0)

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    auto& crit = *level[1];

    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);

    assertState(level[1], ToRhonePrice(.30), 0, 39, -39, 0, 38);

    //We want to quote that level at 5 to force an order to appear at this level
    ASSERT_EQ(level[1]->_orders.size(), 0);
    stacker->quote<BID>(ToRhonePrice(.30), 2, goThroughAllLevels);
    ASSERT_EQ(level[1]->_orders.size(), 1);
    assertState(level[1], ToRhonePrice(.30), 2, 39, -37, 0, 38);

    //We are now going to generate a fill at this level
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 17, true);
    assertState(level[1], ToRhonePrice(.30), 2, 39, -20, 17, 21);

    //And in response the IME is going to say to quote 0
    //TODO: For ICE, it should cancel the order here
    stacker->quote<BID>(ToRhonePrice(.30), 0, goThroughAllLevels);
    onOrderAccepted(stacker, 2, *crit._orders[0]);
    ASSERT_TRUE(crit._orders[0]->is_cancelling);

    onOrderCanceled(stacker, *crit._orders[0]);
    ASSERT_EQ(crit._orders.size(), 0);
    ASSERT_TRUE(askSideThinksLevelIsUsed(&crit));
    onOrderReplaced(stacker, 38, *orders[2]);
    ASSERT_TRUE(askSideThinksLevelIsUsed(&crit));
    onOrderReplaced(stacker, 40, *orders[3]);
    ASSERT_TRUE(askSideThinksLevelIsUsed(&crit));
    onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]);
    ASSERT_FALSE(askSideThinksLevelIsUsed(&crit));

}

TEST_F(StackerFixture, FillAndQuoteWithNegativeWorkingQuantityAndTraded_FillInFlight) {
    //We want to generate a fill with _flight < 0, traded > 0, target > 0
    //This is *extreemly* similar to the previous test, except that we're going to generate a fill-in-flight when we attempt to cancel the new order

    //void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0)

    std::unordered_map<om::order_id_t, om::order_t*> orders;
    Stacker<TestIME_2>::Level** level;
    setupCMEModifyChain(orders, level);
    Stacker<TestIME_2>* stacker = _ime2->getBidStacker();

    auto& crit = *level[1];

    onOrderReplaced(stacker, 40, *orders[2]);
    onOrderReplaced(stacker, ToRhonePrice(.28), 60, *orders[1]);
    onOrderReplaced(stacker, 39, *orders[2]);

    assertState(level[1], ToRhonePrice(.30), 0, 39, -39, 0, 38);

    //We want to quote that level at 5 to force an order to appear at this level
    ASSERT_EQ(level[1]->_orders.size(), 0);
    stacker->quote<BID>(ToRhonePrice(.30), 2, goThroughAllLevels);
    ASSERT_EQ(level[1]->_orders.size(), 1);
    assertState(level[1], ToRhonePrice(.30), 2, 39, -37, 0, 38);

    //We are now going to generate a fill at this level
    onOrderExecuted(stacker, *orders[2], ToRhonePrice(.30), 17, true);
    assertState(level[1], ToRhonePrice(.30), 2, 39, -20, 17, 21);

    //And in response the IME is going to say to quote 0
    //TODO: For ICE, it should cancel the order here
    stacker->quote<BID>(ToRhonePrice(.30), 0, goThroughAllLevels);
    onOrderAccepted(stacker, 2, *crit._orders[0]);
    ASSERT_TRUE(crit._orders[0]->is_cancelling);
    om::order_t* coo = crit._orders[0];
    onOrderExecuted(stacker, *coo, ToRhonePrice(0.30), 2, false);
    onOrderCancelRejected(stacker, *coo, 2045);

    ASSERT_EQ(crit._orders.size(), 0);
    ASSERT_TRUE(askSideThinksLevelIsUsed(&crit));
    onOrderReplaced(stacker, 38, *orders[2]);
    ASSERT_TRUE(askSideThinksLevelIsUsed(&crit));
    onOrderReplaced(stacker, 40, *orders[3]);
    ASSERT_TRUE(askSideThinksLevelIsUsed(&crit));
    onOrderReplaced(stacker, ToRhonePrice(.27), 60, *orders[2]);
    ASSERT_FALSE(askSideThinksLevelIsUsed(&crit));
}

TEST_F(StackerFixture, ArtificalQuoteCalculation) {
        //Having a super hard time reproducing this bug in unit tests using order actions, however looking at the logs it was obvious what happened
        //
        // 2019-07-02 11:19:39,126 INFO  [Logger.cpp:13] Stacker GEU9. Order Accepted. Id=2713 Price=97.98 Size=1
        // 2019-07-02 11:19:39,126 INFO  [Logger.cpp:13] Stacker GEU9. Before. Price=97.98 Target=13 Acked=24 Flight=-11 Trade=5 NumFlightout=0
        // 2019-07-02 11:19:39,126 INFO  [Logger.cpp:13] Stacker GEU9. New order. Id=2734 Price=97.98 Size=5
        // 2019-07-02 11:19:39,126 INFO  [Logger.cpp:13] Stacker GEU9. After. Price=97.98 Target=13 Acked=25 Flight=-7 Trade=5 NumFlightout=0
        //
        //Basically, moveQuantity is called which does  _acknowledged += qty; _flight -= qty; then getQuantityChange() is called.  Unfortunately
        //getQuantityChange() had a bug due to the untransative nature of operator-() with negative numbers and bad placement of parenthesis
        //that it generated +5 instead of -5.  Oops.

        Stacker<TestIME_2>::Level level;
        level._price = ToRhonePrice(97.98);
        level._target = 13;
        level._acknowledged = 24;
        level._flight = -11;
        level._traded = 5;
        level._flightout = 0;

        level._acknowledged += 1;
        level._flight -= 1;

        ASSERT_EQ(level.getQuantityChange(), -5);
}

} //namespace Python