# RelativeValue vector-market-price benchmark

This benchmark compares the two loops in `RelativeValue::calculateVectorMarketPrices()` with a preallocated Eigen matrix-vector implementation. It generates deterministic mock snapshots of already-fitted outright prices, applies the same pricing multipliers in both implementations, checks numerical equivalence, and reports the median of alternating timed runs.

The current baseline deliberately keeps Viper's row-by-row matrix allocation pattern. Matrix construction, Eigen construction, mock-price generation, and all allocations are outside the timed section.

## Build

The benchmark is isolated from Viper's production CMake target. It requires Eigen 3.3 or newer.

Windows with this checkout's vcpkg installation:

```powershell
Y:\repo\vcpkg\vcpkg.exe install eigen3:x64-windows --classic
cmake -S benchmarks -B benchmarks\build -DCMAKE_BUILD_TYPE=Release -DVCPKG_MANIFEST_MODE=OFF -DCMAKE_TOOLCHAIN_FILE=Y:\repo\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build benchmarks\build --config Release
```

Run the benchmark:

```powershell
.\benchmarks\build\Release\relative_value_vector_market_price_benchmark.exe
```

For a longer run at the six-factor shape used by the sample models:

```powershell
.\benchmarks\build\Release\relative_value_vector_market_price_benchmark.exe --dimension 6 --updates 10000000 --runs 9
```

## Parameters

```text
--dimension N       Number of factors and pricing contracts; default 6.
--mock-updates N    Number of deterministic price snapshots cycled through; default 4096.
--updates N         Timed factor calculations per run; default 2000000.
--warmup N          Untimed calculations before timing; default 20000.
--runs N            Alternating current/Eigen runs; median reported; default 7.
--seed N            Deterministic mock-price seed.
```

Run it on the intended production CPU and with the compiler flags used for deployment. The result is a measurement of this one factor-calculation kernel, not the complete RelativeValue market-data path.
