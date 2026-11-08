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

# ---------------------------------------------------------------------------
# MarketMaker engine (new_impl/mm) — see mm/PLAN.md.
#
# Same flags as viper_rv. The translation unit holding the config->price arithmetic is
# compiled with -ffp-contract=off for the reason measured on rv/valuation.cpp
# (new_impl/CMakeLists.txt:54-66): FMA contraction changes the last bit before the int64
# truncation that produces a price, so the parity gate sees a different tick.
# ---------------------------------------------------------------------------
add_library(viper_mm STATIC
    mm/model.cpp
    mm/config.cpp
    mm/order_queue.cpp
    mm/state.cpp
    mm/quoting.cpp
    mm/hedging.cpp
    mm/strategy.cpp
    mm/positions.cpp
    mm/host.cpp
    mm/adapter.cpp
)

# Fast-path switches, both measured by the differential gate (PLAN.md §0 D6, PARITY_NOTES §4):
#   MM_FAST_EXP=1        polynomial exp() in the queue-position S-curve instead of the libm call
#   MM_HOIST_EST_DIV=0   keep the legacy per-order division by the established level
option(MM_FAST_EXP "Approximate exp() in the queue-position S-curve" OFF)
option(MM_HOIST_EST_DIV "Hoist 1/establishedLevel out of the per-order loop" ON)
if (MM_FAST_EXP)
    target_compile_definitions(viper_mm PUBLIC MM_FAST_EXP=1)
endif()
if (MM_HOIST_EST_DIV)
    target_compile_definitions(viper_mm PUBLIC MM_HOIST_EST_DIV=1)
endif()
target_include_directories(viper_mm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mm
)
target_include_directories(viper_mm PUBLIC ${VIPER_RV_STUBS_DIR})
target_compile_options(viper_mm PRIVATE -O3 -march=native -fno-math-errno -Wall -Wextra)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(viper_mm PRIVATE -Wno-unused-parameter)
    # Same measured reason as rv/valuation.cpp: FMA contraction changes the last bit before
    # the int64 truncation that produces a price, which the differential gate then reports.
    set_source_files_properties(mm/model.cpp mm/quoting.cpp
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
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

# MarketMaker phase-1 gate: price scale (D2), tick division, lot flooring, model derivation
# and validation, and a parse of the real config/backtest files.
add_executable(mm_tests mm/tests/mm_tests.cpp)
target_link_libraries(mm_tests PRIVATE viper_mm)

# MarketMaker phase-2 gate: the L2 book (change indices, level lookup, trade adjustment) and
# the resting-order queue (legacy onOrderUpdate branches, queue positions, caps).
add_executable(mm_state_tests mm/tests/mm_state_tests.cpp)
target_link_libraries(mm_state_tests PRIVATE viper_mm)

# MarketMaker phase-3 gate: the pricing block and updateQuote on the bypass paths, plus the
# differential gate against the legacy transcription at the legacy 1e-8 scale.
add_executable(mm_quote_tests mm/tests/mm_quote_tests.cpp)
target_link_libraries(mm_quote_tests PRIVATE viper_mm)

# MarketMaker phase-5 gate: hedge size reconciliation, hedge pricing, and the full cycle
# assembly (on_md_change / apply_updates) with the ordered request sequence.
add_executable(mm_cycle_tests mm/tests/mm_cycle_tests.cpp)
target_link_libraries(mm_cycle_tests PRIVATE viper_mm)

# MarketMaker phase-6 gate: the fill state machine and the host (cycle driver, gates, the
# maxTicksWide latch, deferred emission, fill -> re-hedge).
add_executable(mm_host_tests mm/tests/mm_host_tests.cpp)
target_link_libraries(mm_host_tests PRIVATE viper_mm)

# Cycle-level benchmark: ns/cycle per stage plus the zero-allocation invariant (PLAN.md §7).
add_executable(mm_cycle_bench mm/tests/mm_cycle_bench.cpp)
target_link_libraries(mm_cycle_bench PRIVATE viper_mm)

# Runnable driver: real config directory, deterministic tape, deployment invariants.
add_executable(mm_replay mm/app/mm_replay.cpp)
target_link_libraries(mm_replay PRIVATE viper_mm)

# MarketMaker phase-8 gate: the integration seam (adapter + request_sink).
add_executable(mm_adapter_tests mm/tests/mm_adapter_tests.cpp)
target_link_libraries(mm_adapter_tests PRIVATE viper_mm)

enable_testing()
add_test(NAME rv_parity COMMAND rv_tests 20000)
add_test(NAME rv_bench_smoke COMMAND rv_cycle_bench 20000 3)
add_test(NAME rv_host COMMAND rv_host_test)
add_test(NAME rv_replay_smoke COMMAND rv_replay config/minimal_cme 500 42)
add_test(NAME mm_unit COMMAND mm_tests)
add_test(NAME mm_state COMMAND mm_state_tests)
add_test(NAME mm_quote COMMAND mm_quote_tests 200000)
add_test(NAME mm_cycle COMMAND mm_cycle_tests)
add_test(NAME mm_host COMMAND mm_host_tests)
add_test(NAME mm_bench_smoke COMMAND mm_cycle_bench 20000 3)
add_test(NAME mm_replay_smoke COMMAND mm_replay config/backtest 20000 42)
add_test(NAME mm_adapter COMMAND mm_adapter_tests)
# The config-driven tests/benches resolve paths relative to the repository root.
set_tests_properties(rv_parity rv_bench_smoke rv_replay_smoke mm_unit mm_state mm_quote mm_cycle mm_host mm_bench_smoke mm_replay_smoke mm_adapter
    PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
