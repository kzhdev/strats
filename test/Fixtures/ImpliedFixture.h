//
// Created by cmilutinovic on 10/28/19.
//

#ifndef RHONEDSTEST_IMPLIEDTESTS_H
#define RHONEDSTEST_IMPLIEDTESTS_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <unordered_map>
#include "ImpliedStructures.h"


// So really these tests should be in RhonePublic, however running RhonePublic unit tests is a bear.  We're going to
// just test it here, and worry about moving it to RhonePublic in 3.55
class ImpliedFixture : public ::testing::Test {

protected:

    ImpliedFixture() :
        f_add([this](uint64_t orderId, int64_t price, uint32_t qty) {
            ASSERT_EQ(0u, _levels.count(orderId));
            _levels[orderId] = {.price = price, .qty=qty}; }),
        f_modify([this](uint64_t orderId, int64_t price, uint32_t qty) {
            if (!_corruptedState.skipExistsCheckOnModify) { ASSERT_EQ(1u, _levels.count(orderId));};
            _levels[orderId] = {.price = price, .qty=qty}; }),
        f_del([this](uint64_t orderId) { _levels.erase(orderId); }) {}

    struct Level {
        hf::price_t price = hf::NULL_PRICE;
        hf::qty_t qty = 0;

        bool operator==(const Level& other) const {
            return other.price == price
                   && other.qty == qty;
        }
    };

    void SetUp() override {
        constexpr uint64_t init_args[] = { UINT64_MAX - 10000, UINT64_MAX - 10000 - 1 };
        _tracker = std::make_unique<IntegratedTwoLevelImpliedTracker>(init_args);
        _handler = std::make_unique<IntegratedTwoLevelImpliedTrackerSnapshotHandler>();


        std::function<void(uint64_t, int64_t, uint32_t, cme::mdp::MDEntryType_t)> _addOrderFunc;
        _handler->setAction([&](uint64_t orderId, int64_t price, uint32_t qty, cme::mdp::MDEntryType_t entry) {
            f_add(orderId, price, qty);
        });
        _level1 = std::make_unique<Level>();
        _level2 = std::make_unique<Level>();
        _levels.clear();

        _corruptedState.reset();
    }

    void validate() {
        unsigned count = (_level1->price != hf::NULL_PRICE) + (_level2->price != hf::NULL_PRICE);
        ASSERT_EQ(count, _levels.size());

        if(_level1->price != hf::NULL_PRICE) {
            int count = 0;
            for(const auto& level : _levels) {
                if(level.second == *_level1) {
                    ++count;
                }
            }
            ASSERT_EQ(count, 1);
        }

        if(_level2->price != hf::NULL_PRICE) {
            int count = 0;
            for(const auto& level : _levels) {
                if(level.second == *_level2) {
                    ++count;
                }
            }
            ASSERT_EQ(count, 1);
        }
    }

    void set(std::unique_ptr<Level>& level, hf::price_t price, hf::qty_t qty) {
        level = std::make_unique<Level>(Level{.price = price, .qty = qty});
    }

    void setPrice(std::unique_ptr<Level>& level, hf::price_t price) {
        level = std::make_unique<Level>(Level{.price = price, .qty = level->qty});
    }

    void setQty(std::unique_ptr<Level>& level, hf::qty_t qty) {
        level = std::make_unique<Level>(Level{.price = level->price, .qty = qty});
    }

    struct MDEntryWrapper {
        int MDPriceLevel;
        int64_t MDEntryPx;
        uint32_t MDEntrySize;
        int MDUpdateAction;
    };

    void add(int level, hf::price_t price, hf::qty_t size) {
        MDEntryWrapper wrapper{.MDPriceLevel = level, .MDEntryPx = price, .MDEntrySize = static_cast<uint32_t>(size), .MDUpdateAction = 0};
        _tracker->doMDEntryIMPLIED(f_add, f_modify, f_del, wrapper);
    }

    void mod(int level, hf::price_t price, hf::qty_t size) {
        MDEntryWrapper wrapper{.MDPriceLevel = level, .MDEntryPx = price, .MDEntrySize = static_cast<uint32_t>(size), .MDUpdateAction = 1};
        _tracker->doMDEntryIMPLIED(f_add, f_modify, f_del, wrapper);
    }

    void del(int level) {
        MDEntryWrapper wrapper{.MDPriceLevel = level, .MDEntryPx = 0, .MDEntrySize = 0, .MDUpdateAction = 2};
        _tracker->doMDEntryIMPLIED(f_add, f_modify, f_del, wrapper);
    }

