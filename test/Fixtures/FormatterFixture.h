//
// Created by cmilutinovic on 9/30/19.
//

#ifndef RHONEDSTEST_FORMATTERFIXTURE_H
#define RHONEDSTEST_FORMATTERFIXTURE_H


#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Common.h"

class FormatterFixture : public ::testing::Test {
};

TEST_F(FormatterFixture, valid) {
    auto formatter = CREATE_SAFE_FORMATTER("Test %1% %2%");
    ASSERT_EQ("Test 1 2", static_cast<std::string>(formatter % 1 % 2));
}

TEST_F(FormatterFixture, invalid_too_many_args) {
    auto formatter = CREATE_SAFE_FORMATTER("Test %1% %2%");
    ASSERT_EQ("Exception when creating error message.  formatString=Test %1% %2% what=boost::too_few_args: format-string referred to more arguments than were passed",
            static_cast<std::string>(formatter % "hi"));
}

TEST_F(FormatterFixture, zero_Args_valid) {
    auto formatter = CREATE_SAFE_FORMATTER("test string");
    ASSERT_EQ("test string", static_cast<std::string>(formatter));
}

//These probably should be failures but following boost::format conventions
TEST_F(FormatterFixture, invalid_not_enough_args) {
    auto formatter = CREATE_SAFE_FORMATTER("Test %1% %2%");
    ASSERT_EQ("Test hello 1", static_cast<std::string>(formatter % "hello" % 1 % 2));
}

TEST_F(FormatterFixture, zero_Args_invalid_maybe) {
    auto formatter = CREATE_SAFE_FORMATTER("test string");
    ASSERT_EQ("test string", static_cast<std::string>(formatter % "test"));
}

#endif //RHONEDSTEST_FORMATTERFIXTURE_H
