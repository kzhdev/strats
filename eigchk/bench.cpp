// Runtime benchmark: alglib::rmatrixinverse (today's code path) vs the proposed
// Eigen implementations, for the small square matrices Viper actually inverts.
//
// Everything is compiled with the project's production flag set:
//   -O3 -march=native -mno-avx -mno-avx2
//
// Each variant does the same work the wrapper does today:
//   double** -> contiguous storage -> decompose -> inverse -> double**
#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "linalg.h"

static volatile double g_sink = 0.0;

using Clock = std::chrono::steady_clock;

template <typename Fn>
static double medianNsPerOp(Fn&& fn, int iters, int repeats = 7) {
    std::vector<double> samples;
    samples.reserve(repeats);
    for (int r = 0; r < repeats; ++r) {
        const auto start = Clock::now();
        for (int i = 0; i < iters; ++i) fn(i);
        const auto stop = Clock::now();
        const double ns = std::chrono::duration<double, std::nano>(stop - start).count();
        samples.push_back(ns / iters);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// ---------------------------------------------------------------- variants
struct MatrixLike {  // same shape/storage as Python::Matrix
    uint32_t rows = 0, cols = 0;
    std::vector<double*> data;
    std::vector<std::vector<double>> storage;

    void initialize(uint32_t r, uint32_t c) {
        rows = r; cols = c;
        storage.assign(r, std::vector<double>(c, 0.0));
        data.resize(r);
        for (uint32_t i = 0; i < r; ++i) data[i] = storage[i].data();
    }
    double getValue(uint32_t r, uint32_t c) const { return data[r][c]; }
    void setValue(uint32_t r, uint32_t c, double v) { data[r][c] = v; }
};

static double alglibInverse(const MatrixLike& m, MatrixLike& out) {
    alglib::real_2d_array a;
    a.setlength(static_cast<alglib::ae_int_t>(m.rows), static_cast<alglib::ae_int_t>(m.cols));
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) a[r][c] = m.getValue(r, c);
    alglib::ae_int_t info;
    alglib::matinvreport rep;
    alglib::rmatrixinverse(a, static_cast<alglib::ae_int_t>(m.rows), info, rep);
    if (info != 1) return -1.0;
    out.initialize(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) out.setValue(r, c, a[r][c]);
    return 0.0;
}

static double eigenFullPiv(const MatrixLike& m, MatrixLike& out) {
    Eigen::MatrixXd a(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) a(r, c) = m.getValue(r, c);
    Eigen::FullPivLU<Eigen::MatrixXd> lu(a);
    if (!lu.isInvertible()) return -1.0;
    const Eigen::MatrixXd inv = lu.inverse();
    out.initialize(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) out.setValue(r, c, inv(r, c));
    return 0.0;
}

static double eigenPartialPiv(const MatrixLike& m, MatrixLike& out) {
    Eigen::MatrixXd a(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) a(r, c) = m.getValue(r, c);
    const Eigen::MatrixXd inv = a.partialPivLu().inverse();
    out.initialize(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) out.setValue(r, c, inv(r, c));
    return 0.0;
}

static double eigenMatrixInverse(const MatrixLike& m, MatrixLike& out) {  // MatrixBase::inverse()
    Eigen::MatrixXd a(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) a(r, c) = m.getValue(r, c);
    const Eigen::MatrixXd inv = a.inverse();
    out.initialize(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) out.setValue(r, c, inv(r, c));
    return 0.0;
}

// Copy-only baseline: measures the double** <-> contiguous conversion both paths pay.
static double copyBaseline(const MatrixLike& m, MatrixLike& out) {
    Eigen::MatrixXd a(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) a(r, c) = m.getValue(r, c);
    out.initialize(m.rows, m.cols);
    for (uint32_t r = 0; r < m.rows; ++r)
        for (uint32_t c = 0; c < m.cols; ++c) out.setValue(r, c, a(r, c));
    return 0.0;
}


int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937 rng(20261105);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::printf("ns per inverse call (median of 7 runs) - includes the double**<->dense conversion,\n");
    std::printf("i.e. the same work the current MathStructures.h wrapper does.\n\n");
    std::printf("%-4s %12s %12s %12s %12s %12s %10s %10s\n",
                "n", "ALGLIB", "Eigen Full", "Eigen Part", "A.inverse()", "copy-only", "Full/Alg", "Part/Alg");
    std::printf("%s\n", std::string(98, '-').c_str());

    for (int n : {2, 3, 4, 6, 8, 10, 20}) {
        MatrixLike m, out;
        m.initialize(n, n);
        out.initialize(n, n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) m.setValue(r, c, dist(rng) + (r == c ? 4.0 : 0.0));

        const int iters = n <= 10 ? 2000 : 300;

        for (int i = 0; i < 500; ++i) { g_sink += alglibInverse(m, out); }  // warmup

        const double tAlg = medianNsPerOp([&](int) { g_sink += alglibInverse(m, out); }, iters);
        const double tFull = medianNsPerOp([&](int) { g_sink += eigenFullPiv(m, out); }, iters);
        const double tPart = medianNsPerOp([&](int) { g_sink += eigenPartialPiv(m, out); }, iters);
        const double tInv = medianNsPerOp([&](int) { g_sink += eigenMatrixInverse(m, out); }, iters);
        const double tCopy = medianNsPerOp([&](int) { g_sink += copyBaseline(m, out); }, iters);

        std::printf("%-4d %12.1f %12.1f %12.1f %12.1f %12.1f %9.2fx %9.2fx\n",
                    n, tAlg, tFull, tPart, tInv, tCopy, tFull / tAlg, tPart / tAlg);
        std::fflush(stdout);
    }

    // Cost at the realistic 6-factor shape.
    {
        const int n = 6, iters = 5000;
        MatrixLike m, out;
        m.initialize(n, n);
        out.initialize(n, n);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) m.setValue(r, c, (r == c) ? 1.0 : 0.02 * ((r + 2 * c) % 3 - 1));
        const double tAlg = medianNsPerOp([&](int) { g_sink += alglibInverse(m, out); }, iters);
        const double tFull = medianNsPerOp([&](int) { g_sink += eigenFullPiv(m, out); }, iters);
        std::printf("\n6x6 pricing matrix (identity + small coupling):\n");
        std::printf("  ALGLIB rmatrixinverse : %8.1f ns -> %.3f s per 1e6 calls\n", tAlg, tAlg * 1e6 / 1e9);
        std::printf("  Eigen FullPivLU      : %8.1f ns -> %.3f s per 1e6 calls\n", tFull, tFull * 1e6 / 1e9);
    }

    std::printf("\n(sink=%g)\n", (double)g_sink);
    return 0;
}