    std::unique_ptr<IntegratedTwoLevelImpliedTracker> _tracker;
    std::unique_ptr<IntegratedTwoLevelImpliedTrackerSnapshotHandler> _handler;

    std::unique_ptr<Level> _level1;
    std::unique_ptr<Level> _level2;

    //Explicit overrides for the "bad state" tests
    struct {
        bool skipExistsCheckOnModify = false;

        void reset() { skipExistsCheckOnModify = false; }
    } _corruptedState;

    //AddOrder   : (uint64_t orderId, int64_t price, uint32_t qty)
    //ModifyOrder: (uint64_t orderId, int64_t price, uint32_t qty)
    //DeleteOrder: (uint64_t orderId)
    std::function<void(uint64_t, int64_t, uint32_t)> f_add;
    std::function<void(uint64_t, int64_t, uint32_t)> f_modify;
    std::function<void(uint64_t)> f_del;

private:
//
//    struct LevelHasher {
//        std::size_t operator()(const Level& level) const {
//            return std::hash<hf::price_t>{}(level.price) ^ std::hash<hf::qty_t>{}(level.qty);
//        }
//    };
//
//    static bool Comp(const Level& lhs, const Level& rhs) {
//        return (lhs.price == rhs.price) && (lhs.qty == rhs.qty);
//    }

protected:

    std::unordered_map<uint64_t, Level> _levels;
};


static cme::mdp::SnapshotEntry CreateSnapshotEntry(int level, hf::price_t price, hf::qty_t size, hf::SIDE side) {
    cme::mdp::SnapshotEntry retVal;
    retVal.MDPriceLevel = level;
    retVal.MDEntryPx = price;
    retVal.MDEntrySize = size;

    switch(side) {
        case hf::SIDE::BUY:
            retVal.MDEntryType.value = decltype(retVal.MDEntryType)::ImpliedBid;
            break;
        case hf::SIDE::SELL:
            retVal.MDEntryType.value = decltype(retVal.MDEntryType)::ImpliedOffer;
            break;
        default:
            throw std::invalid_argument(std::string{"Side:"} + std::to_string(static_cast<int>(side)));
    }

    return retVal;
}

TEST_F(ImpliedFixture, compile_test) {
    ASSERT_TRUE(true);
}


TEST_F(ImpliedFixture, simple_insert_modify_delete_wtih_explicit_valdate) {
    set(_level1, 10, 100);
    set(_level2, 11, 102);

    add(1, 10, 100);
    add(2, 11, 102);

    ASSERT_EQ(_levels.size(), 2u);
    ASSERT_NE(_levels.begin()->first, (++_levels.begin())->first);

    auto validate_index = [this](int index) {
        auto iter = (_levels.begin());
        for(int i = 0; i < index; ++i) {
            ++iter;
        }
        switch(iter->second.price) {
            case 10:
                ASSERT_EQ(iter->second.qty, 100);
                break;
            case 11:
                ASSERT_EQ(iter->second.qty, 102);
                break;
            default:
                ASSERT_TRUE(false);
        }
    };

    validate_index(0);
    validate_index(1);
    validate();
}

TEST_F(ImpliedFixture, snapshot_test_1l) {
    auto s1 = CreateSnapshotEntry(1, 10, 100, hf::SIDE::BUY);
    set(_level1, 10, 100);
    _tracker->handleSnapshotEntry(*_handler.get(), s1);
    validate();
}

TEST_F(ImpliedFixture, snapshot_test_2l) {
    auto s1 = CreateSnapshotEntry(1, 10, 100, hf::SIDE::SELL);
    auto s2 = CreateSnapshotEntry(2, 12, 102, hf::SIDE::SELL);
    set(_level1, 10, 100);
    set(_level2, 12, 102);
    _tracker->handleSnapshotEntry(*_handler.get(), s1);
    _tracker->handleSnapshotEntry(*_handler.get(), s2);
    validate();
}


//These are the cases we expect to actually see
TEST_F(ImpliedFixture, a01) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();
}

TEST_F(ImpliedFixture, a11) {
    add(1, 11, 102);
    set(_level1, 11, 102);
    validate();

    add(1, 10, 100);
    _level2 = std::make_unique<Level>(*_level1);
    set(_level1, 10, 100);
    validate();
}

