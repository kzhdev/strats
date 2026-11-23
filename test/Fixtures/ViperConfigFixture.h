#ifndef VIPER_CONFIG_FIXTURE_H
#define VIPER_CONFIG_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "ViperConfig.h"
#include "RelativeValue.h"

namespace Python {

class ViperConfigFixture : public ::testing::Test {

protected:
    ViperConfigFixture() {}

    void SetUp() override {
        _relativeValue = new RelativeValue();
    }

    void TearDown() override {
    }

    ViperConfig _config;
    RelativeValue* _relativeValue;
};

}

#endif //VIPER_CONFIG_FIXTURE_H
