// Focused check of the one behavioural difference found:
// ALGLIB treats a matrix as invertible unless a pivot is *exactly* zero,
// while Eigen::FullPivLU::isInvertible() uses a relative (rank-revealing) threshold.
#include <Eigen/Dense>
#include <cmath>
#include <cstdio>
#include <vector>

#include "linalg.h"

static void caseReport(const char* label, const std::vector<std::vector<double>>& m) {
    const alglib::ae_int_t n = static_cast<alglib::ae_int_t>(m.size());
    alglib::real_2d_array a;
    a.setlength(n, n);
    for (alglib::ae_int_t r = 0; r < n; ++r)
        for (alglib::ae_int_t c = 0; c < n; ++c) a[r][c] = m[r][c];
    alglib::ae_int_t info;
    alglib::matinvreport rep;
    alglib::rmatrixinverse(a, n, info, rep);

    Eigen::MatrixXd e(n, n);
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) e(r, c) = m[r][c];

    Eigen::FullPivLU<Eigen::MatrixXd> defaultLu(e);
    Eigen::FullPivLU<Eigen::MatrixXd> zeroThresholdLu(e);
    zeroThresholdLu.setThreshold(0.0);

    double alglibMax = 0.0;
    for (alglib::ae_int_t r = 0; r < n; ++r)
        for (alglib::ae_int_t c = 0; c < n; ++c) alglibMax = std::max(alglibMax, std::fabs(a[r][c]));

    std::printf("\n%s\n", label);
    std::printf("  ALGLIB  : info=%lld  max|inv element| = %.6e\n",
                (long long)info, info == 1 ? alglibMax : -1.0);
    std::printf("  Eigen   : FullPivLU default   -> isInvertible=%d, rank=%lld (of %lld)\n",
                (int)defaultLu.isInvertible(), (long long)defaultLu.rank(), (long long)n);
    std::printf("  Eigen   : FullPivLU thr=0     -> isInvertible=%d, rank=%lld (of %lld)\n",
                (int)zeroThresholdLu.isInvertible(), (long long)zeroThresholdLu.rank(), (long long)n);
}

int main() {
    caseReport("3x3 exactly rank-2 [[1,2,3],[4,5,6],[7,8,9]]",
               {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
    caseReport("4x4 where row 4 = row 1 + row 2 (redundant spread)",
               {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 0}, {1, 1, 0, 2}});
    caseReport("6x6 pricing matrix, last vector = sum of first two",
               {{1, 0, 0, 0, 0, 0},
                {0, 1, 0, 0, 0, 0},
                {0, 0, 1, 0, 0, 0},
                {0, 0, 0, 1, 0, 0},
                {0, 0, 0, 0, 1, 0},
                {1, 1, 0, 0, 0, 0}});
    caseReport("6x6 healthy pricing matrix",
               {{1, 0.02, -0.02, 0, 0.02, 0},
                {0.02, 1, 0.02, -0.02, 0, 0.02},
                {-0.02, 0.02, 1, 0.02, -0.02, 0},
                {0, -0.02, 0.02, 1, 0.02, -0.02},
                {0.02, 0, -0.02, 0.02, 1, 0.02},
                {0, 0.02, 0, -0.02, 0.02, 1}});
    return 0;
}
