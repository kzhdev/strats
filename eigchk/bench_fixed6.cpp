// Separates "arithmetic cost" from "allocation cost" at Viper's 6x6 shape.
#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "linalg.h"

static volatile double g_sink = 0.0;
using Clock = std::chrono::steady_clock;

template <typename Fn>
static double medianNs(Fn&& fn, int iters, int repeats = 7) {
    std::vector<double> s;
    for (int r = 0; r < repeats; ++r) {
        const auto t0 = Clock::now();
        for (int i = 0; i < iters; ++i) fn();
        const auto t1 = Clock::now();
        s.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
    }
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr int n = 6;
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    double src[n * n];
    for (auto& v : src) v = dist(rng);
    for (int i = 0; i < n; ++i) src[i * n + i] += 4.0;

    alglib::real_2d_array a;
    a.setlength(n, n);
    alglib::ae_int_t info;
    alglib::matinvreport rep;

    Eigen::MatrixXd dyn(n, n), dynOut(n, n);
    Eigen::Matrix<double, n, n> fix, fixOut, fixInv;

    const int iters = 20000;

    const double tAlg = medianNs([&] {
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) a[r][c] = src[r * n + c];
        alglib::rmatrixinverse(a, n, info, rep);
        g_sink += a[0][0];
    }, iters);

    const double tDynFull = medianNs([&] {
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) dyn(r, c) = src[r * n + c];
        Eigen::FullPivLU<Eigen::MatrixXd> lu(dyn);
        dynOut.noalias() = lu.inverse();
        g_sink += dynOut(0, 0);
    }, iters);

    const double tDynPart = medianNs([&] {
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) dyn(r, c) = src[r * n + c];
        dynOut.noalias() = dyn.partialPivLu().inverse();
        g_sink += dynOut(0, 0);
    }, iters);

    // Fixed-size: nothing is heap allocated, so this is the arithmetic-only cost.
    const double tFixFull = medianNs([&] {
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) fix(r, c) = src[r * n + c];
        Eigen::FullPivLU<Eigen::Matrix<double, n, n>> lu(fix);
        fixOut.noalias() = lu.inverse();
        g_sink += fixOut(0, 0);
    }, iters);

    const double tFixPart = medianNs([&] {
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) fix(r, c) = src[r * n + c];
        fixOut.noalias() = fix.partialPivLu().inverse();
        g_sink += fixOut(0, 0);
    }, iters);

    std::printf("6x6 inverse, ns per call (median of 7 runs of %d)\n\n", iters);
    std::printf("  ALGLIB rmatrixinverse           %10.1f ns   1.00x\n", tAlg);
    std::printf("  Eigen FullPivLU  (dynamic)      %10.1f ns   %5.2fx faster\n", tDynFull, tAlg / tDynFull);
    std::printf("  Eigen PartialPivLU (dynamic)    %10.1f ns   %5.2fx faster\n", tDynPart, tAlg / tDynPart);
    std::printf("  Eigen FullPivLU  (fixed 6x6)    %10.1f ns   %5.2fx faster\n", tFixFull, tAlg / tFixFull);
    std::printf("  Eigen PartialPivLU (fixed 6x6)  %10.1f ns   %5.2fx faster\n", tFixPart, tAlg / tFixPart);
    std::printf("\n(sink=%g)\n", (double)g_sink);
    return 0;
}
