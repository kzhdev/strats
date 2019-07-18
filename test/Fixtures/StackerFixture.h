#ifndef STACKER_FIXTURE_H
#define STACKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Stacker.h"

namespace Python {

struct Executor {

    void disableMarketMakerAfterStackerRejects(uint32_t a, const std::string& reason) {
    }

    void cancelOrder(uint32_t id, om::order_t* order) {
        order->is_cancelling = true;
        _cancels.push_back(order);
    }

    void clearCancels() {
        _cancels.clear();
    }

    void modifyOrder(uint32_t id, const om::order_t* order, hf::price_t price, hf::qty_t size) {
        om::order_t* mutableOrder = const_cast<om::order_t*>(order);
        mutableOrder->pr = price;
        mutableOrder->qty = size;
        mutableOrder->is_modifying = true;
        _modifys.push_back(mutableOrder);
    }

    om::order_t* newLimitOrder(uint32_t internalId, uint32_t side, hf::price_t price, hf::qty_t size, om::ORDER_TYPE orderType) {
        om::order_t *order = new om::order_t();
        order->sid = 1;
        order->is_acked = false;
        order->is_active = true;
        order->side = ToRhoneSide(side);
        order->pr = price;
        order->qty = size;
        order->oid = ++_orderId;
        order->ot = om::ORDER_TYPE::LMT;
        order->venue = _venue;
        order->leavesQty = size;
        order->executedQty = 0;
        _orders.insert(std::pair<om::order_id_t, om::order_t *>(order->oid, order));
        ++_orderCount;
        return order;
    }

    void resetOrderCount() {
        _orderCount = 0;
    }

    void onCriticalError(const std::string& message) {
    }

    void setVenue(hf::VENUE venue) {
        _venue = venue;
    }

    uint32_t _orderId = 0;
    std::unordered_map<om::order_id_t, om::order_t *> _orders;
    uint32_t _orderCount = 0;
    hf::VENUE _venue = hf::VENUE::ICE;
    std::vector<om::order_t*> _cancels;
    std::vector<om::order_t*> _modifys;
};

struct TestIME {
    Executor* _executor = new Executor();

    void setVenue(hf::VENUE venue) { _executor->setVenue(venue); }

    void notifyLevelFree(uint32_t side, hf::price_t price) {
        ++_notifyLevelFreeCount[price];
    }

    void notifyLevelUsed(uint32_t side, hf::price_t price) {
        ++_notifyLevelUsedCount[price];
    }

    void removeMarketMakerHedgeOrders() {
    }

    void handleStackerNewOrderReject(uint32_t side, hf::price_t price, const std::string& reason) {
    }

    template<typename... Args>
    std::vector<const om::order_t*>& getGTCs(Args&&...) {
        static std::vector<const om::order_t*> hack;
        return hack;
    }

    template<typename... Args>
    void disableMarketMakerAfterStackerRejects(Args&&...) {}

    std::unordered_map<hf::price_t, int> _notifyLevelFreeCount;
    std::unordered_map<hf::price_t, int> _notifyLevelUsedCount;
};

class StackerFixture : public ::testing::Test {
protected:

    struct TestIME_2 {
        Executor* _executor = new Executor();

        void setVenue(hf::VENUE venue) { _executor->setVenue(venue); }

        void handleStackerNewOrderReject(uint32_t side, hf::price_t price, const std::string& reason) {
        }

        void notifyLevelFree(uint32_t side, hf::price_t price) {
            ++_notifyLevelFreeCount[price];
            _stacker[GetOtherSide(side)].onLevelFree(price);
        }

        void notifyLevelUsed(uint32_t side, hf::price_t price) {
            ++_notifyLevelUsedCount[price];
            _stacker[GetOtherSide(side)].onLevelUsed(price);
        }

        Stacker<TestIME_2> _stacker[2];

        Stacker<TestIME_2>* getBidStacker() {
            return &_stacker[BID];
        }

        Stacker<TestIME_2>* getAskStacker() {
            return &_stacker[ASK];
        }

        Stacker<TestIME_2>::Level* getLevel(const Stacker<TestIME_2>* stacker, hf::price_t price) {
            auto* level = stacker->_topLevel;
            if(level->_price == price) { return level; }
            while(level->after != nullptr) {
                level = level->after;
                if(level->_price == price) {
                    return level;
                }
            }
            return nullptr;
        }

        void removeMarketMakerHedgeOrders() {
        }

        template<typename... Args>
        std::vector<const om::order_t*>& getGTCs(Args&&...) {
            static std::vector<const om::order_t*> hack;
            return hack;
        }

        template<typename... Args>
        void disableMarketMakerAfterStackerRejects(Args&&...) {}

        std::unordered_map<hf::price_t, int> _notifyLevelFreeCount;
        std::unordered_map<hf::price_t, int> _notifyLevelUsedCount;
    };

    typedef Stacker<TestIME> StackerT;

    StackerFixture() { }

