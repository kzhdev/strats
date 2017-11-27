#ifndef VIPER_MATH_STRUCTURES_H
#define VIPER_MATH_STRUCTURES_H

#include <cstdint>

namespace Python {

template <typename T>
struct Vector {
    Vector divide(Vector& vector) {
        return Vector();
    }

    T getValue(uint32_t index) {
        return T();
    }

    void add(uint32_t index, T value) {
    }

    void setValue(uint32_t index, T value) {
    }

    uint32_t _size;
};

struct Matrix {
    Matrix inverse() {
        return Matrix();
    }

    void setValue(uint32_t row, uint32_t column, double value) {
    }

    double getValue (uint32_t row, uint32_t column) {
        return 0;
    }

    double multiplyAbsColumn(uint32_t index, Vector<double>& vector) {
        // take abs value of the column value
        return 0;
    }

    uint32_t _numberOfRows;
    uint32_t _numberOfColumns;
};


}

#endif //VIPER_MATH_STRUCTURES_H

