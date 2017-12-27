#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <GUIThings.h>
#include "ViperConfigFixture.h"
#include "TestSymbolsResource.h"

using ::testing::_;

namespace Python {

TEST_F(ViperConfigFixture, ViperConfigTests) {
    std::string fileName("/home/pkusch/exito/repos/viper/test/Fixtures/unit_test_config.csv");
    TestSymbolsResource symbols;

    hf::price_t tickSize = ToRhonePrice(0.1);

    std::vector<hf::symbol_leg_info_t> legs;

    symbols.addContract("BRN FMG0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMH0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMJ0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMK0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMM0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMN0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMQ0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMU0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMV0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMX0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMZ0018!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMF0019!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMG0019!", hf::VENUE::ICE, tickSize, legs);
    symbols.addContract("BRN FMH0019!", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMG0018!", "BRN FMH0018!");
    symbols.addContract("BRN FMG0018-BRN FMH0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMH0018!", "BRN FMJ0018!");
    symbols.addContract("BRN FMH0018-BRN FMJ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMJ0018!", "BRN FMK0018!");
    symbols.addContract("BRN FMJ0018-BRN FMK0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMK0018!", "BRN FMM0018!");
    symbols.addContract("BRN FMK0018-BRN FMM0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMM0018!", "BRN FMN0018!");
    symbols.addContract("BRN FMM0018-BRN FMN0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMN0018!", "BRN FMQ0018!");
    symbols.addContract("BRN FMN0018-BRN FMQ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMQ0018!", "BRN FMU0018!");
    symbols.addContract("BRN FMQ0018-BRN FMU0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMU0018!", "BRN FMV0018!");
    symbols.addContract("BRN FMU0018-BRN FMV0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMV0018!", "BRN FMX0018!");
    symbols.addContract("BRN FMV0018-BRN FMX0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMX0018!", "BRN FMZ0018!");
    symbols.addContract("BRN FMX0018-BRN FMZ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMZ0018!", "BRN FMF0019!");
    symbols.addContract("BRN FMZ0018-BRN FMF0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMF0019!", "BRN FMG0019!");
    symbols.addContract("BRN FMF0019-BRN FMG0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMG0019!", "BRN FMH0019!");
    symbols.addContract("BRN FMG0019-BRN FMH0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMG0018!", "BRN FMJ0018!");
    symbols.addContract("BRN FMG0018-BRN FMJ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMG0018!", "BRN FMJ0018!");
    symbols.addContract("BRN FMG0018-BRN FMJ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMH0018!", "BRN FMK0018!");
    symbols.addContract("BRN FMH0018-BRN FMK0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMJ0018!", "BRN FMM0018!");
    symbols.addContract("BRN FMJ0018-BRN FMM0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMK0018!", "BRN FMN0018!");
    symbols.addContract("BRN FMK0018-BRN FMN0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMM0018!", "BRN FMQ0018!");
    symbols.addContract("BRN FMM0018-BRN FMQ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMN0018!", "BRN FMU0018!");
    symbols.addContract("BRN FMN0018-BRN FMU0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMQ0018!", "BRN FMV0018!");
    symbols.addContract("BRN FMQ0018-BRN FMV0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMU0018!", "BRN FMX0018!");
    symbols.addContract("BRN FMU0018-BRN FMX0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMV0018!", "BRN FMZ0018!");
    symbols.addContract("BRN FMV0018-BRN FMZ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMX0018!", "BRN FMF0019!");
    symbols.addContract("BRN FMX0018-BRN FMF0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMZ0018!", "BRN FMG0019!");
    symbols.addContract("BRN FMZ0018-BRN FMG0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMF0019!", "BRN FMH0019!");
    symbols.addContract("BRN FMF0019-BRN FMH0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMG0018!", "BRN FMK0018!");
    symbols.addContract("BRN FMG0018-BRN FMK0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMH0018!", "BRN FMM0018!");
    symbols.addContract("BRN FMH0018-BRN FMM0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMJ0018!", "BRN FMN0018!");
    symbols.addContract("BRN FMJ0018-BRN FMN0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMK0018!", "BRN FMQ0018!");
    symbols.addContract("BRN FMK0018-BRN FMQ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMM0018!", "BRN FMU0018!");
    symbols.addContract("BRN FMM0018-BRN FMU0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMN0018!", "BRN FMV0018!");
    symbols.addContract("BRN FMN0018-BRN FMV0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMQ0018!", "BRN FMX0018!");
    symbols.addContract("BRN FMQ0018-BRN FMX0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMU0018!", "BRN FMZ0018!");
    symbols.addContract("BRN FMU0018-BRN FMZ0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMV0018!", "BRN FMF0019!");
    symbols.addContract("BRN FMV0018-BRN FMF0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMX0018!", "BRN FMG0019!");
    symbols.addContract("BRN FMX0018-BRN FMG0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMZ0018!", "BRN FMH0019!");
    symbols.addContract("BRN FMZ0018-BRN FMH0019", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMG0018!", "BRN FMM0018!");
    symbols.addContract("BRN FMG0018-BRN FMM0018", hf::VENUE::ICE, tickSize, legs);
    symbols.populateLegs(legs, "BRN FMM0018!", "BRN FMZ0018!");
    symbols.addContract("BRN FMM0018-BRN FMZ0018", hf::VENUE::ICE, tickSize, legs);

    bool result = _config.initialize(fileName, symbols);

    ASSERT_EQ(true, result);

    GUIThings things;
    things.init(&_config, nullptr);

    _relativeValue->initializeModel(_config);
    Matrix& risk =  _relativeValue->_riskMatrix;
    Matrix& pricing =  _relativeValue->_pricingMatrix;

    proto::GUIInitialization message;
    things.populateGUIInitialization(message, risk, pricing);

}


}