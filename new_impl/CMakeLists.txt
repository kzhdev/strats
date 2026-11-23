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

# ---------------------------------------------------------------------------
# ArbieSpreader engine (new_impl/as) — see as/PLAN.md.
#
# Same flags as viper_rv. The cold config layer reuses rv's model-CSV/secdb reader, so
# viper_as links viper_rv; the engine headers themselves never include rv (PLAN D7).
# ---------------------------------------------------------------------------
add_library(viper_as STATIC
    as/config.cpp
    as/config_model.cpp
    as/hedger.cpp
    as/host.cpp
    as/model.cpp
    as/paths.cpp
    as/quotes.cpp
    as/state.cpp
)
target_include_directories(viper_as PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/as)
target_link_libraries(viper_as PUBLIC viper_rv Eigen3::Eigen)
target_compile_options(viper_as PRIVATE -O3 -march=native -fno-math-errno -Wall -Wextra)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(viper_as PRIVATE -Wno-unused-parameter)
    # Same measured reason as rv/valuation.cpp: FMA contraction changes the last bit before the
    # int64 truncation that produces a price, so the init-time derivations are pinned to
    # sequential arithmetic (PARITY_NOTES F-AS1).
    set_source_files_properties(as/config_model.cpp as/model.cpp
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# Executor + implied engine (new_impl/exec) — see exec/PLAN.md.
#
# Framework-free on purpose: the core links only viper_rv's `common/` headers, so the order
# pipeline can be built and tested without a strategy. The strategy adapters (exec/adapters) are
# header-only and are only pulled in by exec_adapter_tests, which links all three strategies.
#
# EXEC_QUEUE_SPSC=ON makes the default executor use the cross-thread mailbox (one thread per
# strategy). The single-threaded mailbox needs no atomics and no fences, and both modes are
# instantiated in every build (exec/executor.cpp), so neither can rot.
option(EXEC_QUEUE_SPSC "Use the cross-thread (spsc) order mailbox by default" OFF)
add_library(viper_exec STATIC
    exec/implied_book.cpp
    exec/implied_engine.cpp
    exec/executor.cpp
)
target_include_directories(viper_exec PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/exec
)
target_include_directories(viper_exec PUBLIC ${VIPER_RV_STUBS_DIR})
target_compile_options(viper_exec PRIVATE -O3 -march=native -Wall -Wextra)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(viper_exec PRIVATE -Wno-unused-parameter)
endif()
if (EXEC_QUEUE_SPSC)
    target_compile_definitions(viper_exec PUBLIC EXEC_QUEUE_SPSC=1)
endif()

# Phase-0 gate: the request POD, the per-key behaviour table and the mailbox in both modes.
add_executable(exec_queue_tests exec/tests/exec_queue_tests.cpp)
target_link_libraries(exec_queue_tests PRIVATE viper_exec)

# Two-thread stress for the spsc mailbox (also the ThreadSanitizer target).
add_executable(exec_spsc_stress exec/tests/exec_spsc_stress.cpp)
target_link_libraries(exec_spsc_stress PRIVATE viper_exec pthread)

# Phase-7 gate: external order management, phase 1 (the quote/hedge gates in front of the venue).
add_executable(om_banner_tests om/tests/om_banner_tests.cpp)
target_link_libraries(om_banner_tests PRIVATE viper_exec pthread)
target_include_directories(om_banner_tests PRIVATE om)

# Phase-6 gate: the return path (engine -> strategy), including routed == direct equivalence, the
# enabled mirror, overflow reporting and a two-thread drain.
add_executable(exec_feedback_tests exec/tests/exec_feedback_tests.cpp)
target_link_libraries(exec_feedback_tests PRIVATE viper_exec pthread)

# Phase-5 gate: the multi-threaded deployment shape -- one thread per strategy, one executor thread,
# spsc mailboxes. Catches lost/reordered requests and reports the TSan recipe (PLAN.md §10).
add_executable(exec_threaded_tests exec/tests/exec_threaded_tests.cpp)
target_link_libraries(exec_threaded_tests PRIVATE viper_exec pthread)

# Phase-1 gate: the internal book (apply/remove, price collisions, external-leg continuation, FAK).
add_executable(exec_book_tests exec/tests/exec_book_tests.cpp)
target_link_libraries(exec_book_tests PRIVATE viper_exec)

# Phase-2/3 gate: the pipeline end to end (last request wins, cross, residual, publish, fills,
# cancel, determinism, zero allocation).
add_executable(exec_engine_tests exec/tests/exec_engine_tests.cpp)
target_link_libraries(exec_engine_tests PRIVATE viper_exec)

# Phase-4 gate: the strategy adapters, against the real mm/rv/as port types.
add_executable(exec_adapter_tests exec/tests/exec_adapter_tests.cpp)
target_link_libraries(exec_adapter_tests PRIVATE viper_exec viper_mm viper_as)
target_include_directories(exec_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mm
    ${CMAKE_CURRENT_SOURCE_DIR}/rv
    ${CMAKE_CURRENT_SOURCE_DIR}/as
)

# Phase-1b gate: the differential against an independent transcription of the legacy matching engine
# (tests/match_oracle.hpp + .cpp). Both implementations share no code; one random request tape is
# pushed through both and eleven observables are compared after every step (PLAN.md §8).
add_executable(exec_match_tests exec/tests/exec_match_tests.cpp exec/tests/match_oracle.cpp)
target_link_libraries(exec_match_tests PRIVATE viper_exec)

# Cycle-level benchmark: ns per stage plus the zero-allocation invariant.
add_executable(exec_bench exec/tests/exec_bench.cpp)
target_link_libraries(exec_bench PRIVATE viper_exec)

# Runnable driver: a deterministic request tape through the real pipeline, asserting the
# deployment invariants (no silent loss, no crossed residual, no allocation).
add_executable(exec_replay exec/app/exec_replay.cpp)
target_link_libraries(exec_replay PRIVATE viper_exec)

# Integration driver: the three REAL strategy engines (mm::adapter, rv::host, as::host) wired
# through the adapters into the executor, with the engine's fills and internal book fed back in.
add_executable(exec_pipeline exec/app/exec_pipeline.cpp)
target_link_libraries(exec_pipeline PRIVATE viper_exec viper_mm viper_as)
target_include_directories(exec_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mm
    ${CMAKE_CURRENT_SOURCE_DIR}/rv
    ${CMAKE_CURRENT_SOURCE_DIR}/as
)

# Phase-0 gate: primitives, config readers, init derivations, the frozen model's indices,
# switch paths / spread substitution, and the recorded legacy quirks.
add_executable(as_tests as/tests/as_tests.cpp)
target_link_libraries(as_tests PRIVATE viper_as)

# Phase-1 gate: the runtime state layer -- book updates, best-book selection (legacy
# :313-341), path runtime/order, quotes, fairs and dirty tracking.
add_executable(as_state_tests as/tests/as_state_tests.cpp)
target_link_libraries(as_state_tests PRIVATE viper_as)

# Phase-2 gate: the path calculation, the ranking and the update pass against an independent
# transcription of legacy Path::calculate / PathSorter / Contract::updatePaths.
add_executable(as_paths_tests as/tests/as_paths_tests.cpp)
target_link_libraries(as_paths_tests PRIVATE viper_as)

# Phase-3 gate: the incremental path update -- delta application, re-sort rules, size
# asymmetry and fair-value propagation -- against the legacy transcription, plus the
# arithmetic invariant (incremental price == recomputed price).
add_executable(as_incremental_tests as/tests/as_incremental_tests.cpp)
target_link_libraries(as_incremental_tests PRIVATE viper_as)

# Phase-4 gate: the quote stage -- edge/commission arithmetic, tick rounding, the crossing size
# clamp, the maxTicksThroughMarket bound and emission change detection.
add_executable(as_quote_tests as/tests/as_quote_tests.cpp)
target_link_libraries(as_quote_tests PRIVATE viper_as)

# Phase-5 gate: the hedge stage -- aggressive-quote release order, the canHedge veto with its
# forced recalculation, the per-lean orders and the switch-path transfer.
add_executable(as_cycle_bench as/tests/as_cycle_bench.cpp)
target_link_libraries(as_cycle_bench PRIVATE viper_as)

add_executable(as_replay as/app/as_replay.cpp)
target_link_libraries(as_replay PRIVATE viper_as)

add_executable(as_host_test as/tests/as_host_test.cpp)
target_link_libraries(as_host_test PRIVATE viper_as)

add_executable(as_hedge_tests as/tests/as_hedge_tests.cpp)
target_link_libraries(as_hedge_tests PRIVATE viper_as)

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
add_test(NAME as_unit COMMAND as_tests)
add_test(NAME as_state COMMAND as_state_tests)
add_test(NAME as_paths COMMAND as_paths_tests 2000 42)
add_test(NAME as_incremental COMMAND as_incremental_tests 2000 42)
add_test(NAME as_quote COMMAND as_quote_tests 2000 42)
add_test(NAME as_host COMMAND as_host_test)
add_test(NAME as_replay_smoke COMMAND as_replay config/minimal_cme 500 42)
add_test(NAME as_bench_smoke COMMAND as_cycle_bench 20000 3)
add_test(NAME as_hedge COMMAND as_hedge_tests 500 42)
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
add_test(NAME exec_queue COMMAND exec_queue_tests)
add_test(NAME exec_spsc COMMAND exec_spsc_stress 2000000)
add_test(NAME exec_threaded COMMAND exec_threaded_tests 200000 8)
add_test(NAME exec_feedback COMMAND exec_feedback_tests 500)
add_test(NAME om_banner COMMAND om_banner_tests 200)
add_test(NAME exec_book COMMAND exec_book_tests)
add_test(NAME exec_match COMMAND exec_match_tests 20000 42)
add_test(NAME exec_engine COMMAND exec_engine_tests)
add_test(NAME exec_adapter COMMAND exec_adapter_tests)
add_test(NAME exec_bench_smoke COMMAND exec_bench 20000 3 8 4)
add_test(NAME exec_replay_smoke COMMAND exec_replay 4 2000 42)
add_test(NAME exec_pipeline_smoke COMMAND exec_pipeline config/minimal_cme config/backtest 500 42)
# The config-driven tests/benches resolve paths relative to the repository root.
set_tests_properties(rv_parity as_unit as_state as_paths as_incremental as_quote as_hedge as_host as_replay_smoke as_bench_smoke rv_bench_smoke rv_replay_smoke mm_unit mm_state mm_quote mm_cycle mm_host mm_bench_smoke mm_replay_smoke mm_adapter
    exec_queue exec_spsc exec_threaded exec_feedback om_banner exec_book exec_match exec_engine exec_adapter exec_bench_smoke exec_replay_smoke exec_pipeline_smoke
    PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
