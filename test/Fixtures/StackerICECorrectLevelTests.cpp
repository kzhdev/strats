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

namespace {

    template<typename FixtureType, typename S1, typename S2>
    struct Duplicator {

        Duplicator(FixtureType *fixturetype, S1 *stacker1, S2 *stacker2) : _cme(stacker1), _ice(stacker2),
                                                                           _ft(fixturetype) {}

        void quote(double price, hf::qty_t qty) {
            _cme->template quote<Python::BID>(Python::ToRhonePrice(price), qty, false);
            _ice->template quote<Python::BID>(Python::ToRhonePrice(price), qty, false);
        }

        void onOrderAccepted(hf::qty_t qty, const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            _ft->onOrderAccepted(_cme, qty, cmeOrder);
            _ft->onOrderAccepted(_ice, qty, iceOrder);
        }

        void onOrderReplaced(hf::qty_t qty, const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            _ft->onOrderReplaced(_cme, qty, cmeOrder);
            _ft->onOrderReplaced(_ice, qty, iceOrder);
        }

        void onOrderCanceled(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            _ft->onOrderCanceled(_cme, cmeOrder);
            _ft->onOrderCanceled(_ice, iceOrder);
        }

        void onOrderExecuted(double price, hf::qty_t qty, bool isActive, const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            _ft->onOrderExecuted(_cme, cmeOrder, Python::ToRhonePrice(price), qty, isActive);
            _ft->onOrderExecuted(_ice, iceOrder, Python::ToRhonePrice(price), qty, isActive);
        }

        void verifyStateNOInFlight(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetLevelForNewAck(cmeOrder, iceOrder);
        }

        void verifyStatePostNOAck(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetRestingLevel(cmeOrder, iceOrder);
        }

        void verifyStateWithModifyInFlight(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetLevelForExchangeAction(cmeOrder, iceOrder);
            verifyGetLevelForCancel(cmeOrder, iceOrder);
            verifyGetLevelForModifyAck(cmeOrder, iceOrder);
        }

        void verifyStatePostModifyAck(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetLevelForExchangeAction(cmeOrder, iceOrder);
            verifyGetLevelForCancel(cmeOrder, iceOrder);
        }

        void verifyStateWithCancelInFlight(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetRestingLevel(cmeOrder, iceOrder);
            verifyGetLevelForExchangeAction(cmeOrder, iceOrder);
            verifyGetLevelForCancel(cmeOrder, iceOrder);
        }

        void verifyStatePostCancelAck(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetRestingLevel(cmeOrder, iceOrder);
            verifyGetLevelForCancel(cmeOrder, iceOrder);
        }

        void verifyStateResting(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetRestingLevel(cmeOrder, iceOrder);
            verifyGetLevelForExchangeAction(cmeOrder, iceOrder);
            verifyGetLevelForCancel(cmeOrder, iceOrder);
        }

        void verifyStateCancelRejectIncoming(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetLevelForCancel(cmeOrder, iceOrder);
        }

        void verifyStateModifyRejectIncoming(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            verifyGetRestingLevel(cmeOrder, iceOrder);
        }


    private:

        void verifyGetLevelForNewAck(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            ASSERT_EQ(_cme->getLevelForNewAcknowledgement(cmeOrder)->_price, _ice->getLevelForNewAcknowledgement(iceOrder)->_price);
        }

        void verifyGetLevelForModifyAck(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            ASSERT_EQ(_cme->getLevelForModifyAcknowledgement(cmeOrder)->_price, _ice->getLevelForModifyAcknowledgement(iceOrder)->_price);
        }

        void verifyGetLevelForExchangeAction(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            ASSERT_EQ(_cme->getLevelForExchangeAction(cmeOrder)->_price, _ice->getLevelForExchangeAction(iceOrder)->_price);
        }

        void verifyGetRestingLevel(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            ASSERT_EQ(_cme->getRestingLevel(cmeOrder)->_price, _ice->getRestingLevel(iceOrder)->_price);
        }