TEST_F(ImpliedFixture, a12) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    add(2, 12, 104);
    set(_level2, 12, 104);
    validate();

    add(2, 11, 102);
    set(_level2, 11, 102);
    validate();
}

TEST_F(ImpliedFixture, a21) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    add(2, 12, 104);
    set(_level2, 12, 104);
    validate();

    add(1, 9, 98);
    _level2 = std::make_unique<Level>(*_level1);
    set(_level1, 9, 98);
    validate();
}

TEST_F(ImpliedFixture, a22) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    add(2, 12, 104);
    set(_level2, 12, 104);
    validate();

    add(2, 14, 108);
    set(_level2, 14, 108);
    validate();
}

TEST_F(ImpliedFixture, m11) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    mod(1, 11, 102);
    set(_level1, 11, 102);
    validate();
}

TEST_F(ImpliedFixture, m21) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    add(2, 12, 104);
    set(_level2, 12, 104);
    validate();

    mod(1, 11, 102);
    set(_level1, 11, 102);
    validate();
}

TEST_F(ImpliedFixture, m22) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    add(2, 12, 104);
    set(_level2, 12, 104);
    validate();

    mod(2, 122, 1044);
    set(_level2, 122, 1044);
    validate();
}

TEST_F(ImpliedFixture, d11) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    del(1);
    _level1 = std::make_unique<Level>();
    validate();
}

TEST_F(ImpliedFixture, d21) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    add(2, 12, 104);
    set(_level2, 12, 104);
    validate();

    del(1);
    _level1 = std::make_unique<Level>();
    validate();
}

TEST_F(ImpliedFixture, d22) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    add(2, 12, 104);
    set(_level2, 12, 104);
    validate();

    del(2);
    _level2 = std::make_unique<Level>();
    validate();
}

//These are cases that should never happen.  But we want to strive for "eventual consistency" so they should do something
//meaningful

TEST_F(ImpliedFixture, error_a02) {
    add(2, 12, 104);
    set(_level2, 12, 104);
    ASSERT_EQ(_tracker->getNumLevel(), 2);
    validate();

    //At this point we should have 2 levels, even if we only know of 1.  We'll interact with the book
    //And see what happens
    add(2, 120, 108);
    set(_level2, 120, 108);
    validate();

    add(1, 10, 100);
    set(_level1, 10, 100);
    _level2 = std::make_unique<Level>();
    validate();
}

TEST_F(ImpliedFixture, error_m01) {
    _corruptedState.skipExistsCheckOnModify = true;
    //This should be treated as an add
    mod(1, 10, 100);
    set(_level1, 10, 100);
    ASSERT_EQ(_tracker->getNumLevel(), 1);
    validate();
}

TEST_F(ImpliedFixture, error_m02) {
    _corruptedState.skipExistsCheckOnModify = true;
    //this should be treated as in the 'a02' case
    mod(2, 12, 104);
    set(_level2, 12, 104);

    //Copied and pasted from 'a02' test below
    ASSERT_EQ(_tracker->getNumLevel(), 2);
    validate();

    //At this point we should have 2 levels, even if we only know of 1.  We'll interact with the book
    //And see what happens
    add(2, 120, 108);
    set(_level2, 120, 108);
    validate();

    add(1, 10, 100);
    set(_level1, 10, 100);
    _level2 = std::make_unique<Level>();
    validate();
}

TEST_F(ImpliedFixture, error_m12) {
    _corruptedState.skipExistsCheckOnModify = true;
    //This should be treated as 'a12'
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    mod(2, 12, 104);
    set(_level2, 12, 104);
    ASSERT_EQ(_tracker->getNumLevel(), 2);
    validate();

    add(2, 11, 102);
    set(_level2, 11, 102);
    validate();
}

TEST_F(ImpliedFixture, error_d01) {
    //This should be a no-op
    del(1);
    ASSERT_EQ(_tracker->getNumLevel(), 0);
    validate();
}

TEST_F(ImpliedFixture, error_d02) {
    //This should be a no-op
    del(2);
    ASSERT_EQ(_tracker->getNumLevel(), 0);
    validate();
}

TEST_F(ImpliedFixture, error_d12) {
    add(1, 10, 100);
    set(_level1, 10, 100);
    validate();

    //This should be a no-op
    del(2);
    ASSERT_EQ(_tracker->getNumLevel(), 1);
    validate();
}

#endif //RHONEDSTEST_IMPLIEDTESTS_H
