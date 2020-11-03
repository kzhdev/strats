#ifndef VIPER_MATH_STRUCTURES_H
#define VIPER_MATH_STRUCTURES_H

#include <cstdint>
#include <cmath>
#include "math/ap.h"
#include "math/linalg.h"
#include <sstream>
#include "boost/assert.hpp"

namespace Python {

template <typename T>
struct Vector {

    static_assert(std::is_trivially_copyable<T>::value, "non-copyable vector type");

    Vector(const Vector& in) {
        init(in._size);
        for (uint32_t index = 0; index < _size; ++index) {
            _data[index] = in.getValue(index);
        }
    }

    Vector() {
    }

    Vector divide(Vector& in) {
        Vector<T> result(*this);
        for (uint32_t index = 0; index < _size; ++index) {
            result.divide(index, in.getValue(index));
        }
        return result;
    }

    void init(uint32_t size) {
        _size = size;
        _data = new T[size];
        for (uint32_t i = 0; i < size; ++i) {
            _data[i] = 0;
        }
    }

    void initDefault(uint32_t size) {
        _size = size;
        _data = new T[size];
        for(uint32_t i = 0; i < size; ++i) {
            _data[i] = T{};
        }
    }

    T getValue(uint32_t index) const {
        BOOST_ASSERT(index < _size);
        return _data[index];
    }

    T& getRef(uint32_t index) const {
        BOOST_ASSERT(index < _size);
        return _data[index];
    }

    void add(uint32_t index, T value) {
        BOOST_ASSERT(index < _size);
        _data[index] += value;
    }

    void setValue(uint32_t index, T value) {
        BOOST_ASSERT(index < _size);
        _data[index] = value;
    }

    void divide(uint32_t index, T value) {
        BOOST_ASSERT(index < _size);
        _data[index] /= value;
    }

    uint32_t _size;
    T* _data;
};

struct Matrix {

    Matrix(const Matrix& in)
    :
        Matrix(in._numberOfRows, in._numberOfColumns)
    {
        copyMatrix(in);
    }

    Matrix() {}

    Matrix(uint32_t rows, uint32_t columns) {
        initialize(rows, columns);
    }

    void copyMatrix(const Matrix& in) {
        for (uint32_t row = 0; row < in._numberOfRows; ++row) {
            for (uint32_t column = 0; column < in._numberOfColumns; ++column) {
                data[row][column] = in.getValue(row, column);
            }
        }
    }

    void initialize(uint32_t rows, uint32_t columns) {
        data = new double*[rows];
        for (uint32_t row = 0; row < rows; ++row) {
            data[row] = new double[columns];
            for (uint32_t column = 0; column < columns; ++column) {
                data[row][column] = 0;
            }
        }
        _numberOfRows = rows;
        _numberOfColumns = columns;
    }

    void removeMemory() {
        /*
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            delete[] data[row];
        }
        delete[] data;
         */
    }

    void operator=(const Matrix& in) {
        if (data != nullptr) {
            removeMemory();
        }
        initialize(in._numberOfRows, in._numberOfColumns);
        copyMatrix(in);
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

        if (info != 1) {
            throw std::runtime_error("Inverse was not solved.");
        }

        Matrix result(_numberOfRows, _numberOfColumns);
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            for (uint32_t column = 0; column < _numberOfColumns; ++column) {
                result.setValue(row, column, matrix[row][column]);
            }
        }
        return result;
    }

    Matrix transpose() {
        Matrix result;
        result.initialize(_numberOfColumns, _numberOfRows);
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            for (uint32_t column = 0; column < _numberOfColumns; ++column) {
                result.setValue(column, row, data[row][column]);
            }
        }
        return result;
    }

    Matrix multiply(const Matrix& matrix) {
        Matrix result;
        result.initialize(_numberOfRows, matrix._numberOfColumns);
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            for (uint32_t col = 0; col < matrix._numberOfColumns; ++col) {
                double sum = 0;
                for (uint32_t i = 0; i < _numberOfColumns; ++i) {
                    double a = getValue(row, i);
                    double b = matrix.getValue(i, col);
                    const double product = a * b;
                    sum += product;
                }
                result.setValue(row, col, sum);
            }
        }
        return result;
    }

    void setValue(uint32_t row, uint32_t column, double value) {
        BOOST_ASSERT(row < _numberOfRows);
        BOOST_ASSERT(column < _numberOfColumns);
        data[row][column] = value;
    }

    double getValue (uint32_t row, uint32_t column) const {
        BOOST_ASSERT(row < _numberOfRows);
        BOOST_ASSERT(column < _numberOfColumns);
        return data[row][column];
    }

    double multiplyAbsColumn(uint32_t column, Vector<double>& vector) {
        double total = 0;
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            total += vector.getValue(row) * std::abs(data[row][column]);
        }
        return total;
    }

    void toLog() {
        for (uint32_t row = 0; row < _numberOfRows; ++ row) {
            std::stringstream stream;
            for (uint32_t col = 0; col < _numberOfColumns; ++ col) {
                stream << getValue(row, col) << ", ";
            }
            log(stream.str());
        }
        log("");
    }

    void log(const std::string& message);

    uint32_t _numberOfRows;
    uint32_t _numberOfColumns;

    double** data;
};

template <typename T>
struct SimpleMatrix {

    void initialize(uint32_t rows, uint32_t columns) {
        data = new T*[rows];
        for (uint32_t row = 0; row < rows; ++row) {
            data[row] = new T[columns];
        }
        _numberOfRows = rows;
        _numberOfColumns = columns;

        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t col = 0; col < rows; ++col) {
                data[row][col] = 0;
            }
        }
    }

    T getValue (uint32_t row, uint32_t column) const {
        BOOST_ASSERT(row < _numberOfRows);
        BOOST_ASSERT(column < _numberOfColumns);
        return data[row][column];
    }

    void setValue(uint32_t row, uint32_t column, T value) {
        BOOST_ASSERT(row < _numberOfRows);
        BOOST_ASSERT(column < _numberOfColumns);
        data[row][column] = value;
    }

    uint32_t _numberOfRows;
    uint32_t _numberOfColumns;

    T** data;
};


}

#endif //VIPER_MATH_STRUCTURES_H