        void verifyGetLevelForCancel(const om::order_t& cmeOrder, const om::order_t& iceOrder) {
            ASSERT_EQ(_cme->getLevelForCancel(cmeOrder)->_price, _ice->getLevelForCancel(iceOrder)->_price);
        }

        S1* _cme;
        S2* _ice;
        FixtureType *_ft;
    };

    template<typename LevelType, typename LevelType2>
    void VerifyBulkOfState(const LevelType *l1, const LevelType2 *l2, const om::order_t &cmeOrder,
                           const om::order_t &iceOrder) {
        //Note level doesn't matter the way the API is defined
        ASSERT_EQ(l1->getTargetQuantity(cmeOrder), l2->getTargetQuantity(iceOrder));
        ASSERT_EQ(l1->getTargetLevel(cmeOrder)->_price, l2->getTargetLevel(iceOrder)->_price);
        ASSERT_EQ(l1->getRestingQty(cmeOrder), l2->getRestingQty(iceOrder));
        ASSERT_EQ(l1->getLeavesQty(cmeOrder), l2->getLeavesQty(iceOrder));
        ASSERT_EQ(l1->isModifying(cmeOrder), l2->isModifying(iceOrder));

        if (l1->isModifying(cmeOrder)) {
            ASSERT_EQ(l1->getInFlightQty(cmeOrder), l2->getInFlightQty(iceOrder));
        }
    }

    template<typename LevelType, typename LevelType2>
    void VerifyBulkOfStateDuringModify(const LevelType *l1, const LevelType2 *l2, const om::order_t &cmeOrder,
                                       const om::order_t &iceOrder) {
        VerifyBulkOfState(l1, l2, cmeOrder, iceOrder);
        ASSERT_EQ(l1->getInFlightQty(cmeOrder), l2->getInFlightQty(iceOrder));
        ASSERT_EQ(l1->getLeavesQuantityOnPriorLevelOnOrderReplaced(cmeOrder), l2->getLeavesQuantityOnPriorLevelOnOrderReplaced(iceOrder));
        ASSERT_EQ(l1->getLeavesQuantityOnCurrentLevelOnOrderReplaced(cmeOrder), l2->getLeavesQuantityOnCurrentLevelOnOrderReplaced(iceOrder));
    }

    template<typename LevelType, typename LevelType2>
    void VerifyBulkOfStateAfterModify(const LevelType *l1, const LevelType2 *l2, const om::order_t &cmeOrder,
                                      const om::order_t &iceOrder) {
        VerifyBulkOfState(l1, l2, cmeOrder, iceOrder);
        ASSERT_EQ(l1->isStillModifyingAfterModifyAck(cmeOrder), l2->isStillModifyingAfterModifyAck(iceOrder));
    }
}

namespace Python {

