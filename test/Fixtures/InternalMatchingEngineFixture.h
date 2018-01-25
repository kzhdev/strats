#ifndef INTERNAL_MATCHING_ENGINE_FIXTURE_H
#define INTERNAL_MATCHING_ENGINE_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MarketMakers.h"
#include "TestStrategy.h"
#include "TestExecutor.h"

namespace Python {

class InternalMatchingEngineFixture : public ::testing::Test {

protected:

    InternalMatchingEngineFixture() { }

    void SetUp() override {
        _matchingEngine = new InternalMatchingEngine<TestExecutor>();
    }

    void TearDown() override {
    }

    InternalMatchingEngine<TestExecutor>* _matchingEngine;
};

}

#endif //INTERNAL_MATCHING_ENGINE_FIXTURE_H
