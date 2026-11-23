#ifndef VIPER_MATH_FIXTURE_H
#define VIPER_MATH_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace Python {

class MathStructuresFixture : public ::testing::Test {

protected:
    MathStructuresFixture() {}

    void SetUp() override {
    }

    void TearDown() override {
    }
};

}

#endif //VIPER_MATH_FIXTURE_H