    TEST_F(StackerFixture, ICE_TwoStackersInit) {
        //Just validates that if we init two stackers we get no surprises for the rest of these tests

        //Generic Init Code
        OrderManager<TestIME>* cme = _stacker;
        OrderManager<TestIME_2>* ice = _ime2->getBidStacker();

        cme->initialize(cmeContract, BID, config_60_0_0, _ime, true);
        ice->initialize(iceContract, BID, config_60_0_0, _ime2, true);

        cme->_ime->_executor->setVenue(hf::VENUE::CME);
        ice->_ime->_executor->setVenue(hf::VENUE::ICE);

        cme->quote<BID>(ToRhonePrice(0.35), 4, false);
        ice->quote<BID>(ToRhonePrice(0.35), 4, false);

        ASSERT_EQ(cme->_venue, hf::VENUE::CME);
        ASSERT_EQ(ice->_venue, hf::VENUE::ICE);

        ASSERT_EQ(cme->_topLevel->_orders[0]->venue, hf::VENUE::CME);
        ASSERT_EQ(ice->_topLevel->_orders[0]->venue, hf::VENUE::ICE);

        //Generic first quote code
        cme->quote<BID>(ToRhonePrice(0.35), 4, false);
        ice->quote<BID>(ToRhonePrice(0.35), 4, false);
        const om::order_t& cmeOrder =  *cme->_topLevel->_orders[0];
        const OrderManager<TestIME>::Level* cme35 = cme->_topLevel;
        const OrderManager<TestIME>::Level* cme34 = cme->_topLevel->after;
        const om::order_t& iceOrder =  *ice->_topLevel->_orders[0];
        const OrderManager<TestIME_2>::Level* ice35 = ice->_topLevel;
        const OrderManager<TestIME_2>::Level* ice34 = ice->_topLevel->after;

        ASSERT_EQ(ice34->_price, cme34->_price);
        ASSERT_EQ(ice35->_price, cme35->_price);
    }

TEST_F(StackerFixture, ICE_NoMoCo_LevelGetters) {

    //Generic Init Code
    OrderManager<TestIME>* cme = _stacker;
    OrderManager<TestIME_2>* ice = _ime2->getBidStacker();
    cme->initialize(cmeContract, BID, config_60_0_0, _ime, true);
    ice->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    cme->_ime->_executor->setVenue(hf::VENUE::CME);
    ice->_ime->_executor->setVenue(hf::VENUE::ICE);
    Duplicator<std::decay_t<decltype(*this)>, std::decay_t<decltype(*cme)>, std::decay_t<decltype(*ice)>> d{this, cme, ice};

    //Generic first quote code
    d.quote(0.35, 4);
    const om::order_t& cmeOrder =  *cme->_topLevel->_orders[0];
    const OrderManager<TestIME>::Level* cme35 = cme->_topLevel;
    const OrderManager<TestIME>::Level* cme34 = cme->_topLevel->after;
    const om::order_t& iceOrder =  *ice->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* ice35 = ice->_topLevel;
    const OrderManager<TestIME_2>::Level* ice34 = ice->_topLevel->after;
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    ASSERT_EQ(cme35->getOriginalOrderQtyForNOAck(cmeOrder), ice35->getOriginalOrderQtyForNOAck(iceOrder));
    ASSERT_EQ(cme35->getInFlightQty(cmeOrder), ice35->getInFlightQty(iceOrder));
    ASSERT_EQ(cme35->getRestingQty(cmeOrder), ice35->getRestingQty(iceOrder));
    ASSERT_EQ(cme35->getLeavesQty(cmeOrder), ice35->getLeavesQty(iceOrder));

    //TODO: So these really should be equal, but we'll let it slide since this is tied to "can we modify this order" which
    //is true for ICE but false for CME.  Also the CME is so well tested at this point we really don't want to touch it
    //unless we discover a true bug
    ASSERT_EQ(cme35->isModifying(cmeOrder), true);
    ASSERT_EQ(ice35->isModifying(iceOrder), false);
    d.onOrderAccepted(4, cmeOrder, iceOrder);
    VerifyBulkOfState(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStatePostNOAck(cmeOrder, iceOrder);

    //Modify order
    d.quote(0.34, 5);
    VerifyBulkOfStateDuringModify(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateWithModifyInFlight(cmeOrder, iceOrder);

    //Modify Ack
    d.onOrderReplaced(5, cmeOrder, iceOrder);
    VerifyBulkOfStateAfterModify(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStatePostModifyAck(cmeOrder, iceOrder);

    //Cancel
    d.quote(0.34, 0);
    VerifyBulkOfState(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateWithCancelInFlight(cmeOrder, iceOrder);

    //Cancel Ack
    d.onOrderCanceled(cmeOrder, iceOrder);
    VerifyBulkOfState(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStatePostCancelAck(cmeOrder, iceOrder);

}


TEST_F(StackerFixture, ICE_NoMoPatialCo_LevelGetters) {

    //Generic Init Code
    OrderManager<TestIME>* cme = _stacker;
    OrderManager<TestIME_2>* ice = _ime2->getBidStacker();
    cme->initialize(cmeContract, BID, config_60_0_0, _ime, true);
    ice->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    cme->_ime->_executor->setVenue(hf::VENUE::CME);
    ice->_ime->_executor->setVenue(hf::VENUE::ICE);
    Duplicator<std::decay_t<decltype(*this)>, std::decay_t<decltype(*cme)>, std::decay_t<decltype(*ice)>> d{this, cme, ice};
    d.quote(0.35, 4);
    const om::order_t& cmeOrder =  *cme->_topLevel->_orders[0];
    const OrderManager<TestIME>::Level* cme35 = cme->_topLevel;
    const OrderManager<TestIME>::Level* cme34 = cme->_topLevel->after;
    const om::order_t& iceOrder =  *ice->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* ice35 = ice->_topLevel;
    const OrderManager<TestIME_2>::Level* ice34 = ice->_topLevel->after;
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    d.onOrderAccepted(4, cmeOrder, iceOrder);
    d.onOrderExecuted(.35, 2, true, cmeOrder, iceOrder);
    VerifyBulkOfState(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateResting(cmeOrder, iceOrder);

    d.quote(0.34, 5);
    d.verifyStateWithModifyInFlight(cmeOrder, iceOrder);

    //Modify Ack
    d.onOrderReplaced(7, cmeOrder, iceOrder);
    VerifyBulkOfStateAfterModify(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStatePostModifyAck(cmeOrder, iceOrder);

    //Cancel
    d.quote(0.34, 0);
    VerifyBulkOfState(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateWithCancelInFlight(cmeOrder, iceOrder);

    //Full Fill
    d.onOrderExecuted(.34, 5, false, cmeOrder, iceOrder);
    d.verifyStateCancelRejectIncoming(cmeOrder, iceOrder);
}

TEST_F(StackerFixture, ICE_NoMoPartialFullModifyReject) {
    //Generic Init Code
    OrderManager<TestIME>* cme = _stacker;
    OrderManager<TestIME_2>* ice = _ime2->getBidStacker();
    cme->initialize(cmeContract, BID, config_60_0_0, _ime, true);
    ice->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    cme->_ime->_executor->setVenue(hf::VENUE::CME);
    ice->_ime->_executor->setVenue(hf::VENUE::ICE);
    Duplicator<std::decay_t<decltype(*this)>, std::decay_t<decltype(*cme)>, std::decay_t<decltype(*ice)>> d{this, cme, ice};
    d.quote(0.35, 4);
    const om::order_t& cmeOrder =  *cme->_topLevel->_orders[0];
    const OrderManager<TestIME>::Level* cme35 = cme->_topLevel;
    const OrderManager<TestIME>::Level* cme34 = cme->_topLevel->after;
    const om::order_t& iceOrder =  *ice->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* ice35 = ice->_topLevel;
    const OrderManager<TestIME_2>::Level* ice34 = ice->_topLevel->after;
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    //MO
    d.onOrderAccepted(4, cmeOrder, iceOrder);
    d.quote(0.34, 5);
    d.verifyStateWithModifyInFlight(cmeOrder, iceOrder);

    //Partial then full
    d.onOrderExecuted(ToRhonePrice(0.34), 1, true, cmeOrder, iceOrder);
    VerifyBulkOfStateDuringModify(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateWithModifyInFlight(cmeOrder, iceOrder);
    d.onOrderExecuted(ToRhonePrice(0.34), 3, false, cmeOrder, iceOrder);
    VerifyBulkOfStateDuringModify(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateModifyRejectIncoming(cmeOrder, iceOrder);
}


TEST_F(StackerFixture, ICE_NoMoPartialOverfillModifyReject) {
    //Generic Init Code
    OrderManager<TestIME>* cme = _stacker;
    OrderManager<TestIME_2>* ice = _ime2->getBidStacker();
    cme->initialize(cmeContract, BID, config_60_0_0, _ime, true);
    ice->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    cme->_ime->_executor->setVenue(hf::VENUE::CME);
    ice->_ime->_executor->setVenue(hf::VENUE::ICE);
    Duplicator<std::decay_t<decltype(*this)>, std::decay_t<decltype(*cme)>, std::decay_t<decltype(*ice)>> d{this, cme, ice};
    d.quote(0.35, 4);
    const om::order_t& cmeOrder =  *cme->_topLevel->_orders[0];
    const OrderManager<TestIME>::Level* cme35 = cme->_topLevel;
    const OrderManager<TestIME>::Level* cme34 = cme->_topLevel->after;
    const om::order_t& iceOrder =  *ice->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* ice35 = ice->_topLevel;
    const OrderManager<TestIME_2>::Level* ice34 = ice->_topLevel->after;
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    d.onOrderAccepted(4, cmeOrder, iceOrder);
    d.quote(0.34, 1);

    //Partial (overfill)
    d.onOrderExecuted(ToRhonePrice(0.34), 1, true, cmeOrder, iceOrder);
    VerifyBulkOfStateDuringModify(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateWithModifyInFlight(cmeOrder, iceOrder);

    //Full fill (overfill)
    d.onOrderExecuted(ToRhonePrice(0.34), 3, false, cmeOrder, iceOrder);
    VerifyBulkOfStateDuringModify(cme34, ice34, cmeOrder, iceOrder);
    d.verifyStateModifyRejectIncoming(cmeOrder, iceOrder);

}

TEST_F(StackerFixture, ICE_NoMo) {
    //Just verifies if we have NO->MO, with the NO in flight, the level is correct for the NOAck
    //Generic Init Code
    OrderManager<TestIME>* cme = _stacker;
    OrderManager<TestIME_2>* ice = _ime2->getBidStacker();
    cme->initialize(cmeContract, BID, config_60_0_0, _ime, true);
    ice->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    cme->_ime->_executor->setVenue(hf::VENUE::CME);
    ice->_ime->_executor->setVenue(hf::VENUE::ICE);
    Duplicator<std::decay_t<decltype(*this)>, std::decay_t<decltype(*cme)>, std::decay_t<decltype(*ice)>> d{this, cme, ice};
    d.quote(0.35, 4);
    const om::order_t& cmeOrder =  *cme->_topLevel->_orders[0];
    const OrderManager<TestIME>::Level* cme35 = cme->_topLevel;
    const OrderManager<TestIME>::Level* cme34 = cme->_topLevel->after;
    const om::order_t& iceOrder =  *ice->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* ice35 = ice->_topLevel;
    const OrderManager<TestIME_2>::Level* ice34 = ice->_topLevel->after;
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    d.quote(0.34, 1); //Allowed for ICE
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    ASSERT_EQ(ice->getLevelForNewAcknowledgement(iceOrder)->_price, ToRhonePrice(0.35));
}

TEST_F(StackerFixture, ICE_NoCo) {
    //Just verifies if we have NO->CO, with the NO in flight, the level is correct for the NOAck
    //Generic Init Code
    OrderManager<TestIME>* cme = _stacker;
    OrderManager<TestIME_2>* ice = _ime2->getBidStacker();
    cme->initialize(cmeContract, BID, config_60_0_0, _ime, true);
    ice->initialize(iceContract, BID, config_60_0_0, _ime2, true);
    cme->_ime->_executor->setVenue(hf::VENUE::CME);
    ice->_ime->_executor->setVenue(hf::VENUE::ICE);
    Duplicator<std::decay_t<decltype(*this)>, std::decay_t<decltype(*cme)>, std::decay_t<decltype(*ice)>> d{this, cme, ice};
    d.quote(0.35, 4);
    const om::order_t& cmeOrder =  *cme->_topLevel->_orders[0];
    const OrderManager<TestIME>::Level* cme35 = cme->_topLevel;
    const OrderManager<TestIME>::Level* cme34 = cme->_topLevel->after;
    const om::order_t& iceOrder =  *ice->_topLevel->_orders[0];
    const OrderManager<TestIME_2>::Level* ice35 = ice->_topLevel;
    const OrderManager<TestIME_2>::Level* ice34 = ice->_topLevel->after;
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    d.quote(0.35, 0); //Allowed for ICE
    ASSERT_TRUE(ice35->_orders[0]->action_flags.is_cancelling);
    d.verifyStateNOInFlight(cmeOrder, iceOrder);

    ASSERT_EQ(ice->getLevelForNewAcknowledgement(iceOrder)->_price, ToRhonePrice(0.35));
}

}