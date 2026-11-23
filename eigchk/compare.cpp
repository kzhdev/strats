// Compares ALGLIB's alglib::rmatrixinverse with the proposed Eigen (FullPivLU) replacement
// on matrices of the shapes and conditioning Viper actually uses.
#include <Eigen/Dense>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "linalg.h"

static Eigen::MatrixXd toEigen(const std::vector<std::vector<double>>& m) {
    Eigen::MatrixXd a(m.size(), m.size());
    for (size_t r = 0; r < m.size(); ++r)
        for (size_t c = 0; c < m.size(); ++c) a(r, c) = m[r][c];
    return a;
}

static bool alglibInverse(const std::vector<std::vector<double>>& m, std::vector<std::vector<double>>& out) {
    const alglib::ae_int_t n = static_cast<alglib::ae_int_t>(m.size());
    alglib::real_2d_array a;
    a.setlength(n, n);
    for (alglib::ae_int_t r = 0; r < n; ++r)
        for (alglib::ae_int_t c = 0; c < n; ++c) a[r][c] = m[r][c];
    alglib::ae_int_t info;
    alglib::matinvreport rep;
    alglib::rmatrixinverse(a, n, info, rep);
    if (info != 1) return false;
    out.assign(n, std::vector<double>(n, 0.0));
    for (alglib::ae_int_t r = 0; r < n; ++r)
        for (alglib::ae_int_t c = 0; c < n; ++c) out[r][c] = a[r][c];
    return true;
}

static void report(const char* label, const std::vector<std::vector<double>>& m) {
    std::vector<std::vector<double>> alglibInv;
    const bool alglibOk = alglibInverse(m, alglibInv);

    const Eigen::MatrixXd a = toEigen(m);
    Eigen::FullPivLU<Eigen::MatrixXd> lu(a);
    const bool eigenOk = lu.isInvertible();

    if (alglibOk != eigenOk) {
        std::printf("%-42s MISMATCH: alglib_ok=%d eigen_ok=%d\n", label, (int)alglibOk, (int)eigenOk);
        return;
    }
    if (!alglibOk) {
        std::printf("%-42s both report singular\n", label);
        return;
    }

    const Eigen::MatrixXd eigenInv = lu.inverse();
    double maxAbsDiff = 0.0, maxRelDiff = 0.0, maxMag = 0.0;
    for (size_t r = 0; r < m.size(); ++r) {
        for (size_t c = 0; c < m.size(); ++c) {
            const double d = std::fabs(alglibInv[r][c] - eigenInv(r, c));
            maxAbsDiff = std::max(maxAbsDiff, d);
            maxMag = std::max(maxMag, std::fabs(alglibInv[r][c]));
            if (std::fabs(alglibInv[r][c]) > 1e-300) maxRelDiff = std::max(maxRelDiff, d / std::fabs(alglibInv[r][c]));
        }
    }
    std::printf("%-42s maxAbsDiff=%.3e  maxRelDiff=%.3e  (max|inv|=%.3e)\n", label, maxAbsDiff, maxRelDiff, maxMag);
}

int main() {
    // 1. A realistic relative-value pricing matrix: identity + small spread couplings.
    {
        const int n = 6;
        std::vector<std::vector<double>> m(n, std::vector<double>(n, 0.0));
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) m[r][c] = (r == c) ? 1.0 : 0.02 * ((r + 2 * c) % 3 - 1);
        report("6x6 pricing matrix (identity + coupling)", m);
    }

    // 2. Random matrices of the sizes used (2..10), well conditioned.
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    for (int n : {2, 3, 4, 6, 8, 10}) {
        std::vector<std::vector<double>> m(n, std::vector<double>(n, 0.0));
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) m[r][c] = dist(rng) + (r == c ? 3.0 : 0.0);
        char label[64];
        std::snprintf(label, sizeof label, "random %dx%d (diagonally dominant)", n, n);
        report(label, m);
    }

    // 3. Deliberately ill-conditioned (near-singular) matrices.
    for (double eps : {1e-6, 1e-10, 1e-13}) {
        const int n = 6;
        std::vector<std::vector<double>> m(n, std::vector<double>(n, 0.0));
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) m[r][c] = (r == c) ? (1.0 + eps * r) : eps * 0.5;
        char label[64];
        std::snprintf(label, sizeof label, "6x6 near-singular (eps=%g)", eps);
        report(label, m);
    }

    // 4. Exactly singular matrices.
    {
        std::vector<std::vector<double>> m = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
        report("3x3 rank-2 [[1,2,3],[4,5,6],[7,8,9]]", m);
        std::vector<std::vector<double>> z = {{0.0, 0.0}, {0.0, 0.0}};
        report("2x2 all-zero", z);
        std::vector<std::vector<double>> dup = {{1, 2, 2}, {3, 4, 4}, {5, 6, 6}};
        report("3x3 duplicated last two columns", dup);
    }
    return 0;
}
