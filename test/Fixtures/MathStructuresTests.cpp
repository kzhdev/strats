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

    Matrix n;
    n.initialize(2, 4);

    n.setValue(0, 0, 7);
    n.setValue(0, 1, 8);
    n.setValue(0, 2, 9);
    n.setValue(0, 3, 10);

    n.setValue(1, 0, 11);
    n.setValue(1, 1, 12);
    n.setValue(1, 2, 13);
    n.setValue(1, 3, 14);

    Matrix multiplied = m.multiply(n);

    ASSERT_EQ(3, multiplied._numberOfRows);
    ASSERT_EQ(4, multiplied._numberOfColumns);

    ASSERT_EQ(29, multiplied.getValue(0, 0));
    ASSERT_EQ(32, multiplied.getValue(0, 1));
    ASSERT_EQ(35, multiplied.getValue(0, 2));
    ASSERT_EQ(38, multiplied.getValue(0, 3));

    ASSERT_EQ(65, multiplied.getValue(1, 0));
    ASSERT_EQ(72, multiplied.getValue(1, 1));
    ASSERT_EQ(79, multiplied.getValue(1, 2));
    ASSERT_EQ(86, multiplied.getValue(1, 3));

    ASSERT_EQ(101, multiplied.getValue(2, 0));
    ASSERT_EQ(112, multiplied.getValue(2, 1));
    ASSERT_EQ(123, multiplied.getValue(2, 2));
    ASSERT_EQ(134, multiplied.getValue(2, 3));
}

}