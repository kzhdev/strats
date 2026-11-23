//
// Created by cmilutinovic on 7/19/20.
//

#ifndef RHONEDSTEST_IOPTRACKERFIXTURE_H
#define RHONEDSTEST_IOPTRACKERFIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

class IOPTrackerFixture : public ::testing::Test {

    void SetUp() override { }

    void TearDown() override { }
};

TEST_F(IOPTrackerFixture, compile_test) {
    ASSERT_EQ(true, false);
}


#endif //RHONEDSTEST_IOPTRACKERFIXTURE_H
