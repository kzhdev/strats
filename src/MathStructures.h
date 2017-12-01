#ifndef VIPER_MATH_STRUCTURES_H
#define VIPER_MATH_STRUCTURES_H

#include <cstdint>
#include <cmath>

namespace Python {

template <typename T>
struct Vector {
    Vector divide(Vector& in) {
        Vector<T> result(*this);
        for (uint32_t index = 0; index < _size; ++index) {
            result.divide(index, in.getValue(index));
        }
        return result;
    }

    T getValue(uint32_t index) {
        return data[index];
    }

    void add(uint32_t index, T value) {
        data[index] += value;
    }

    void setValue(uint32_t index, T value) {
        data[index] = value;
    }

    void divide(uint32_t index, T value) {
        data[index] /= value;
    }

    uint32_t _size;
    T* data;
};

struct Matrix {

    Matrix(const Matrix& in) {
        data = new double*[in._numberOfRows];
        for (uint32_t row = 0; row < in._numberOfRows; ++row) {
            data[row] = new double[in._numberOfColumns];
        }
        for (uint32_t row = 0; row < in._numberOfRows; ++row) {
            for (uint32_t column = 0; column < in._numberOfColumns; ++column) {
                data[row][column] = in.getValue(row, column);
            }
        }
        _numberOfRows = in._numberOfRows;
        _numberOfColumns = in._numberOfColumns;
    }

    Matrix() {
    }

    Matrix inverse() {
        return Matrix();
    }

    void setValue(uint32_t row, uint32_t column, double value) {
        data[row][column] = value;
    }

    double getValue (uint32_t row, uint32_t column) const {
        return data[row][column];
    }

    double multiplyAbsColumn(uint32_t column, Vector<double>& vector) {
        double total = 0;
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            total += vector.getValue(row) * std::abs(data[row][column]);
        }
        return total;
    }

    uint32_t _numberOfRows;
    uint32_t _numberOfColumns;

    double** data;
};


}

#endif //VIPER_MATH_STRUCTURES_H

