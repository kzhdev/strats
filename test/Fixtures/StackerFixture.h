#ifndef STACKER_FIXTURE_H
#define STACKER_FIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "Stacker.h"

namespace Python {

struct Executor {
};

class StackerFixture : public ::testing::Test {
protected:
    StackerFixture() { }

    void SetUp() override {
        _stacker = new Stacker<Executor>();
    }

    void TearDown() override {
    }

private:
    Stacker<Executor>* _stacker;
};

}

#endif //STACKER_FIXTURE_H
