// Isolates the decomposition cost itself: all buffers preallocated, no heap traffic
// inside the timed region (beyond what each algorithm does internally).
#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
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
    std::vector<double> s;
    for (int r = 0; r < repeats; ++r) {
        const auto t0 = Clock::now();
        for (int i = 0; i < iters; ++i) fn(i);
        const auto t1 = Clock::now();
        s.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
    }
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::printf("decomposition only (preallocated buffers), ns per call, median of 7 runs\n\n");
    std::printf("%-4s %12s %12s %12s %12s %10s %10s\n",
                "n", "ALGLIB", "Eigen Full", "Eigen Part", "copy-only", "Full/Alg", "Part/Alg");
    std::printf("%s\n", std::string(80, '-').c_str());

    for (int n : {1, 2, 3, 4, 6, 8, 10, 20}) {
        std::vector<double> src(static_cast<size_t>(n) * n);
        for (auto& v : src) v = dist(rng);
        for (int r = 0; r < n; ++r) src[r * n + r] += 4.0;

        alglib::real_2d_array a;
        a.setlength(n, n);
        alglib::ae_int_t info;
        alglib::matinvreport rep;

        Eigen::MatrixXd em(n, n);
        Eigen::MatrixXd eo(n, n);
        std::vector<double> out(static_cast<size_t>(n) * n);

        const int iters = n <= 10 ? 2000 : 300;

        // ALGLIB: refill + invert (rmatrixinverse overwrites its argument)
        const double tAlg = medianNsPerOp([&](int) {
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c) a[r][c] = src[r * n + c];
            alglib::rmatrixinverse(a, n, info, rep);
            g_sink += a[0][0];
        }, iters);

        const double tFull = medianNsPerOp([&](int) {
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c) em(r, c) = src[r * n + c];
            Eigen::FullPivLU<Eigen::MatrixXd> lu(em);
            eo.noalias() = lu.inverse();
            g_sink += eo(0, 0);
        }, iters);

        const double tPart = medianNsPerOp([&](int) {
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c) em(r, c) = src[r * n + c];
            eo.noalias() = em.partialPivLu().inverse();
            g_sink += eo(0, 0);
        }, iters);

        // copy-only floor for the same refill amount
        const double tCopy = medianNsPerOp([&](int) {
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c) out[r * n + c] = src[r * n + c];
            g_sink += out[0];
        }, iters);

        std::printf("%-4d %12.1f %12.1f %12.1f %12.1f %9.2fx %9.2fx\n",
                    n, tAlg, tFull, tPart, tCopy, tFull / tAlg, tPart / tAlg);
    }
    std::printf("\n(sink=%g)\n", (double)g_sink);
    return 0;
}
