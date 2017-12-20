#ifndef VIPER_CONFIG_FIXTURE_H
#define VIPER_CONFIG_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "ViperConfig.h"

namespace Python {

class ViperConfigFixture : public ::testing::Test {

protected:
    ViperConfigFixture() {}

    void SetUp() override {
    }

    void TearDown() override {
    }

};

}

#endif //VIPER_CONFIG_FIXTURE_H
