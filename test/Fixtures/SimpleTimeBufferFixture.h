//
// Created by cmilutinovic on 6/25/20.
//

#ifndef RHONEDSTEST_SIMPLETIMEBUFFERFIXTURE_H
#define RHONEDSTEST_SIMPLETIMEBUFFERFIXTURE_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <unordered_map>
#include "FillRateChecker.h"

class SimpleTimeBufferFixture : public ::testing::Test {
    void SetUp() override { _buffer = std::make_unique<Python::FillRateChecker<>::SimpleTimeBuffer>(); }

protected:
    std::unique_ptr<Python::FillRateChecker<>::SimpleTimeBuffer> _buffer;
    static constexpr size_t InternalBufferSize = decltype(_buffer)::element_type::InternalBufferSize;
};

//Note this test was written before SumTotal feature
TEST_F(SimpleTimeBufferFixture, tf) {
    //Simple Test
    uint64_t current_time = 100;
    const hf::qty_t fillQty = 3;
    for(int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(_buffer->push_back(current_time++, fillQty));
    }
    ASSERT_EQ(1000, _buffer->count());

    _buffer->pop_older_than(100 + 500 - 1);
    ASSERT_EQ(1000/2, _buffer->count());

    //Now wrap it
    for(unsigned i = 0; i < InternalBufferSize - 500 - 1; ++i) {
        ASSERT_TRUE(_buffer->push_back(current_time++, fillQty));
    }

    ASSERT_EQ((1000/2) + InternalBufferSize - 500 - 1, _buffer->count());

    //We should be full
    ASSERT_FALSE(_buffer->push_back(current_time, fillQty));

    //Let's pop everything but the last 50 elements
    _buffer->pop_older_than(current_time - 50);
    ASSERT_EQ(_buffer->count(), 49);

    //Now let's insert a couple entries, should be in a good state
    for(int i = 0; i < 17; ++i) {
        _buffer->push_back(current_time++, fillQty);
    }
    ASSERT_EQ(_buffer->count(), 49 + 17);
    _buffer->pop_older_than(current_time - 2); //-1 to undo the ++i, -1 because this tests <=, not <
    ASSERT_EQ(_buffer->count(), 1);
    _buffer->pop_older_than(1'000'000'000);
    ASSERT_EQ(_buffer->count(), 0);
}


#endif //RHONEDSTEST_SIMPLETIMEBUFFER_H
