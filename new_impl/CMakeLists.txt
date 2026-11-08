# Standalone build for the new implementation. Intentionally separate from the
# legacy RhoneDs target: C++20, AVX2/FMA enabled, no -ffast-math (see PLAN.md §8).
#
#   cmake -S new_impl -B new_impl/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build new_impl/build
#
# Eigen is header-only; point Eigen3_DIR at a vcpkg/CMake package or set
# EIGEN3_INCLUDE_DIR to the header folder directly.

cmake_minimum_required(VERSION 3.16)
project(viper_new_impl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Eigen3 3.3 CONFIG QUIET)
if (NOT TARGET Eigen3::Eigen)
    if (NOT EIGEN3_INCLUDE_DIR)
        message(FATAL_ERROR "Eigen3 not found: install eigen3 or set EIGEN3_INCLUDE_DIR")
    endif()
    add_library(eigen_headers INTERFACE)
    target_include_directories(eigen_headers INTERFACE ${EIGEN3_INCLUDE_DIR})
    add_library(Eigen3::Eigen ALIAS eigen_headers)
endif()

add_library(viper_rv STATIC
    common/liquidity_ledger.cpp
    common/market.cpp
    rv/model.cpp
    rv/state.cpp
    rv/valuation.cpp
    rv/hedger.cpp
    rv/quotes.cpp
    rv/strategy.cpp
    rv/host.cpp
    rv/config.cpp
    rv/config_model.cpp
)
target_include_directories(viper_rv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/rv)
target_link_libraries(viper_rv PUBLIC Eigen3::Eigen)

# Standalone builds need the vendored tachyon/zenith stubs that shadow the real
# framework headers (see PLAN.md §8). They live in the source tree so a fresh clone
# builds without any manual setup; point VIPER_RV_STUBS_DIR at the real framework
# include path instead when integrating (the stubs are then simply not found first).
set(VIPER_RV_STUBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    CACHE PATH "Include dir for tachyon/zenith stubs (see new_impl/stubs)")
if (NOT EXISTS "${VIPER_RV_STUBS_DIR}/tachyon/core/om/order_types.hpp")
    message(FATAL_ERROR
        "VIPER_RV_STUBS_DIR='${VIPER_RV_STUBS_DIR}' does not look like a stub/include root "
        "(missing tachyon/core/om/order_types.hpp). Use new_impl/stubs or the real framework headers.")
endif()
target_include_directories(viper_rv PUBLIC ${VIPER_RV_STUBS_DIR})
# AVX2 + FMA are deliberate (legacy builds with -mno-avx/-mno-avx2). No -ffast-math:
# it would break the parity contract in PLAN.md §5.
target_compile_options(viper_rv PRIVATE -O3 -march=native -fno-math-errno -Wall -Wextra)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(viper_rv PRIVATE -Wno-unused-parameter)
    # PLAN §6.4 fallback, applied because it was measured: with FMA contraction on, the
    # synthetic differential produced 3 stf and 6 quote-price diffs per 200 000
    # cycle-contracts (99.9985 % parity, within §6.3 but not "explained"); with
    # -ffp-contract=off on this one translation unit the gate is 100 % identical, and the
    # pricing pipeline cost is unchanged within noise (PARITY_NOTES §1a). AVX2 stays on:
    # only the fused multiply-add is disabled.
    set_source_files_properties(rv/valuation.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

add_executable(rv_tests rv/tests/rv_tests.cpp)
target_link_libraries(rv_tests PRIVATE viper_rv)

add_executable(rv_cycle_bench rv/tests/rv_cycle_bench.cpp)
target_link_libraries(rv_cycle_bench PRIVATE viper_rv)

# End-to-end host tests (composition root: quote lifecycle, validation gate, ledger round trip).
add_executable(rv_host_test rv/tests/rv_host_test.cpp)
target_link_libraries(rv_host_test PRIVATE viper_rv)

# Runnable driver: loads a real config directory, replays a deterministic tape through the
# host and asserts the deployment invariants (see rv/app/rv_replay.cpp).
add_executable(rv_replay rv/app/rv_replay.cpp)
target_link_libraries(rv_replay PRIVATE viper_rv)

enable_testing()
add_test(NAME rv_parity COMMAND rv_tests 20000)
add_test(NAME rv_bench_smoke COMMAND rv_cycle_bench 20000 3)
add_test(NAME rv_host COMMAND rv_host_test)
add_test(NAME rv_replay_smoke COMMAND rv_replay config/minimal_cme 500 42)
# The config-driven tests/benches resolve paths relative to the repository root.
set_tests_properties(rv_parity rv_bench_smoke rv_replay_smoke PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
