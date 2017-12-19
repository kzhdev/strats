#ifndef INTEGRAION_FIXTURE_H
#define INTEGRAION_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Stacker.h"

namespace Python {

class IntegrationFixture : public ::testing::Test {
protected:
    IntegrationFixture() { }

    void SetUp() override {
    }

    void TearDown() override {
    }


};

}

#endif //INTEGRAION_FIXTURE_H
