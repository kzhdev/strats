//
// Created by cmilutinovic on 10/28/19.
//

#ifndef RHONEDSTEST_IMPLIEDSTRUCTURES_H
#define RHONEDSTEST_IMPLIEDSTRUCTURES_H

#include <hf/hf.h>
#include <om/om.h>

namespace cme {
    namespace mdp {
        struct MDEntryType_t {
            char value;

            operator char() const { return value; }

            enum {
                Bid = '0',
                Offer = '1',
                Trade = '2',
                OpeningPrice = '4',
                SettlementPrice = '6',
                TradingSessionHighPrice = '7',
                TradingSessionLowPrice = '8',
                TradeVolume = 'B',
                OpenInterest = 'C',
                ImpliedBid = 'E',
                ImpliedOffer = 'F',
                EmptyBook = 'J',
                SessionHighBid = 'N',
                SessionLowOffer = 'O',
                FixingPrice = 'W',
                ElectronicVolume = 'e',
                ThresholdLimitsandPriceBandVariation = 'g'
            };
        };


        static const int32_t Int32NULL_VALUE = 2147483647;
        typedef int32_t Int32NULL;

        static const int8_t Int8NULL_VALUE = 127;
        typedef int8_t Int8NULL;

        static const uint8_t uInt8NULL_VALUE = 255;
        typedef uint8_t uInt8NULL;

        typedef int64_t PRICENULL;
        static constexpr PRICENULL NULL_PRICENULL = INT64_MAX;

        struct SnapshotEntry
        {
            PRICENULL MDEntryPx;
            Int32NULL MDEntrySize;
            Int32NULL NumberOfOrders;
            Int8NULL MDPriceLevel;
            uint16_t TradingReferenceDate;
            uInt8NULL OpenCloseSettlFlag;
            uint8_t SettlPriceType;
            MDEntryType_t MDEntryType;
        };
    }
}

//This is basically copied over from OrderCollections.h

struct IntegratedTwoLevelImpliedTrackerSnapshotHandler {

    template<typename T>
    void setAction(T&& t) {
        _addOrderFunc = std::forward<decltype(_addOrderFunc)>(t);
    }

    std::function<void(uint64_t, int64_t, uint32_t, cme::mdp::MDEntryType_t)> _addOrderFunc;
};

struct IntegratedTwoLevelImpliedTracker {

    static constexpr bool IntegratedImpliedOrders = true;
    static constexpr int NumLevels = 2;

    IntegratedTwoLevelImpliedTracker(const uint64_t(&list)[NumLevels]) {
        BOOST_ASSERT_MSG(list[0] != om::NULL_RHONE_ORDER_ID.value(), "Will cause null tests to fail");
        BOOST_ASSERT_MSG(list[1] != om::NULL_RHONE_ORDER_ID.value(), "Will cause null tests to fail");
        BOOST_ASSERT_MSG(list[0] != list[1], "non-unique Order IDs presented");

        for(int i = 0; i < NumLevels; ++i) {
            level[i] = list[i];
        }
    }