    void SetUp() override {
        _stacker = new StackerT();
        _ime = new TestIME();
        _ime2 = new TestIME_2();
    }

    void TearDown() override {
    }

    void onOrderAccepted(hf::qty_t size, const om::order_t& order) {
        onOrderAccepted(_stacker, size, order);
    }

    template <typename Level>
    void assertState(Level* level, hf::price_t price, hf::qty_t target, hf::qty_t acked, hf::qty_t flight, hf::qty_t traded, int flightout = 0, int overfill = INT_MIN) {
        ASSERT_EQ(price, level->_price);
        ASSERT_EQ(target, level->_target);
        ASSERT_EQ(flight, level->_flight);
        ASSERT_EQ(acked, level->_acknowledged);
        ASSERT_EQ(traded, level->_traded);
        ASSERT_EQ(flightout, level->_flightout);
        if(overfill != INT_MIN) {
            ASSERT_EQ(overfill, level->_potentialOverfill);
        }

        auto candidateTarget = (level->_acknowledged + level->_flight) - level->_traded + level->_potentialOverfill;

        ASSERT_TRUE(level->_potentialOverfill >= 0);
        ASSERT_TRUE(candidateTarget >= 0);
        ASSERT_TRUE(level->_flight + level->_acknowledged + level->_potentialOverfill >= 0);
    }


    template <typename StackerType>
    void onOrderAccepted(StackerType* stacker, hf::qty_t size, const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        om::OMOrderAcceptedEvent orderAck;
        orderAck.acceptedQty = size;
        mutableOrder.is_acked = true;
        stacker->onOrderAccepted(orderAck, order);
    }

    void onOrderReplaced(hf::qty_t size, const om::order_t& order) {
        onOrderReplaced(_stacker, size, order);
    }

    template <typename StackerType>
    void onOrderReplaced(StackerType* stacker, hf::qty_t size, const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        om::OMOrderModifiedEvent modifyAck;
        modifyAck.acceptedQty = size;
        stacker->onOrderReplaced(modifyAck, order);
    }

    template <typename StackerType>
    void onOrderReplaced(StackerType* stacker, hf::price_t price, hf::qty_t size, const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.pr = price;
        onOrderReplaced(stacker, size, order);
    }

    void onOrderCanceled(const om::order_t& order) {
        onOrderCanceled(_stacker, order);
    }

    template <typename StackerType>
    void onOrderCanceled(StackerType* stacker, const om::order_t& order) {
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.is_active = false;
        om::OMOrderCanceledEvent event;
        stacker->onOrderCanceled(event, order);
    }

    void onOrderExecuted(const om::order_t& order, hf::price_t price, hf::qty_t size, bool active) {
        onOrderExecuted(_stacker, order, price, size, active);
    }

    template <typename StackerType>
    void onOrderExecuted(StackerType* stacker, const om::order_t& order, hf::price_t price, hf::qty_t size, bool active) {
        om::OMExecutionEvent event;
        event.filledPrice = price;
        event.filledQty = size;
        event.totalExecutedQty = order.executedQty + size;
        om::order_t& mutableOrder = const_cast<om::order_t&>(order);
        mutableOrder.is_active = active;
        mutableOrder.executedQty = mutableOrder.executedQty + size;
        stacker->onOrderExecution(event, order);
    }

    void onOrderRejected(const om::order_t& order, uint32_t reasonCodeExt) {
        onOrderRejected(_stacker, order, reasonCodeExt);
    }

    template <typename StackerType>
    void onOrderRejected(StackerType* stacker, const om::order_t& order, uint32_t reasonCodeExt) {
        om::OMOrderRejectedEvent event;
        event.reasonCodeExt = reasonCodeExt;
        stacker->onOrderRejected(event, order);
    }

    template <typename StackerType>
    void onOrderReplaceRejected(StackerType* stacker, const om::order_t& order, uint32_t reasonCodeExt) {
        om::OMOrderModifyRejectedEvent event;
        event.reasonCodeExt = reasonCodeExt;
        stacker->onOrderReplaceRejected(event, order);
    }

    template <typename StackerType>
    void onOrderCancelRejected(StackerType* stacker, const om::order_t& order, uint32_t reasonCodeExt) {
        om::OMOrderCancelRejectedEvent event;
        event.reasonCodeExt = reasonCodeExt;
        stacker->onOrderCancelRejected(event, order);
    }

    //#region Used in StackerModifyTests
    void setupCMEModifyChain(std::unordered_map<om::order_id_t, om::order_t*>& orders, Stacker<TestIME_2>::Level**& level);
    bool askSideThinksLevelIsUsed(Stacker<TestIME_2>::Level* level);
    void initAskStackerLevels(hf::price_t startPrice, hf::price_t endPrice);
    void performKnownGoodModifyAcks(std::unordered_map<om::order_id_t, om::order_t*>& orders);
    //#endregion

    StackerT* _stacker;
    TestIME* _ime;
    TestIME_2* _ime2;
};

}

#endif //STACKER_FIXTURE_H
