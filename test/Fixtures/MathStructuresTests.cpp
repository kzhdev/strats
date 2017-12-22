#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MathStructures.h"
#include "MathStructuresFicture.h"

using ::testing::_;

namespace Python{

TEST_F(MathStructuresFixture, MathTests) {
    Matrix m;
    m.initialize(3, 2);

    m.setValue(0, 0, 1);
    m.setValue(0, 1, 2);

    m.setValue(1, 0, 3);
    m.setValue(1, 1, 4);

    m.setValue(2, 0, 5);
    m.setValue(2, 1, 6);

    Matrix transposed = m.transpose();

    ASSERT_EQ(2, transposed._numberOfRows);
    ASSERT_EQ(3, transposed._numberOfColumns);

    ASSERT_EQ(1, transposed.getValue(0, 0));
    ASSERT_EQ(3, transposed.getValue(0, 1));
    ASSERT_EQ(5, transposed.getValue(0, 2));

    ASSERT_EQ(2, transposed.getValue(1, 0));
    ASSERT_EQ(4, transposed.getValue(1, 1));
    ASSERT_EQ(6, transposed.getValue(1, 2));

}

}