#ifndef PNL_CALCULATOR_FIXTURE_H
#define PNL_CALCULATOR_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <NegativeScalpChecker.h>

namespace Python {

class PnlCalculatorFixture : public ::testing::Test {
protected:

    PnlCalculatorFixture() {
        populateContracts();
    }

    void SetUp() override {
        _calc = new PnlCalculator();
        _negativeScalpCheckers = new NegativeScalpCheckers();
        _negativeScalpCheckers->initialize(_contracts, 100, ToRhonePrice(100), GetCyclesFromMicroSeconds(100, 3));
        _calc->init(_contracts, *_negativeScalpCheckers);
    }

    void TearDown() override {
    }

    void populateContracts() {
        ConfigContract contract1;
        contract1._name = "c1";
        contract1._tickSize = ToRhonePrice(0.01);
        contract1._tickValue = ToRhonePrice(10);
        _contracts.push_back(contract1);
        ConfigContract contract2;
        contract2._name = "c2";
        contract2._tickSize = ToRhonePrice(0.01);
        contract2._tickValue = ToRhonePrice(10);
        _contracts.push_back(contract2);
        _multiplier = contract1._tickValue / contract1._tickSize;
    }

    FillMessage createFill(uint32_t id, uint32_t side, hf::price_t price, hf::qty_t qty) {
        FillMessage fill;
        fill._contractId = id;
        fill._side = side;
        fill._price = price;
        fill._qty = qty;
        fill._strategy = StrategyId::RelativeValue;
        return fill;
    }

    hf::price_t _multiplier;

    PnlCalculator* _calc;
    NegativeScalpCheckers* _negativeScalpCheckers;
    std::vector<ConfigContract> _contracts;
};

}

#endif //PNL_CALCULATOR_FIXTURE_H