    //AddOrder   : (uint64_t orderId, int64_t price, uint32_t qty)
    //ModifyOrder: (uint64_t orderId, int64_t price, uint32_t qty)
    //DeleteOrder: (uint64_t orderId)
    template<typename Entry, typename AddOrderFunc, typename ModifyOrderFunc, typename DeleteOrderFunc>
    void doMDEntryIMPLIED(AddOrderFunc&& addOrder, ModifyOrderFunc&& modifyOrder, DeleteOrderFunc&& deleteOrder, const Entry& entry) {

        const auto add = [&](uint64_t orderID){ addOrder(orderID, entry.MDEntryPx, entry.MDEntrySize); };
        const auto modify = [&](uint64_t orderID) { modifyOrder(orderID, entry.MDEntryPx, entry.MDEntrySize); };
        const auto del = [&](uint64_t orderID) { deleteOrder(orderID); };

        //Action, # Levels, MDLevel
        const auto a01 = [&]() { add(level[0]); numLevel = 1; };
        const auto a11 = [&]() { swap(); add(level[0]); numLevel = 2; };
        const auto a12 = [&]() { add(level[1]); numLevel = 2; };
        const auto a21 = [&]() { swap(); modify(level[0]); };
        const auto a22 = [&]() { modify(level[1]); };

        const auto m11 = [&]() { modify(level[0]); };
        const auto m21 = [&]() { modify(level[0]); };
        const auto m22 = [&]() { modify(level[1]); };

        const auto d11 = [&]() { del(level[0]); numLevel = 0; };
        const auto d21 = [&]() { del(level[0]); swap(); numLevel = 1; };
        const auto d22 = [&]() { del(level[1]); numLevel = 1; };

        //These cases are errors, but we should converge to correctness
        const auto a02 = [&]() { add(level[1]); numLevel = 2; };
        const auto m01 = [&]() { modify(level[0]); numLevel = 1; };
        const auto m02 = [&]() { modify(level[1]); numLevel = 2; };
        const auto m12 = [&]() { modify(level[1]); numLevel = 2; };
        const auto d01 = [&]() { del(level[0]); };
        const auto d02 = [&]() { del(level[1]); };
        const auto d12 = [&]() { del(level[1]); };

        switch(jump_kernel(entry.MDUpdateAction, numLevel, entry.MDPriceLevel)) {
            case jump_kernel(0, 0, 1):
                a01(); break;
            case jump_kernel(0, 0, 2):
                a02(); break;
            case jump_kernel(0, 1, 1):
                a11(); break;
            case jump_kernel(0, 1, 2):
                a12(); break;
            case jump_kernel(0, 2, 1):
                a21(); break;
            case jump_kernel(0, 2, 2):
                a22(); break;
            case jump_kernel(1, 0, 1):
                m01(); break;
            case jump_kernel(1, 0, 2):
                m02(); break;
            case jump_kernel(1, 1, 1):
                m11(); break;
            case jump_kernel(1, 1, 2):
                m12(); break;
            case jump_kernel(1, 2, 1):
                m21(); break;
            case jump_kernel(1, 2, 2):
                m22(); break;
            case jump_kernel(2, 0, 1):
                d01(); break;
            case jump_kernel(2, 0, 2):
                d02(); break;
            case jump_kernel(2, 1, 1):
                d11(); break;
            case jump_kernel(2, 1, 2):
                d12(); break;
            case jump_kernel(2, 2, 1):
                d21(); break;
            case jump_kernel(2, 2, 2):
                d22(); break;
            default:
                BOOST_ASSERT(false);
        }
    }

    template<typename SnapshotEntry>
    void handleSnapshotEntry(const IntegratedTwoLevelImpliedTrackerSnapshotHandler& funcs, const SnapshotEntry& entry) {
        BOOST_ASSERT(funcs._addOrderFunc);
        BOOST_ASSERT(entry.MDEntryType == 'E' || entry.MDEntryType == 'F');
        if(BOOST_LIKELY((entry.MDEntryType == 'E') || (entry.MDEntryType == 'F'))) {
            funcs._addOrderFunc(level[entry.MDPriceLevel - 1], entry.MDEntryPx, entry.MDEntrySize, entry.MDEntryType);
            numLevel = std::max(numLevel, static_cast<uint8_t>(entry.MDPriceLevel));
        }
    }

    template<typename DeleteOrderFunc>
    void emptyBook(DeleteOrderFunc&& deleteOrder) {
        //It should be safe to delete an order that is not in the book
        deleteOrder(level[0]);
        deleteOrder(level[1]);
        numLevel = 0;
    }

    //Mainly for unit testing
    auto getNumLevel() {
        return numLevel;
    }

private:

    static constexpr uint8_t jump_kernel(int mdUpdateAction, int currentNumLevels, int priceLevel) {
        //mdUpdateAction \in {0,1,2}, currentNumLevels \in {0,1,2}, priceLevel \in {1,2}
        //It's safest to have mdUpdateAction at the end just in case a '5' or something slips through
        return (priceLevel-1) | (currentNumLevels << 2) | (mdUpdateAction << 4);
    }

    void swap() {
        std::swap(level[0], level[1]);
    }

    //We could do this with only numLevels, however keeping track of OrderIDs here allows less
    //reindexing in my cases
    uint64_t level[2];
    uint8_t numLevel = 0;
};
#endif //RHONEDSTEST_IMPLIEDSTRUCTURES_H
