// Standalone validation of the proposed Eigen replacement for
// alglib::rmatrixinverse used in src/MathStructures.h (Python::Matrix::inverse()).
#include <Eigen/Dense>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

// ---------------------------------------------------------------- proposed impl
struct Matrix {
    uint32_t _numberOfRows = 0;
    uint32_t _numberOfColumns = 0;
    double** data = nullptr;

    Matrix() {}
    Matrix(uint32_t rows, uint32_t columns) { initialize(rows, columns); }

    void initialize(uint32_t rows, uint32_t columns) {
        data = new double*[rows];
        for (uint32_t row = 0; row < rows; ++row) {
            data[row] = new double[columns];
            for (uint32_t column = 0; column < columns; ++column) data[row][column] = 0;
        }
        _numberOfRows = rows;
        _numberOfColumns = columns;
    }
    void setValue(uint32_t row, uint32_t column, double value) { data[row][column] = value; }
    double getValue(uint32_t row, uint32_t column) const { return data[row][column]; }

    // ==== the replacement under test ====
    Matrix inverse() {
        if (_numberOfRows != _numberOfColumns) {
            throw std::runtime_error("Matrix is not square");
        }
        Eigen::MatrixXd a(_numberOfRows, _numberOfColumns);
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            for (uint32_t column = 0; column < _numberOfColumns; ++column) {
                a(row, column) = data[row][column];
            }
        }
        Eigen::FullPivLU<Eigen::MatrixXd> lu(a);
        if (!lu.isInvertible()) {
            throw std::runtime_error("Inverse was not solved.");
        }
        const Eigen::MatrixXd inverse = lu.inverse();

        Matrix result(_numberOfRows, _numberOfColumns);
        for (uint32_t row = 0; row < _numberOfRows; ++row) {
            for (uint32_t column = 0; column < _numberOfColumns; ++column) {
                result.setValue(row, column, inverse(row, column));
            }
        }
        return result;
    }
};

// ---------------------------------------------------------------- helpers
static double maxResidual(const Matrix& a, const Matrix& inv) {
    double worst = 0.0;
    for (uint32_t i = 0; i < a._numberOfRows; ++i) {
        for (uint32_t j = 0; j < a._numberOfColumns; ++j) {
            double sum = 0.0;
            for (uint32_t k = 0; k < a._numberOfColumns; ++k) sum += a.getValue(i, k) * inv.getValue(k, j);
            worst = std::max(worst, std::fabs(sum - (i == j ? 1.0 : 0.0)));
        }
    }
    return worst;
}

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    };

    // 1. A 3x3 with an exactly verifiable inverse.
    //    A = [[4,7,2],[3,6,1],[2,5,3]], det = 9,
    //    inv = adj(A)/9 = [[13,-11,-5],[-7,8,2],[3,-6,3]] / 9
    {
        Matrix m(3, 3);
        const double vals[3][3] = {{4, 7, 2}, {3, 6, 1}, {2, 5, 3}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) m.setValue(r, c, vals[r][c]);
        Matrix inv = m.inverse();
        std::printf("3x3 inverse returned: [%g %g %g; %g %g %g; %g %g %g]\n",
                    inv.getValue(0, 0), inv.getValue(0, 1), inv.getValue(0, 2),
                    inv.getValue(1, 0), inv.getValue(1, 1), inv.getValue(1, 2),
                    inv.getValue(2, 0), inv.getValue(2, 1), inv.getValue(2, 2));
        check(std::fabs(inv.getValue(0, 0) - 13.0 / 9) < 1e-12, "3x3 inverse: element (0,0) == 13/9");
        check(std::fabs(inv.getValue(0, 1) - (-11.0 / 9)) < 1e-12, "3x3 inverse: element (0,1) == -11/9");
        check(std::fabs(inv.getValue(2, 2) - 3.0 / 9) < 1e-12, "3x3 inverse: element (2,2) == 1/3");
        check(std::fabs(inv.getValue(1, 1) - 8.0 / 9) < 1e-12, "3x3 inverse: element (1,1) == 8/9");
        check(maxResidual(m, inv) < 1e-14, "3x3 inverse: max |A*inv - I| < 1e-14");
    }

    // 2. 6x6 well-conditioned matrix shaped like Viper's pricing matrix (identity plus
    //    small coupling), the shape used by ArbieSpreader::init / RelativeValue::initializeModel.
    {
        Matrix m(6, 6);
        for (int r = 0; r < 6; ++r)
            for (int c = 0; c < 6; ++c) m.setValue(r, c, (r == c) ? 1.0 : 0.02 * ((r + 2 * c) % 3 - 1));
        Matrix inv = m.inverse();
        check(maxResidual(m, inv) < 1e-13, "6x6 pricing-matrix inverse: max |A*inv - I| < 1e-13");
    }

    // 3. Non-square must throw, same as today.
    {
        Matrix m(3, 2);
        bool threw = false;
        try { m.inverse(); } catch (const std::runtime_error& e) { threw = std::string(e.what()) == "Matrix is not square"; }
        check(threw, "non-square matrix throws 'Matrix is not square'");
    }

    // 4. Singular matrix must throw, matching ALGLIB's info != 1 path.
    {
        Matrix m(3, 3);
        const double vals[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) m.setValue(r, c, vals[r][c]);
        bool threw = false;
        try { m.inverse(); } catch (const std::runtime_error& e) { threw = std::string(e.what()) == "Inverse was not solved."; }
        check(threw, "singular matrix throws 'Inverse was not solved.'");
    }

    // 5. 1x1 and 2x2 edge cases.
    {
        Matrix m(1, 1);
        m.setValue(0, 0, -4.0);
        Matrix inv = m.inverse();
        check(std::fabs(inv.getValue(0, 0) - (-0.25)) < 1e-15, "1x1 inverse");
        Matrix z(1, 1);
        z.setValue(0, 0, 0.0);
        bool threw = false;
        try { z.inverse(); } catch (const std::runtime_error&) { threw = true; }
        check(threw, "1x1 zero matrix throws");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
    return failures;
}
