#ifndef VIPER_MATH_STRUCTURES_H
#define VIPER_MATH_STRUCTURES_H

#include <cstdint>
#include <cmath>
#include "math/ap.h"
#include "math/linalg.h"

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

    Matrix(const Matrix& in)
    :
        Matrix(in._numberOfRows, in._numberOfColumns)
    {
        for (uint32_t row = 0; row < in._numberOfRows; ++row) {
            for (uint32_t column = 0; column < in._numberOfColumns; ++column) {
                data[row][column] = in.getValue(row, column);
            }
        }
    }

    Matrix() {}

    Matrix(uint32_t rows, uint32_t columns) {
        data = new double*[rows];
        for (uint32_t row = 0; row < rows; ++row) {
            data[row] = new double[columns];
        }
        _numberOfRows = rows;
        _numberOfColumns = columns;
    }

    Matrix inverse() {
        if (_numberOfRows != _numberOfColumns) {
            throw std::runtime_error("Matrix is not square");
        }
        alglib::real_2d_array matrix;
        matrix.setlength(_numberOfRows, _numberOfColumns);
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            for (uint32_t column = 0; column < _numberOfColumns; ++column) {
                matrix[row][column] = data[row][column];
            }
        }
        alglib::ae_int_t info;
        alglib::matinvreport rep;
        alglib::rmatrixinverse(matrix, _numberOfRows, info, rep);

        Matrix result(_numberOfRows, _numberOfColumns);
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            for (uint32_t column = 0; column < _numberOfColumns; ++column) {
                result.setValue(row, column, matrix[row][column]);
            }
        }
        return result;
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

