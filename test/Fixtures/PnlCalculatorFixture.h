#ifndef PNL_CALCULATOR_FIXTURE_H
#define PNL_CALCULATOR_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Stacker.h"

namespace Python {

class PnlCalculatorFixture : public ::testing::Test {
protected:

    PnlCalculatorFixture() {
        populateContracts();
    }

    void SetUp() override {
        _calc = new PnlCalculator();
        _calc->init(_contracts);
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
    }

    FillMessage createFill(uint32_t id, uint32_t side, hf::price_t price, hf::qty_t qty) {
        FillMessage fill;
        fill._contractId = id;
        fill._side = side;
        fill._price = price;
        fill._qty = qty;
        return fill;
    }

    PnlCalculator* _calc;
    std::vector<ConfigContract> _contracts;
};

}

#endif //PNL_CALCULATOR_FIXTURE_H