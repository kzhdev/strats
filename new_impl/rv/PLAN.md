# RelativeValue — minimal re-implementation plan (`new_impl/rv`)

Status: **draft for review** · Owner: TBD · Target: v1 = pricing + quotes + hedge decisions

This document plans a from-scratch, performance-first implementation of the RelativeValue (RV)
strategy in `new_impl/`, using Eigen for the linear algebra. The reference implementation being
replaced is `src/RelativeValue.h` (3,183 lines) with `src/MathStructures.h` + ALGLIB.

The goal is **lower per-cycle latency and a codebase that can be reasoned about**, not a rewrite of
the Viper framework. Everything not on the per-cycle path (GUI, protobuf, config hot-reload, MD
status handling, order transport) stays where it is.

---

## 0. Recorded decisions

| # | Decision | Value |
|---|---|---|
| D1 | Parity standard | **≥ 99.99 % of cycles produce an identical decision; every remaining diff must be explained and logged** (not "bit-exact") |
| D2 | Config source for v1 | ~~Read `ViperConfig` through an adapter~~ — **superseded by instruction**: a standalone `rv_model_cfg` parses the model CSV + secdb directly (§4.8), so the engine has no legacy-framework dependency |
| D3 | Matrix-shape assumption | **Reproduce the legacy square-only assumption**: `n_vectors == n_pricing_contracts`, validated at init |
| D4 | Recorded production sessions | **None available** → golden data must be *generated*; see §6.2 |
| D5 | Eigen | Used for init-time linear algebra and for the batched per-cycle matvecs only; §4.3 |
| D6 | FMA / AVX2 | New target compiles with AVX2 + FMA; parity impact handled by D1 + §6.4 |
| D7 | Hedge **sending** path | **In v1**: decision, sizing, FAK adjustment and the order call are owned by the new engine; only the transport primitive stays behind a seam (§4.7) |
| D8 | Concurrency | **Single-threaded, cycle-driven**; `timestamp_t` in, no atomics, no locks in the cycle |

All review questions from the first pass are closed (D1–D8).

---

## 1. Scope

### 1.1 In scope for v1

| Stage | Legacy reference |
|---|---|
| Mid-points per contract (simple / weighted) | `RelativeValue.h:1921` `updateMidPoint`, `:1998-2012` |
| Over-determined (weighted LS) outright prices | `:1931` `calculateOverdeterminedPrices`, `:2060` `calculateOverdeterminedMatrix` |
| Vector (factor) market prices | `:1942` `calculateVectorMarketPrices` |
| Long-term fair value (EMA) | `:1962` `calculateLongTermFairsInVector` |
| Short-term fair value, per vector and per contract/spread | `:2687` `calculateShortTermFairs` |
| Hedge cost / quote price / quote size maths | `:2963-3004`, `:2409-2458` |
| Lean generation, club aggregation, quote construction | `:2825` `calculateQuotes`, `:123-297` `Quote::createQuote` |
| Residuals, hedge request tiling, hedge order sizing, **and sending** | `:578-607`, `:514-576`, `:2466-2575`, `:2602-2637` |
| Position / vector-exposure bookkeeping | `:2602-2637` `addPosition`, `addPositionToVector` |
| Displayed-size accounting: reservations, source scoping, price-level invalidation, hedge lifecycle (§4.9) | `Common.h:611-696` (`Market::update`/`getAdjustedSize`/FAK adjustments), `new_impl/common/{market,liquidity_ledger}.hpp` |

| Cycle orchestration (dirty tracking, quote emission trigger) | `:387` `runCycle` |

### 1.2 Out of scope for v1 (stays legacy, behind a seam)

- Protobuf messages, GUI (`RelativeValueGUI.cpp`, `sendStateToGUI`, grid updates).
- Config parsing and hot-reload (`ViperConfig`, `onRv*Update`, `*Grid*` handling) — v1 consumes a
  snapshot produced by the adapter (§4.8) and supports "reload = rebuild model" only.
- MD status / venue-specific handling (`onMDStatus`, ICE / EUREX / CME-B3 paths), sweeps, trades.
- Manual fills, shifts, EMA on/off *scheduling* (v1 takes the EMA flag/params as model inputs).
- Order transport *internals*: `OrderRequestManager` queue/ring mechanics, `Executor`,
  `InternalMatchingEngine`. v1 calls a narrow `order_port` seam (`send_hedge`, `send_quote`) —
  the hedge and quote **decisions** are in v1 (D7).

- Logging (log4cxx), `PerfMonitor`, latency tracking, KDB.

These are ~70 % of the legacy line count and ~0 % of the per-cycle cost (see §3).

---

## 2. Reference model (what must be computed)

All quantities are `int64` fixed point with **1e9 scaling** (`PRICE_SCALE`, matching tachyon's
`DEFAULT_FIXED_PRICE_MULTIPLIER`; legacy `hf::price_t` used 1e-8 — see PARITY_NOTES F5;
`ToDollars` — `src/Common.h:267-277`). Let:

- `n_all` — all contracts (outrights + spreads)
- `n_pricing` — pricing contracts (= outrights that define the factor space)
- `n_vectors` — factors (vectors)
- `lean(c)` — set of contracts contract `c` may lean on (`_leanMatrix`, `:3083`)

Init-time operators (computed once, from `ViperConfig`):

```
P     [n_vectors x n_pricing]  pricing matrix                      (ViperConfig::populatePricingMatrix)
risk  [n_vectors x n_pricing]  = P^-1 transposed                   (:2026, :2097)
ODM   [n_pricing x n_all]      = (A^T W^T W A)^-1 A^T W^T W        (:2097)
hedge[c][j]  [n_all x n_all]   hedge ratio, precomputed            (:2239-2292)
qsize[c][j]  [n_all x n_all]   quote size, precomputed             (:2409-2458)
```

Per-cycle dataflow (dense — every stage sees every mid):

```
mid[c]      = weighted | simple midpoint of best bid/ask                (:1998-2012)
pricing[p]  = SUM_c ODM[p][c] * mid[c]                                 (:1931-1940)
factor[m]   = SUM_p P[m][p] * pricing[p] * mult[p]                     (:1942-1960)
ltf[m]      = EMA(factor[m]) | factor[m] | external override           (:1962-1996)
stf_m[m]    = setToMarket ? factor[m] : ltf[m] - posVec[m]*fareMove[m] (:2687-2699)
stf[c]      = SUM_m stf_m[m] * risk[m][c] / mult[c]                    (:2701-2715)
stf[spread] = SUM_legs stf[leg] * ratio                                (:2717-2735)
lean_px(s,c,j) = fair[c] -/+ width[c] -/+ hedgeCost -/+ comm/mult      (:2954-3004)
quote(s,c)  = club-aggregate over lean_px, rounded to tick             (:123-297)
```

Three properties drive every design decision:

1. **No locality.** One mid change reaches every factor and every contract. Incremental
   invalidation buys nothing; the full recompute must be cheap. This is a feature — there is no
   dependency graph to maintain or debug.
2. **All stages are tiny dense linear maps.** Batched matvecs with `n_vectors <= 32`,
   `n_pricing <= 32`, `n_all <= 128` are exactly where Eigen wins, and the operators are
   precomputed, so the only per-cycle Eigen work is a handful of matvecs.
3. **The quote stage dominates.** It is `n_all x 2 x |lean(c)|` scalar computations, and in the
   legacy implementation each one performs ~5 divisions (§3.1).

---

## 3. Sizing and cost model

Reference configurations in the repo:

| Config | `n_vectors` | `n_pricing` | `n_all` | Note |
|---|---|---|---|---|
| `config/backtest/model.csv` (CME QMQ1…QMX1) | 4 | 4 | 10 | square — the backtest model |
| `test/Fixtures/unit_test_config.csv` | 29 | 14 | ~50 | **non-square → invalid under D3** (see R3 in §10) |
| Caps in `limits.hpp` (proposal) | 32 | 32 | 128 | validated at init, hard fail if exceeded |

Per-cycle work at the backtest config (`n_all=10`, `n_pricing=4`, `n_vectors=4`, `|lean|~9`):

| Stage | Ops | Legacy cost driver |
|---|---|---|
| mids (updated contracts only) | ~10 | `double` <-> `price_t` conversion |
| `ODM * mid` | 40 FMA | `double**` indirection, non-inlined `getValue` |
| `P * (pricing .* mult)` | 16 FMA | same |
| EMA | ~40 flops | negligible |
| `risk * stf_m` + spread legs | ~50 flops | same |
| **quote stage** | 10 x 2 x 9 = **180 lean computations** | **~4 double divisions + 1 int64 division each** |

Estimate: pipeline ≈ 100–200 cycles; quote stage ≈ 180 x ~85 cycles ≈ **15 k cycles ≈ 5 µs** at
3 GHz. That is ~96 % of the arithmetic cost of the strategy, and it is entirely avoidable.

### 3.1 The five divisions per lean

Per lean, per side, the legacy code performs (`RelativeValue.h:2963-2987`):

| Op | Line | Cost |
|---|---|---|
| `commissions[c][j] / contract._pricingMultiplier` | `:2970` | double div |
| `lean._pricingMultiplier / contract._pricingMultiplier` | `:2978` | double div |
| `lean._quantityMultiplier / contract._quantityMultiplier` | `:2979` | double div |
| clamp against `0.95 * width` | `:2981` | 1 mul |
| `RoundToTickSize`: `rawPrice % tickSize` | `:193`, `Common.h:1196` | **int64 div (~20–40 cyc)** |

`commissions`, both multipliers, `width` and `tickSize` are **constant for the life of the model**.
Every one of those divisions recomputes an init-time constant, per cycle, per lean, per side.

Fix — precompute per `(contract, lean)` pair:

```
commission_scaled[c][j] = commissions[c][j] / contractPricingMult      (int64, truncated once)
hedge_unity[c][j]       = all four multipliers exactly 1.0
tick_div[c]             = magic-number divider for tickSize[c]
```

and dispatch once per pair:

```
hedge_cost = hedge_unity ? (price_t)(cost * ratio)                              // 1 mul, no div
                         : (price_t)(cost*ratio*leanPm/contractPm*leanQm/contractQm)  // legacy order
```

The commission division (a per-cycle division today) is always safe to hoist: it is a
single `int64 / double` whose result is truncated once. The **hedge-cost scaling is not
safe to precompute as a single factor**: the oracle measured a **0.5–0.7 % tick
mismatch rate** when the four operations were folded into one multiply, even with
power-of-ten multipliers (the association changes the last bit before truncation). So the
fast path is used only where it is *provably* bit-identical — all four multipliers exactly
1.0 — and the legacy four-division expression is kept otherwise. `rv_tests` sweeps 10.5 M
multiplier/cost/ratio combinations and requires **zero** mismatches.

### 3.2 Other legacy hot-path findings

| # | Finding | Location | Fix |
|---|---|---|---|
| 1 | Full `n_all^2` scan of `_leanMatrix` per contract per side | `:2840-2841` | CSR lean list |
| 2 | Leans sorted every cycle | `:158-162` → `Leans::sortBids/sortAks` | single best-scan, O(k) |
| 3 | Per-cycle heap traffic: `std::vector` reserve, `Vector::init`, inverse temporaries | `:2831`, `:2409-2411`, `MathStructures.h:149` | zero-allocation contract (§7.3) |
| 4 | Hash containers on the hot path | `:3135`, `:3099`, `:3148` | bitset + packed index arrays |
| 5 | `double**` matrices, `BOOST_ASSERT` per element access | `MathStructures.h:100-243` | contiguous row-major `double[]` + `Eigen::Map` |
| 6 | `Matrix` leaks on every copy — `removeMemory()` body is commented out, no destructor | `MathStructures.h:132-147` | fixed by construction (POD storage) |
| 7 | Every lean materialised into an array, only the best is used | `Quote::createQuote` `:155-215` | best-only accumulation |
| 8 | `LOG`/`TLOG` calls inside per-lean loops | e.g. `:198`, `:2552` | new loop has no logging |
| 9 | Position update is a matvec written element-wise: `addPositionToVector` loops all vector columns per leg through `_riskMatrix.getValue` | `:2614-2637` | one `pos_vec += risk · Δp` matvec |
| 10 | `pickBestHedge` allocates a `std::vector<uint32_t>` of ties per call | `:2323-2346` | fixed-size array + count |

---

## 4. Design

### 4.1 File layout

```
new_impl/rv/
  limits.hpp           compile-time caps (MAX_CONTRACTS, MAX_PRICING, MAX_VECTORS)
  types.hpp            POD config/state types, fixed-point helpers, fast_div + round_to_tick, side tags
  model.hpp/.cpp       class model: immutable-after-init operators + per-(contract, lean)
                       coefficients + CSR leans/legs, with contract queries and model::build()
  state.hpp/.cpp       class cycle_state: books, fairs, quotes, positions, dirty set; owns
                       market()/best_market()/quote_of()/mark_dirty()/init()
  valuation.hpp/.cpp   class valuation: mid -> ODM -> factor -> ltf/stf (the dense pipeline,
                       Eigen matvecs)
  quotes.hpp/.cpp      class lean_calculator (lean price/size/cost maths, shared with hedging),
                       class quote_engine (best/club aggregation, tick rounding, change
                       detection, maxTicksThroughMarket gate) + the quote_policy seam
                       (validate, adjusted_size, price_changed)
  hedger.hpp/.cpp      class hedger + order_port seam: hedge requests, residuals, tiling/sizing,
                       club walk, order emission, position/vector-exposure bookkeeping
  strategy.hpp/.cpp    class strategy: the façade -- input surface, enabled flag, cycle order;
                       composes valuation + quote_engine + hedger
  ledger_bridge.hpp    binds quote_policy/order_port to common::liquidity_ledger (see §4.9)
  host.hpp/.cpp        composition root: owns model + state + ledger + strategy, the PLAN §4
                       input surface, the maxTicksThroughMarket gate and the quote lifecycle
  config.hpp/.cpp      rv_model_cfg: model CSV + secdb parser (§4.8)
  config_model.cpp     derivation of model_inputs from the parsed config (§4.8)
  app/rv_replay.cpp    runnable driver: real config -> model -> ledger -> host -> deterministic
                       tape, with deployment invariant checks (exit code)
  tests/               reference_oracle.hpp, rv_tests.cpp, rv_host_test.cpp, rv_cycle_bench.cpp
```

Namespace `tachyon::trade::rv`, types `snake_case`, `#pragma once`, `noexcept` on the hot path, no
exceptions in the cycle (matching `common/market.hpp`, `common/liquidity_ledger.hpp`).

`order_port.hpp` from the original plan is now a struct inside `hedger.hpp` (three function
pointers: `send_hedge`, `send_quote`, `ctx`), and `config_adapter.hpp/.cpp` became `config.*`
(no `ViperConfig`, §4.8). `pricing.*` was renamed `valuation.*` when the pipeline became an object.

### 4.1b Object model (who owns what)

The engine is six collaborating classes, each owning one stage and its data. There are no free
functions on `(model, state, ...)`: behaviour lives on the object that owns the data it mutates, so a
caller cannot reach a stage's invariants (a double-marked dirty contract, a half-initialised state, a
quote built without the maxTicksThroughMarket gate) from the outside.

| Class | Owns | Collaborators it calls | Legacy counterpart |
|---|---|---|---|
| `model` | immutable config: operators, coefficient tables, CSR leans/legs | -- (factory: `model::build`) | `ViperConfig` snapshot + `initialize*` |
| `cycle_state` | books, `mid`/`stf`/`ltf`/`pos_vec`, quotes, positions, dirty set | -- | the strategy's member fields |
| `valuation` | nothing but the model reference | `cycle_state` (read/write) | `calculateMidpoint`, `calculateShortTermFairs`, ... |
| `lean_calculator` | nothing (stateless, static methods) | -- | `calculateQuote`, `calculateHedgingCost`, `calculateSize` |
| `quote_engine` | the quote policy (seam) | `lean_calculator`, `cycle_state` | `calculateQuotes`, `Quote::createQuote`, `validateMaxTicksThroughMarket` |
| `hedger` | the request book, hedge id counter, rhone clips | `lean_calculator`, `order_port`, `cycle_state` | `handleFill`, `getResiduals`, `sendHedges`, `addPosition` |
| `strategy` | enabled flag + cycle order | `valuation`, `quote_engine`, `hedger` | `runCycle` |
| `host` | ledger bridge, ports, venue gates, stats | `strategy` (facade), `ledger_bridge` | the `RelativeValue` event handlers |

`strategy::run_cycle` is the only place that orders them: mids (dirty) -> `valuation::recompute` ->
`quote_engine::build_all` -> at most one `hedger::run` -> recompute/rebuild -> clear dirty. The hedger
reports a fail-safe by return value (`run()` -> must-disable); it does not own the enabled flag.

**Two rules keep this free.** They are not stylistic -- both were measured (callgrind, PLAN §7.3):

1. **No state in `lean_calculator`.** It takes `const model&` per call. Storing a `const model&`
   member adds a second indirection to every table access in the hottest loop and cost **+5 % of the
   cycle** before the reference points were compared away.
2. **Hot kernels stay file-local.** `quote_engine::build`'s lean loops and `valuation::recompute`'s six
   stages are `inline` functions in an anonymous namespace, not members. As class members with
   external linkage GCC keeps them out of line (~2.8M extra instructions per 2000 cycles, +4 %).
   Public members are the API; the kernels are implementation detail.

With both rules the refactored engine is instruction-identical to the free-function version in the
quote build (45.75M vs 45.78M) and the pricing pipeline (identical), and the full cycle measures
1964 ns against 1955-1968 ns before (same machine, same flags).

### 4.2 Caps, storage layout, zero allocation

All sizes are bounded at compile time so the cycle performs **no heap traffic**:

```cpp
// limits.hpp
constexpr uint32_t MAX_CONTRACTS = 128;   // n_all
constexpr uint32_t MAX_PRICING   = 32;    // n_pricing (== n_vectors under D3)
constexpr uint32_t MAX_VECTORS   = 32;
constexpr uint32_t MAX_LEANS     = 512;   // total CSR edges, ~ MAX_CONTRACTS * MAX_CONTRACTS / 2
constexpr uint32_t MAX_UPDATES   = MAX_CONTRACTS;
```

`model` holds the operators as **flat row-major `std::vector<double>`**, written once at init and
never mutated. `cycle_state` holds all mutable arrays, sized at init, reused every cycle:

```cpp
struct cycle_state {
    std::array<double, MAX_CONTRACTS>  mid{};        // dollars
    std::array<double, MAX_PRICING>    pricing{};
    std::array<double, MAX_VECTORS>    factor{}, ltf{}, stf_vec{}, pos_vec{};
    std::array<int64_t, MAX_CONTRACTS> stf{};        // contract short-term fair, 1e9 ticks
    std::array<quote, MAX_CONTRACTS * 2> quotes{};   // [side * MAX_CONTRACTS + c]
    std::array<uint32_t, MAX_UPDATES>  changed{};    // packed dirty list
    uint32_t changed_count = 0;
};
```

Flat arrays indexed by contract id (no per-contract `std::vector`, no `unordered_map`) remove
findings 3, 4, 5 from §3.2 by construction. Alignment: `alignas(64)` on the enclosing struct so
each cycle touches the same cache lines; the model's contiguous arrays let `Eigen::Map` read them
directly.

Why caps rather than dynamic: it makes the model reloadable by plain assignment, keeps all hot
arrays on one allocation, and gives Eigen compile-time outer dimensions for the specialised paths.
Exceeding a cap is an init-time hard failure with an actionable message (today: silent
`std::runtime_error("Matrix is not square")` from `MathStructures.h:151`).

### 4.3 Eigen policy (where it is used, and where it is not)

| Use Eigen | Where | Why |
|---|---|---|
| Yes — init only | `risk = P⁻¹ᵀ`, `ODM = (AᵀWᵀWA)⁻¹AᵀWᵀW` (`FullPivLU`) | Exactly the ALGLIB call site; also gives `isInvertible()` for the fail-fast rule in §5.2 |
| Yes — per cycle, batched | `ODM·mid`, `P·(pricing⊙mult)`, `risk·stf_vec`, EMA vector ops | One matvec per stage, 16–850 FMA, preallocated, `noalias()` |
| No — per lean | quote price / hedge cost / tick rounding | Scalar integer work; expression templates only add compile time |
| No — anywhere | `-ffast-math` | Breaks the §5 semantics and makes diffs unexplainable |

Operators are stored as plain `double[]` and wrapped with
`Eigen::Map<const Matrix<double, Dynamic, Dynamic, RowMajor>>` at the use site. This avoids
`aligned_allocator` / `std::vector<Eigen::...>` alignment pitfalls, keeps `model` trivially
copyable, and costs nothing at run time.

Sizing: start with dynamic-size preallocated `Map`/`MatrixXd`; specialise the two hottest products
to compile-time-size (`Matrix<double, MAX_VECTORS, MAX_PRICING>`) **only if** the benchmark shows
>= 10 % (measured earlier on this repo: fixed-size 6x6 solve was 2.4x faster than dynamic, but that
was an O(n³) solve, not an O(n²) matvec).

### 4.3b Fixed point vs double: where the boundary is, and why it does not move

The engine is **not** "all doubles". Prices are `int64` fixed point at 1e9 (`PRICE_SCALE`,
`types.hpp`) and the cycle is integer-dominated; `double` occupies exactly one band:

| Stage | Type | Why it cannot be integer |
|---|---|---|
| books, sizes, positions, thresholds, widths, commissions | `int64` / `uint64` | exact by construction; this is what the order message carries |
| midpoint (`state.mid`) | `double` dollars | a midpoint is genuinely fractional (half a tick, and the weighted mid divides by a size sum). Legacy computes it in double and the parity gate is bit-exact against that |
| ODM, `P`, `risk` operator tables | `double` | `P⁻¹` and `(AᵀW²A)⁻¹` are non-terminating rationals: no fixed-point representation is exact, so every use would carry a residual |
| `pricing` / `factor` / `ltf` / `stf_vec` | `double` | products with those operators, plus the EMA (`exp(-α)`) which is transcendental |
| short-term fair (`state.stf`) | `int64` | truncated back to price units as soon as the value is final (`to_rhone_price_saturated`) |
| quote price, club, tick rounding, sizes | `int64` | all of quoting and hedging, i.e. 85 % of the cycle |

Three reasons not to push the integers further into the middle:

1. **On this hardware, 64-bit integer arithmetic is *slower* than double** for exactly the two
   operations involved. Measured (`-O3 -march=native`, GCC 11.5, same loop shape, operands from
   memory, variable divisor as in real tick sizes):

   | Operation | int64 | double |
   |---|---|---|
   | division by a variable divisor | 3.7 ns | **2.1 ns** (`idiv` vs `divsd`) |
   | matvec 4x10, `__int128` accumulate | 70 ns | **29 ns** (2.5x) |
   | matvec 32x128, `__int128` accumulate | 6.25 us | **3.9 us** (1.6x) |

   AVX2 has no 64-bit integer multiply (`vpmullq` is AVX-512DQ) and FMA retires four doubles per
   instruction, so a fixed-point matvec cannot use the vector units at all. This is also why the one
   integer division in the hot path is already gone (`fast_div`, magic-number `` `%` `` replacement).

2. **Eigen cannot do the decomposition in fixed point.** `FullPivLU` / `inverse()` need a field;
   integer division is not exact, so integer scalar types are only usable for coefficient-wise ops
   and products. In any case that work happens once in `model::build`, so making it integer would buy
   exactly zero per cycle.

3. **The prize is small and the parity price is high.** The whole double band is 13 % of the cycle
   (255 ns of 1968 ns). Eliminating it entirely would need int128 accumulation (see 1), changes every
   rounding point, and breaks D1: the reference oracle is a literal double transcription, so an
   integer pipeline diverges by more than a last bit. That is the trade this design deliberately
   refuses.

What *is* worth keeping integer is already integer: the tick grid (exact `round_to_tick`), quantities
and order sizes, and the comparisons that decide quotes. `fast_div` exists because one such
operation (tick division) sat in the inner loop at ~120 leans/cycle.

### 4.4 Pricing pipeline (hot path)

The snippets in §4.4-§4.7 are the **design-phase sketches**. They are kept because they carry the
reasoning; the shipped code expresses the same stages as the classes in §4.1b (`valuation::recompute`,
`quote_engine::build`, `hedger::run`, `strategy::run_cycle`), with the stage kernels file-local.

```cpp
// valuation.cpp — pointers into the model are cached in the engine, nothing is allocated here
inline void recompute(const model& m, cycle_state& s) noexcept {
    using MapV = Eigen::Map<Eigen::VectorXd>;
    using MapCV = Eigen::Map<const Eigen::VectorXd>;

    const MapCV mid(s.mid.data(), m.n_all);
    MapV pricing(s.pricing.data(), m.n_pricing);
    pricing.noalias() = m.odm_map * mid;                     // stage: ODM

    MapV factor(s.factor.data(), m.n_vectors);
    Eigen::VectorXd scaled(m.n_pricing);                      // preallocated scratch, not per-call
    scaled.array() = pricing.array() * m.mult_map.array();
    factor.noalias() = m.pricing_map * scaled;                // stage: P

    update_ema(m, s);                                         // stage: LTF (m <= 32 scalars)
    compute_vector_stf(m, s);                                 // stage: LTF - pos*fareMove | factor
    MapV stf_vec(s.stf_vec.data(), m.n_vectors);
    MapV pricing_stf(s.pricing_stf.data(), m.n_pricing);      // scratch
    pricing_stf.noalias() = m.riskT_map * stf_vec;            // stage: risk^T
    // divide by mult + accumulate spread legs in ticks (int64, exact)
    finish_contract_stf(m, s);
}
```

Two notes that matter for correctness and speed:

- `setToMarket` / `use_ema` / `weighted_mid` are **per-vector / per-contract flags**. Do not branch
  per element in the inner loop; materialise the three variants once per cycle into `stf_vec`
  (the loop is <= 32 elements), so the matvec is uniform. Legacy does the same thing, just with
  runtime `Vector<bool>` reads per element.
- The `int64` tick conversion happens once per contract at the end of the pipeline, not per stage.
  Legacy converts `double -> price_t` at `:2708` and again at `:2728`; keep one truncation point.

### 4.5 Quote stage

```cpp
struct best_quote { int64_t price; uint32_t qty; double size; uint32_t lean; uint32_t club;
                    int64_t ext_price; uint32_t ext_qty; bool has_change; };

inline best_quote compute_quote(const model& m, cycle_state& s, uint32_t c, uint32_t side) noexcept {
    best_quote best{.price = NULL_PRICE};
    const int64_t fair  = s.stf[c];
    const int64_t width = m.width_extra[c];               // width + extraWidth, precomputed
    for (uint32_t k = m.lean_begin[c]; k < m.lean_begin[c + 1]; ++k) {
        const uint32_t j = m.lean_index[k];
        if (m.hedge_ratio[c * m.n_all + j] < 0) continue;      // "canQuote" — precomputed
        const int64_t cost  = side == BID ? s.stf[j] - lean_px(s, j, side)
                                          : lean_px(s, j, side) - s.stf[j];
        const int64_t hedge = hedge_cost(m, c, j, cost, width);          // 1 mul, precomputed scale
        const int64_t raw   = side == BID ? fair - width - hedge - m.comm_scaled[c * m.n_all + j]
                                          : fair + width + hedge + m.comm_scaled[c * m.n_all + j];
        const int64_t px    = round_to_tick(raw, m.tick_div[c], side);   // magic divide
        accumulate(best, px, m.qsize[c * m.n_all + j], j, m.is_external[c * m.n_all + j]);
    }
    return best;
}
```

Semantics preserved from `Quote::createQuote` (`:155-215`): the first lean at the winning rounded
price seeds the club, subsequent leans at the same rounded price add to it; `_leanMarketGone`,
`_hasChange`, `_priceChangeThreshold` are integer comparisons carried over unchanged.

### 4.6 Cycle orchestration

```cpp
void strategy::on_market_data(uint32_t contract, side sd, bool external, price_t price,
                              qty_t size) noexcept {              // per event, O(1)
    market_state& mkt = markets_[market_index(contract, sd, external)];
    if (mkt.price == price && mkt.size == size) return;           // no-op
    if (external && mkt.price != price && policy_.price_changed)  // §4.9 behaviour 3
        policy_.price_changed(policy_.price_changed_ctx, contract, sd, external);
    mkt.price = price; mkt.size = size;                           // POD mirror (§4.9)
    if (!dirty_.test_and_set(contract)) changed_[changed_count++] = contract;
}
void strategy::run_cycle(timestamp_t now) noexcept {              // once per cycle
    if (!changed_count && !hedge_pending_ && !ema_tick(now)) return;   // fast exit, ~5 cycles
    valuation_.refresh_dirty_mids(state_);   // only dirty contracts
    valuation_.recompute(state_, now);       // §4.4 — always full, pipeline is dense
    for (uint32_t c = first_traded_; c < model_.n_all; ++c)
        if (state_.traded[c]) { quotes_[2*c + BID] = compute_quote(...); quotes_[2*c + ASK] = compute_quote(...); }
    emit_changed_quotes();           // diff vs last sent, then order-manager call
    process_hedge_requests();        // §4.7 — decision + sizing + send; may re-run STF
    dirty_.clear(); changed_count_ = 0;
}
```

The fast exit matters: with `config/backtest`-sized models, most MD events touch one contract and
the cycle is fully recomputed, but the *quote emission* stays diff-based (as legacy:
`Quote::_hasChange`, `:175-184`), so the order path sees only real changes.

Single-threaded (D8): the whole cycle, including order emission, runs on the strategy thread. No
atomics, no locks, no memory fences — the model is immutable after init, so there is no sharing to
protect. `ema_tick(now)` takes `timestamp_t` from the caller so the cycle is deterministic and
unit-testable without a clock.

The complete input surface the host must deliver on that thread, in arrival order (R10):

```cpp
void on_market_data(uint32_t contract, side sd, bool external, price_t price, qty_t size) noexcept;
                                                  // external or internal book, price + size
void on_fill(const fill&) noexcept;               // own fill (creates a hedge request) / AS->RV fill
void on_hedge_ack(const hedge_ack&) noexcept;     // FAK done, internal hedge done, MD-adjust ack
void on_command(const command&) noexcept;         // enable/disable, config rebuild, manual fill
```


### 4.7 Hedge path — decision, sizing **and sending** (D7)

Two entry points, both allocation-free:

1. **Fill ingestion** (`:793-825`): accumulate the signed fill into `hedge_request[contract]`
   (flat `double[]` + dirty flags, replacing the `std::unordered_map<uint32_t, double>` at `:3148`),
   and stamp latency tracking from the *first* fill only.
2. **`process_hedge_requests()`** at the top of the cycle (`:514-576`), reproducing exactly:
   `residuals = get_residuals(c, request_side)` (S11) → `size = request + residuals` →
   `abs(size) <= rhone_max_clip[c]`, else disable + abort (S12) → `send_hedges(...)` →
   reconcile positions, retain the unhedged remainder (S13). **One contract per cycle** (S10).

`send_hedges` walks the same **club** the quote was built from (S6), and per lean:
`order_size = get_order_size(...)` (`:2577-2600`, including the `hedge_ratio == 0` "no quote path"
that only decrements `need_to_hedge`), the self-match / 2-ticks-worse guard with the
`skip_club_count` fail-safe (S14), then

```cpp
// emission — the engine only decides; the ledger reservation happens in the port (§4.9)
hedge_order order{lean_contract, other_side, hedge_price, order_size, external,
                  ++hedge_id_, market_index(lean_contract, display_sd, external)};
                                                 // display market, not the order side (O4)
if (port_.send_hedge != nullptr) port_.send_hedge(port_.ctx, order);
add_position(lean, other_side == side::bid ? +order_size : -order_size);
// ledger_bridge::send_hedge → liquidity_ledger::reserve_hedge(key, source, hedge_id, size)
```

Then, exactly as `sendHedgeOrder` does (`:2572-2574`), the quotes for that contract that were
**blocked** earlier in the cycle are emitted immediately
(`send_quote_order(contract, quote_external, /*unblocked_by_hedge=*/true)`), so a hedge never costs
a cycle of quoting latency.

Cost note: `process_hedge_requests` ends with `calculateShortTermFairs()` (`:572`) and hedging moves
positions, so a hedging cycle runs the pricing pipeline **twice**. Budget it explicitly (§7.2) —
this is the only place where a cycle can pay 2x the pipeline cost. Each hedge lean also pays one
division for the remainder (`:2597`); that runs per *fill*, not per cycle, so it is left as-is
rather than precomputed (the divisor is the per-pair hedge ratio, and the hedge path must stay
bit-identical for parity).


The seam (only coupling to legacy order plumbing):

```cpp
struct order_port {                                     // POD, filled by the legacy host
    void (*send_hedge)(void* ctx, const hedge_order&) noexcept = nullptr;
    void (*send_quote)(void* ctx, uint32_t contract) noexcept = nullptr;
    void* ctx = nullptr;
};
```

Hedge-id allocation stays a monotonic counter owned by the strategy (`getHedgeId`, `:2554`), so the
parity harness can compare hedge ordering, not just contents.

### 4.8 Config: `rv_model_cfg` (supersedes the original `ViperConfig` adapter, D2)

**Superseded by instruction:** the config source is now a standalone parser rather than an
adapter over `ViperConfig`. `config.{hpp,cpp}` read the RV-relevant sections straight out of a
Viper model CSV + secdb, so the engine has no dependency on the legacy framework at all:

```cpp
rv_model_cfg cfg = rv_model_cfg::load("config/backtest/model.csv");   // throw with file:line
secdb_table secdb = load_secdb("config/backtest/secdb.cme.csv");      // tick, lot, venue
model_inputs in = build_model_inputs(cfg, secdb, width_multiplier, cycles_per_second);
model m = model::build(in);                                          // Eigen operators
```

The parser is **header-driven** (column order differs between configs — `2msgw` has no
`EMAOnTime`/`EMAOffTime` columns) and skips the non-RV sections as blocks. `build_model_inputs`
ports the init-time derivation from `calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes`
(`:2239-2459`): legs from contract names, tick/lot/venue from secdb (spreads inherit from their
first leg), `threshold = 0.05·tick`, widths split into `width` + `extra_width`, weights,
`establishedLevel`, the lean matrix, per-pair hedge ratios via `pickBestHedge`, commissions with
normalisation, and quote sizes.

Validation happens here or in `model::build`: square model (D3), caps, non-singular pricing matrix
V1), positive ticks, positive multipliers/lots.

Hot reload: out of scope for v1 — a config change rebuilds the model. Measured cost of
parse → derive → build on the backtest model: **30 ms** on this dev box (target was 10 ms; same
6–19× hardware caveat as §7.2).

#### Legacy field equivalence (kept for review; the CSV/secdb path above is what runs)

| `model_inputs` field | `ViperConfig` / legacy source |
|---|---|
| `n_contracts`, `n_pricing`, `n_vectors`, `first_traded` | `_allContracts.size()`, `_numberOfPricingContracts`, `_vectorConfigs.size()`, `_indexOfFirstTradedContract` |
| `pricing_matrix` | `populatePricingMatrix()` |
| `hedge_ratio`, `quote_size`, `commissions`, `factor_hedged_index`, `leans`, `rhone_max_clip` | `_hedgeRatios`, `_quoteSizes`, `_commissions`, `_factorHedgedIndexes`, `_leanMatrix`, `_rhoneMaxClipSize` (built in `:2239-2458`, `:2158`, `:828-843`) |
| `contracts[].tick_size` | `_definition._tickSize` |
| `contracts[].pricing_multiplier`, `quantity_multiplier` | `PricingConfig::_pricingMultiplier`, `_quantityMultiplier` |
| `contracts[].width`, `extra_width` | `_contractWidths`, `_extraContractWidths` (`:1562-1572`) |
| `contracts[].price_change_threshold`, `established_level` | `_priceChangeThreshold`, `_establishedLevel` |
| `contracts[].lot_size` | the lot used by `ConfigContract::toValidQty` |
| `contracts[].weight` | `_weights` (0 ⇒ not `_usedInPricing`) |
| `contracts[].quote_external`, `static_quote`, `static_quote_size`, `weighted_mid`, `traded`, `venue` | `_contractQuoteExternal`, `_staticQuote`, `_staticQuoteSize`, `_useWeightedMid`, `_traded`, `_definition._venue` |
| `contracts[].leg_begin/leg_count`, `legs[]` | `_definition._legs` (`index`, `ratio`), flattened |
| `vectors[].range`, `max_position`, `initial_ltf` | `_vectorsRange`, `_vectorsMaxPosition`, `VectorConfig::_ltf` |
| `vectors[].use_ema`, `set_to_market`, `ema_period_cycles`, `factor_hedged` | `_useEMAInVector`, `_setToMarket`, `_emaPeriodInVector`, `_factorHedged` |

---

### 4.9 Market data and displayed-size accounting (`market`, `liquidity_ledger`)

This layer is part of the design, not an add-on: every quoted size and every hedge is constrained by
it. It lives in `new_impl/common/` and is shared with MM/AS; the RV engine touches it only through
two seams — `quote_policy` (`quotes.hpp`) and `order_port` (`strategy.hpp`).

```
MD event (contract, side, internal|external, price, size)
        │
        ▼
  market_state  (engine, POD)        price + size per book, indexed by market_index()
        │                            on a replaced *external* price level: quote_policy::price_changed
        ▼
  liquidity_ledger (common/)         reservations: HEDGE / AGGRESSIVE_QUOTE / ANTICIPATED_HEDGE,
        │                            keyed by market_key, scoped by liquidity_source
        ├──► adjusted displayed size ──► quote_policy::adjusted_size ──► lean_size() ──► quote size
        └──► hedge emission ──► order_port::send_hedge ──► reserve_hedge() ──► ack / complete
```

### Responsibilities

| Component | Owns | Knows nothing about |
|---|---|---|
| `cycle_state.markets[]` (`market_state{price,size}`) | the book values the pipeline reads | reservations, sources |
| `liquidity_ledger` | reservation records, id allocation, source scoping, group atomicity | prices, quotes, RV model |
| `ledger_bridge` | the `(contract, side, book) → (market_key, liquidity_source)` table and the three callbacks | the model, the pipeline |
| engine (pricing/quotes/strategy) | the math | the ledger's internals |

### The four behaviours this layer must provide

1. **Adjusted size** — `available(key, displayed, filter) = displayed − reservations` where the
   filter's `source` is the source currently supplying the displayed size (legacy
   `Market::getAdjustedSize`, `Common.h:611-696`). Wired as `quote_policy::adjusted_size`.
2. **Source scoping** — a HEDGE reservation only reduces the size of the book whose source matches
   it; quote-group reservations are market-wide (`liquidity_ledger::reserve`, `reservation_filter`).
3. **Flush on price change** — a replaced external level drops its reservations (legacy
   `Market::update`, `Common.h:636-644`); `updateInternalMarket` (`:673-679`) never does. Wired as
   `quote_policy::price_changed`, external books only.
4. **Hedge lifecycle** — `reserve_hedge(key, source, hedge_id, size)` at emission; then
   `acknowledge_external(hedge_id, key, order_id, actual)` on FAK-done and
   `complete_by_hedge_id(hedge_id, key)` on internal-hedge-done. Wired as `order_port::send_hedge`
   plus the bridge's ack/complete entry points (plan risk R9).

### Ownership rule (and why the engine keeps a POD mirror)

`common::market` *is* the natural home for 1–3: it holds price/size, the current `liquidity_source`,
and a `liquidity_ledger*`, and its `update()` already performs the flush. The engine deliberately does
**not** instantiate it, for two reasons:

- the lean loop reads price/size once per lean-side (1,200× per cycle in the dense benchmark) and must
  not pay indirection per read; it reads a flat `market_state` array instead, filled by one copy at
  MD time;
- the engine must run with **no ledger at all** for the parity tests (`policy.ctx == nullptr`), which
  a `market` that requires an initialised ledger cannot do.

So the split is: `market`/`ledger_bridge` at the host boundary, POD mirror inside the engine, and the
three callbacks as the contract between them. Behaviour 3 is implemented in `strategy::on_market_data`
(predicate: external book **and** price changed) and delegated to the bridge, because the engine
cannot see the ledger itself. Adopting `market` at the boundary later collapses that callback into
`market::update` and shrinks `ledger_bridge` to the key/source table — a pure refactor of the host,
no engine change.

### 4.10 Host: the composition root (`host.{hpp,cpp}`)

The engine is pure arithmetic; `host` is what makes it a running strategy. It owns nothing that
the engine also owns — it *wires* the pieces and implements the two behaviours the engine
deliberately leaves outside itself:

| Host responsibility | Legacy source |
|---|---|
| Event API: `on_market_data` / `on_fill` / `on_hedge_ack` / `on_hedge_done` / `on_timer` | the `RelativeValue` handlers |
| **maxTicksThroughMarket gate** (`quote_policy::validate`) — uncapped lean price vs the traded contract's opposite side, reject on `>= ticks*tickSize` | `validateMaxTicksThroughMarket` :2795-2824 |
| **Disable path** — one violation disables RV *and* external quoting, emits removals, raises the alert | :2871-2880 |
| **Quote lifecycle** — one `quote_request` per changed side, then one notification per contract, `canSendQuotes` deferral | `sendQuoteOrder` :2914-2950 |
| Hedge emission → ledger reservation → ack/complete | `addFAKAdjustment` :2512 + R9 events |
| Rhone clip (`set_max_clip`) — the model ships 0 as "not supplied yet" | `SetRhoneLimits` |

Two properties matter for parity and are easy to get wrong:

- **`market_index` encodes the *quote* side, not the order side.** The host's ack bookkeeping and
  the tests decode it as `contract = mi / 4`, `side = (mi % 4) / 2`, `external = mi % 2`. Using
  `hedge_order::side_` silently targets the wrong book (this is O4 in `PARITY_NOTES`).
- **A deferred emission keeps `has_change`.** While `canSendQuotes()` is false the host returns
  before clearing the flag, so the change is retried next cycle rather than lost.

Allocation: the bridge and the engine are allocated once in the host constructor; the event path
and the cycle perform no heap traffic.

---

## 5. Semantics contract

### 5.1 Behaviours that must be reproduced exactly

| # | Behaviour | Reference |
|---|---|---|
| S1 | `ToRhonePrice` **truncates** `double -> int64` (`+0.5` only under `VIPER_UNIT_TEST`). The new engine keeps truncation but at `PRICE_SCALE = 1e9` (legacy 1e-8) — see PARITY_NOTES F5 | `Common.h:267-273` |
| S2 | `RoundToTickSize` uses C++ `%` on `int64`, with distinct negative-remainder handling per side | `Common.h:1196-1211` |
| S3 | `Round<2>` in `getResiduals`, `Round<4>` in `pickBestHedge` | `:595`, `:2349` |
| S4 | EMA recursion and `std::exp(-alpha)`, same association and operation order | `:1974-1980` |
| S5 | `IsZero`, `IsTicksWorse` (2-tick rule), `0.95 * width` negative-cost clamp | `:2981`, `:2531` |
| S6 | Club aggregation: first lean at the winning rounded price seeds the club, equal rounded prices add | `:190-215` |
| S7 | Hedging walks **only the club**: leans whose rounded price equals the rounded price of the first lean; the walk stops at the first non-club lean | `:2482-2492` |
| S8 | Weighted mid uses `min(unadjustedSize, establishedLevel)` per side | `:2005-2009` |
| S9 | Quote change detection: `abs(dPrice) > priceChangeThreshold` or `dSize != 0` | `:178-184` |
| S10 | **One** hedge contract processed per cycle (`break` after the first request) | `:570` |
| S11 | `getResiduals`: vector index from `_factorHedgedIndexes[c][lean]`, then `vectorPos / vectorRisk / quantityMultiplier` with `Round<2>`; skipped when not `_factorHedged` or `vectorRisk == 0` | `:578-607` |
| S12 | `abs(size) > _rhoneMaxClipSize[c]` → disable RV and abort the hedge loop | `:531-542` |
| S13 | Residual reconciliation: `sameSide` branch, `left`/`hedgedSize` arithmetic, request retained when only partially hedged | `:547-563` |
| S14 | Self-match guard: skip a club member whose hedge price crosses the own quote price or is >= 2 ticks worse; `_skipClubCount >= 50` disables RV | `:2505-2540` |
| S15 | `getOrderSize`: cap by `(size+residuals)*hedgeRatio` and available lean size, `round()` then `toValidQty`, remainder recomputed in hedge terms; `hedgeRatio == 0` decrements `needToHedge` by `quoteSize` and returns 0 | `:2577-2600` |
| S16 | Hedge emission re-sends the contract's blocked quotes with `unblockedByHedge = true` | `:2572-2574` |
| S17 | Quote sending is blocked while any hedge request is outstanding (`_canSendQuotes`) | `:406` |


### 5.2 Deliberate deviations (each needs sign-off — these are *behaviour* changes)

| # | Deviation | Why | Risk |
|---|---|---|---|
| V1 | **Init fails on a singular pricing matrix** (`FullPivLU::isInvertible()`), where ALGLIB returned `info=1` for some rank-deficient matrices and produced garbage of magnitude ~1e15 | Measured: `[[1,2,3],[4,5,6],[7,8,9]]` gives ALGLIB `info=1`, `max|inv|=2.0e15`; Eigen reports rank 2 | A config that "runs" today would refuse to start. Arguably a fix; must be announced |
| V2 | Non-square pricing matrix fails with a precise message instead of `"Matrix is not square"` | Diagnostics only; same outcome (D3) | None |
| V3 | Caps exceeded → init failure | Bounds the hot arrays | Check against the largest production config before rollout |
| V4 | Risk/ODM values differ in the last ~1e-16 relative (ALGLIB vs Eigen LU) | Different LU implementation | Absorbed by S1 truncation in nearly all cases; quantified in §6 |

Nothing else may change. Any other difference is a bug, not a deviation.

---

## 6. Parity and validation strategy (D1, D4)

No recorded production sessions exist (D4), so validation is built from three layers, in this order
of importance.

### 6.1 Layer 1 — reference oracle (hermetic, no data needed) — *primary gate*

`tests/reference_oracle.hpp` is a **literal, deliberately slow transcription of the legacy
formulas** (§2), operating on plain `double`/`int64` arrays, written directly from
`RelativeValue.h:1921-2012, 2687-2735, 2954-3004` with the same operation order and the same
truncation points. ~200 lines, reviewed side-by-side with the legacy code, never optimised.

The differential test drives the oracle and the optimised engine from identical synthetic input and
compares per cycle:

- each `stf[c]` (ticks), each `ltf[m]` (1e-12 relative)
- each quote field: `price`, `size`, `qty`, `lean`, `externalPrice`, `externalSize`

Input generation: deterministic pseudo-random walks over **structurally valid** market states —
prices on real tick grids, crossed/locked markets, `NULL_PRICE` sides, zero sizes, wide spreads,
position extremes, `EMA on/off`, `setToMarket` on/off. Seeds are fixed and recorded, so any failure
is reproducible from seed + cycle index. This is the layer that catches optimisation bugs (magic
divide, CSR leans, precomputed coefficients) with zero infrastructure cost.

Two structural properties are pinned *outside* the differential, because a differential cannot see
something both sides get wrong (PARITY_NOTES O1/O3/O5): `test_priced_and_quoted_overlap` (a contract
in both matrices is one priced+quoted contract, D-V8) and `test_secdb_details_legs` (spread legs come
from secdb's `details` JSON, including the unquoted export form, D-V9).

### 6.2 Layer 2 — generated golden traces (end-to-end, real code paths)

Since we have no recorded sessions, **we generate them from the legacy engine**:

1. Add an instrumentation-only `RV_TRACE` build flag to the legacy `RelativeValue` (compiled out by
   default, no behaviour change) writing one fixed-layout binary record per cycle:
   *inputs* — per contract: internal/external bid/ask price+size, position, flags;
   *outputs* — `ltf[]`, `stf[]`, and per contract per side
   `{price, size, externalPrice, externalSize, lean}`, plus hedge decisions.
2. Drive it with the backtester (`RhoneDsBackTest`, `VIPER_BACKTEST`, `RhoneDS_BackTest.cmake`) on
   `config/backtest` (CME QMQ1…QMX1), and with the simulator for configs that generate internal
   market data.
3. Store traces under `testdata/rv_traces/` (git-ignored or LFS; a 10-contract model at ~10 Hz for
   one trading day is a few MB) with the generating command and config hash in a `MANIFEST`.
4. `tests/trace_diff.cpp` replays a trace through the new engine and reports identical/total per
   field, the first N mismatches with full context, and a per-stage histogram so the responsible
   stage is immediately visible.

Runs in CI on one small fixed trace (sub-minute) and nightly on the full set.

### 6.3 Thresholds (D1)

| Field | Requirement |
|---|---|
| `stf[c]` (ticks) | >= 99.99 % identical per cycle-contract |
| quote `price` / `size` / `qty` / `lean` / external fields | >= 99.99 % identical per cycle-contract-side |
| hedge decisions (contract, side, size, price, external flag, market index) | >= 99.99 % identical |
| hedge-id sequences | same length and ordering (ids are a monotonic counter, compared structurally, not by value) |
| residual values | 1e-12 relative (`Round<2>` applied before comparison, as legacy) |


Every remaining diff must be **explained** in `new_impl/rv/PARITY_NOTES.md`: root cause, evidence
(the trace record plus the divergence), classification (`FMA`, `LU last-bit`,
`documented deviation V1..V4`, `bug fixed`). Unexplained diffs block the phase gate. If the
explained-diff rate exceeds 0.01 % in any field the phase does not pass, and we remove the cause
(e.g. `-ffp-contract=off`, §6.4) rather than widen the threshold.

### 6.4 FMA / AVX2 (D6)

The legacy target compiles `-O3 -march=native -mno-avx -mno-avx2` (`CMakeLists.txt:57-61`) — **no
AVX, no FMA**. The new target uses AVX2 + FMA for the matvecs, which changes the last bit of some
results and can, rarely, flip a truncated tick.

Plan: enable AVX2 + FMA by default, measure the diff rate with §6.1/§6.2, and keep a documented
fallback — `-ffp-contract=off` on `valuation.cpp` only (AVX2 retained, FMA removed) — applied if and
only if the measured diff rate exceeds §6.3.

**Measured so far** (`PARITY_NOTES.md` §1a/§2a): the whole suite built with `-O2` (no AVX,
matching the legacy build) is identical to the oracle. With `-O3 -march=native` **and FMA
contraction enabled** it is not: 3 stf and 6 quote-price diffs per 200 000 cycle-contracts
(99.9985 %, still inside §6.3, but unexplained). Turning contraction off removes **all** of them,
and a per-translation-unit override on `valuation.cpp` is enough — so that override is now shipped
(`new_impl/CMakeLists.txt`) with AVX2 retained; the pricing pipeline cost is unchanged within
noise. This proves compiler sensitivity, not anything about the legacy **binary**, because both
sides of the synthetic gate are compiled with the same flags. That last comparison is exactly
what the layer-2 traces are for.

### 6.5 Numeric kernel validation

`fast_div` (magic-number tick division) and the precomputed coefficient tables are validated
exhaustively, not by sampling:

- `fast_div` vs `%`/`/` for every tick size in use, over the neighbourhood of realistic prices plus
  boundary sweeps (multiples ±0..3 ticks) and negative prices.
- `hedge_scale` / `commission_scaled` / `width_extra` recomputed at test time from the raw
  `ViperConfig` values and compared for **bit equality** with the precomputed tables.

---

## 7. Performance validation

### 7.1 Benchmarks (extend `benchmarks/`, same harness style)

| Benchmark | Measures | Notes |
|---|---|---|
| `rv_pricing_pipeline_bench` | §4.4 stages 2–6 | shapes 4/4/10 (backtest) and 32/32/128 (caps) |
| `rv_quote_stage_bench` | §4.5 full quote stage | reports ns/lean and ns/cycle |
| `rv_cycle_bench` | §4.6 end to end from a synthetic MD stream | p50 / p99 / max, plus cycles/quote |
| `rv_hedge_bench` | §4.7 tiling, sizing, club walk, emission decision | reports ns/hedge and hedging-cycle cost (pipeline twice) |
| `rv_model_init_bench` | Eigen init (risk, ODM) + table build | guards config reload cost |

All benchmarks follow the existing conventions in `benchmarks/README.md`: deterministic input,
warmup, median of N alternating runs, no allocation inside the timed region, and — critically —
report **cycles** as well as ns so results are comparable across machines.

### 7.2 Targets (backtest config: 4 vectors / 4 pricing / 10 contracts, 180 lean-sides)

| Stage | Legacy estimate | Target |
|---|---|---|
| pricing pipeline (§4.4) | ~200 cycles | **<= 100 cycles** |
| quote stage (§4.5) | ~15 k cycles (~5 µs) | **<= 2 k cycles (~0.6 µs)** |
| full cycle (§4.6) | ~5–6 µs | **<= 1 µs p50**, <= 2 µs p99 |
| hedging cycle (§4.7, pipeline runs twice) | ~2x pipeline | **<= 1.4 µs p50**; separate benchmark case |
| single hedge emission (excl. `order_port` call) | — | **<= 500 cycles** |
| model init | ~seconds | <= 10 ms |


At the caps (32/32/128, ~4 k lean-sides) the quote stage is the scaling term: target
<= 25 k cycles. Record the measured numbers in `benchmarks/README.md` next to the existing table.

Regression policy: benchmarks are run manually before/after each phase and the numbers pasted into
the PR; a > 10 % regression versus the stored baseline blocks the phase.

### 7.3 Zero-allocation assertion

Debug/CI builds install a counting `operator new`/`delete` (or a `malloc` hook) that hard-fails if a
cycle allocates. `run_cycle` and everything it calls must be allocation-free; the test drives
100k synthetic cycles with assertions enabled. This also keeps `std::pmr`/`boost::small_vector`
temptations out of the hot path.

### 7.4 Structure must not cost instructions (callgrind gate)

Wall-clock on a shared dev box is ±10 % run to run, which is wider than any regression the object
model could introduce -- the refactor in §4.1b was first read as "+10 %" and turned out to be noise.
So structural changes are validated on **retired instructions** instead, which are deterministic:

```
valgrind --tool=callgrind --callgrind-out-file=cg.out ./rv_cycle_bench 2000 1
callgrind_annotate --threshold=99 cg.out        # compare per-function Ir with the baseline
```

Baseline for 2000 cycles (4/4/10 synthetic model, quote stage quoted with 6 traded contracts):

| Function | free functions | object model |
|---|---|---|
| quote build (`compute_quote` / `quote_engine::build`) | 45.78 M | 45.75 M |
| pricing pipeline (`compute_fairs` / `valuation::recompute`) | 3.11 M | 3.11 M |
| **program total** | **70.24 M** | **71.44 M** |

A structural change that moves the total by more than ~2 % needs an explanation in the commit; the
remaining +1.7 % sits in `cycle_state::best_is_external` call accounting (identical 34-instruction
body, more call sites recorded by the profiler) and does not reproduce in the wall clock (1964 ns vs
1955-1968 ns full cycle, same machine).

---

## 8. Build and integration

`new_impl/CMakeLists.txt` builds the library, the tests, the benchmark and the replay driver
standalone (C++20, AVX2, system or vcpkg Eigen). The shape is:

```cmake
# new_impl/CMakeLists.txt (new, standalone; added via add_subdirectory or built on its own)
add_library(viper_new_impl STATIC
    common/liquidity_ledger.cpp common/market.cpp
    rv/model.cpp rv/state.cpp rv/valuation.cpp rv/hedger.cpp rv/quotes.cpp rv/strategy.cpp
    rv/config.cpp rv/config_model.cpp)
target_compile_features(viper_new_impl PRIVATE cxx_std_20)
find_package(Eigen3 CONFIG REQUIRED)
target_link_libraries(viper_new_impl PUBLIC Eigen3::Eigen)
target_include_directories(viper_new_impl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(viper_new_impl PRIVATE
    -O3 -march=skylake -fno-math-errno -Wall -Wextra)   # NOT -mno-avx*, NOT -ffast-math
```

Points to settle at P0:

- `new_impl` uses `std::span`/`concepts` (`requires` in `types.hpp`) → **C++20**, while the legacy
  target is C++17 (`CMakeLists.txt:4`). Keep the new target C++20 and the legacy one untouched.
- `tachyon/core/common/types.hpp` in `new_impl/` shadows the real framework header; the deeper
  stubs it needs (`tachyon/core/om/order_types.hpp`, `zenith/types/Endpoint.hpp`) live in
  `new_impl/stubs/`, which is now the default `VIPER_RV_STUBS_DIR` — **done**: the tree builds
  from a fresh clone with no manual setup, and pointing `VIPER_RV_STUBS_DIR` at the real framework
  headers replaces them without touching the sources.
- **`vcpkg.json` is an empty (0-byte) file**, which breaks manifest mode
  (`build/vcpkg-manifest-install.log`: *"Failed to load manifest … Unexpected EOF"*). Either
  populate it with `eigen3` or document `-DVCPKG_MANIFEST_MODE=OFF` + system Eigen. The benchmarks
  README already documents the intended vcpkg invocation. `new_impl/CMakeLists.txt` accepts either
  a real `Eigen3::Eigen` package or `-DEIGEN3_INCLUDE_DIR=<headers>`.
- Production/Linux is GCC; the new target must build clean with `-std=c++20 -O3 -march=skylake`
  under GCC 11+ *and* clang (dev machines). **Confirmed on GCC 11.5 / Linux (x86-64): 0 errors,
  0 warnings with `-Wall -Wextra`.**

---

## 9. Phases

| Phase | Deliverable | Acceptance | Est. |
|---|---|---|---|
| **P0** — scaffold | `limits.hpp`, `types.hpp` (+ `fast_div` with exhaustive self-test), `model` init from `ViperConfig`, CMake target, oracle skeleton | model matches legacy `risk`, `ODM`, `hedgeRatio`, `quoteSize`, `width` to 1e-12 (tables bit-exact); `fast_div` equals `%` on all cases | 2–3 d |
| **P1** — pricing | §4.4 pipeline, all three `stf` modes, EMA | oracle diff: `ltf`/`stf` >= 99.99 % identical over 1M synthetic cycles × 3 modes; pipeline <= 100 cycles | 3 d |
| **P2** — quotes | §4.5 quote stage, CSR leans, coefficient tables, magic divide, change detection | quote parity >= 99.99 % both vs oracle and vs generated traces; full cycle <= 1 µs p50 at backtest config; zero allocations | 4–5 d |
| **P3** — hedges (decision + send) | §4.7: residuals, tiling, sizing, club walk, self-match guard, `order_port` emission, blocked-quote re-send, position/vector bookkeeping | hedge parity >= 99.99 % vs oracle on synthetic fill streams (incl. S10–S17); `rv_hedge_bench` within budget; ledger tests from `build/new_impl_test.cpp` extended | 4–5 d |
| **P4** — hardening | dirty tracking, fast exit, SIM/backtest trace run, `PARITY_NOTES.md` complete, ASAN/UBSAN clean | 0 unexplained diffs; p99 within budget; docs complete | 3 d |
| **P5** — shadow & cutover | new engine computes quotes **and hedges** into a side channel while legacy trades; compare live | N days zero unexplained divergence; rollback plan written | open |

P3 is on the v1 critical path (D7). The `order_port` seam keeps `OrderRequestManager`/`Executor`
untouched, so the transport risk stays contained; what P3 owns is the *decision and sizing* code.

Definition of done for v1: **P0–P4 green**, i.e. parity gates met on both layers, benchmarks at
target, `PARITY_NOTES.md` with every diff classified, and the shadow run signed off.

---

## 10. Risks and open questions

| # | Risk / question | Impact | Proposed handling |
|---|---|---|---|
| R1 | ~~Hedge *sending* path not scoped~~ — **resolved by D7**: in v1, decision + sizing + emission | Larger v1 (P3 is 4–5 d and on the critical path) | Keep the logic out of the transport: `order_port` only carries already-decided orders |
| R2 | ~~Threading / cadence contract unknown~~ — **resolved by D8**: single-threaded, cycle-driven | Cadence comes from the host calling `run_cycle(timestamp_t)` | Cycle must stay allocation-, lock- and syscall-free so it can run on the strategy thread |
| R3 | ~~Non-square configs exist in the repo~~ — **audited: resolved**. All 11 `model*.csv` files in `config/` parse and are square (2/2, 3/3, 4/4, 16/16). Note `test/Fixtures/unit_test_config.csv` (29 vectors / 14 pricing) is not a model file and is not parsed by the engine | None for real configs | `rv_tests::test_all_configs` re-runs the audit; the parser is header-driven because column order differs between configs (`2msgw` has no `EMAOnTime`/`EMAOffTime`) |
| R4 | Benchmark numbers from dev machines are noisy (measured 6x-19x inflated on a loaded Windows box during the ALGLIB/Eigen comparison) | Targets may look unreachable or too easy | Report cycles, benchmark on the production-class CPU before signing off |
| R5 | Generated golden traces only cover the paths the backtester/simulator exercise (no live IME behaviour, no venue quirks) | Parity on rare live states is unproven | Mitigated by the oracle layer (§6.1) covering adversarial synthetic states + P5 shadow run |
| R6 | `-ffast-math` / FMA drift accumulating through EMA state | Diff rate could creep above 0.01 % over long sessions | EMA is `double` state carried across cycles; trace set includes multi-hour runs; fallback `-ffp-contract=off` |
| R7 | `new_impl` depends on a vendored `tachyon` header namespace that will be replaced | Rework when the real framework lands | Keep `config` and the cycle entry points as the only coupling points; stub include path is removable |
| R8 | Reviewer bandwidth: the oracle must be reviewed line-by-line against legacy | A wrong oracle silently blesses a wrong implementation | Oracle reviewed by someone other than the implementer; oracle diff failures always reported with both value sets |
| R9 | **Hedge lifecycle events** must reach the new engine once sending is owned (D7): FAK done (`:1813`), internal hedge done (`:778`), MD-adjustment ack (`:786`) | A missed ack leaks a reservation and permanently shrinks displayed size in the ledger | `order_port`/input must deliver all three; `liquidity_ledger` already models them (`acknowledge_external`, `complete_by_hedge_id`), so this is wiring + tests |
| R10 | Single-threaded (D8) requires the host to serialise MD, internal fills and acks into one input sequence | If two queues feed the strategy today, the adapter must merge them; ordering changes could alter decisions | Document the expected input order in the adapter; parity harness feeds events in the legacy order |


---

## 11. Appendix

### 11.1 Legacy line map (for the implementer)

| Concern | Legacy location |
|---|---|
| Model init / risk / ODM / tables | `:2015-2262` (`initializeModel` … `calculateVectorRiskExposureForContract`) |
| Quote size / normalisation / commissions | `:2353-2458` |
| Hedge selection (`pickBestHedge`) | `:2322-2350` |
| Mid-points | `:1921-2012` |
| Pricing pipeline | `:1931-1996` |
| STF (vectors, pricing contracts, spreads) | `:2687-2735` |
| Quote construction | `:2825-2900`, `:123-297`, `:2954-3004` |
| Hedge sending | `:2466-2575` (`sendHedges`, `getOrderSize`, `sendHedgeOrder`) |
| Hedge requests / residuals | `:514-607` (`processHedgeRequests`, `getResiduals`) |
| Position / vector bookkeeping | `:2602-2637` (`addPosition`, `addPositionToVector`) |
| Hedge lifecycle events | `:778-792` (internal done, MD-adjust ack), `:1813` (FAK done) |
| Cycle | `:387-435` |

### 11.2 Existing assets to reuse

| Asset | Use |
|---|---|
| `benchmarks/RelativeValueVectorMarketPriceBenchmark.cpp` | harness style + the Eigen matvec pattern for the `P · (pricing ⊙ mult)` kernel |
| `new_impl/common/{market,liquidity_ledger}` | market state + reservation accounting for the hedge path |
| `new_impl/common/types.hpp` | `market_key`, `strategy_id`, `liquidity_source`, side constants |
| `build/new_impl_test.cpp` | plain-`assert` test style (no framework added to `new_impl`) |
| `RhoneDsBackTest` + `config/backtest` | golden-trace generation |
| `test/Fixtures/MathStructuresTests.cpp` | existing matrix test to extend if `MathStructures` remains in the legacy path |

### 11.3 Repository notes found while preparing this plan

- `src/math/` (ALGLIB, 308 k lines, 11 MB) is used by exactly **one** call — `Matrix::inverse()` in
  `MathStructures.h:149-175` via `alglib::rmatrixinverse`. The new impl does not touch it; replacing
  it with Eigen is an independent, low-risk cleanup (Eigen `FullPivLU` matched ALGLIB to ~1e-16 and
  was ~7x faster on the same work).
- `Python::Matrix` has no destructor and `removeMemory()` is commented out → every copy leaks. Not
  inherited by `new_impl`.
- `Matrix::inverse()` requires a square matrix, which is the origin of the D3 constraint.

---

## 12. Implementation status (P0–P3 landed)

| Task | State |
|---|---|
| Golden traces from `RhoneDsBackTest` (§6.2) + `trace_diff` | **not started** — needs a Linux/legacy build. Blocks F2/F4 closure, the FMA-vs-legacy-binary question, and P5 |
| Production-hardware benchmark (§7.2) | **not run** — numbers are from a loaded dev box (advisory budget miss; model init 30 ms vs 10 ms target) |
| P4: ASAN/UBSAN, SIM trace | **not run** — no sanitizer support in this MSYS/clang setup |
| §7.1 benchmarks | **done in one binary** — full cycle, pricing pipeline, quote stage, **model init (config → model)**, plus the zero-allocation assert. Separate binaries not split out |
| §3.2 #9: position update as an Eigen matvec | **open** — still element-wise loops (fill-rate path, low value) |
| §6.5 commission/width bit-equality vs legacy | **partial** — fast_div and hedge-cost sweeps are exhaustive; commission/width tables are spot-checked against known values (real legacy comparison needs the legacy build) |
| Runtime inputs: `lot_size`, `venue` | **done** — from secdb `order_increment_qty` / `activate`, spreads inherit from first leg |
| Runtime input: `rhone_max_clip` | **faithful, needs host wiring** — defaults to 0 like legacy, so a fill before `SetRhoneLimits` disables RV (S12). `strategy::set_max_clip` is the wiring point; covered by `test_hedge_path` |
| Legacy spin-off: `RoundToTickSize` fast path | **done in `src/Common.h`** — `RoundToTickSizeConstant<Tick>` template + a frequency-ordered switch over the 14 tick sizes covering ~99% of instruments (fallback = original code). Benefits **MM (6 call sites), AS (1) and legacy RV (2)** with zero call-site changes. Verified: 58,024-case bit-equivalence vs the original (`build/round_to_tick_check.cpp`), no `idiv` in the generated code for dispatched ticks, and 1.23x / 0.66 ns saved per rounding (`build/tick_round_bench.cpp`). Not compiled here (Common.h needs the Rhone framework) — the legacy build must confirm integration |
| Multi-leg (>2) spreads | **works by construction** (alternating ±1) but no real config exercises it; unverified against legacy |
| Price scale | **1e9** (`PRICE_SCALE`, asserted equal to `tachyon::DEFAULT_FIXED_PRICE_MULTIPLIER`). Legacy was 1e-8; the difference and its bounds are recorded as F5 in `PARITY_NOTES.md`, and `test_price_scale` pins the invariants |
| Spread legs: name-derived vs secdb `details` | **done** — legs now come from secdb's authoritative `details` JSON (`{"legs":[{"sym":"6JZ9","qty":1},{"sym":"6JV9","qty":-1}]}`), with the name splitter as a fallback that must agree (a disagreement is an init failure, D-V9). The lookup tolerates ICE's trailing `!` and EUREX's `efFGBMU1:Z1` shorthand, and the secdb reader is quote-aware so the `details` commas cannot shift later columns |
| Pricing+quoting overlap (a contract in both PricingMatrix and RVConfigs) | **done** — the contract list is merged and `traded[]` is a flag rather than implied by `index >= first_traded` (D-V8); the `first_traded`-based quote, host-emission and differential loops are now traded scans. `config/2msgw` builds and replays |
| `market` / `liquidity_ledger` integration | **ledger done, `market` not adopted.** The engine keeps a POD `market_state` mirror in `cycle_state.markets[]` and reaches the ledger through three hooks (`adjusted_size`, `price_changed`, `send_hedge`) plus ack/complete. `common::market` (price/size + source + ledger pointer) stays at the host boundary. Rationale in §12.4 |
| `vcpkg.json` (0 bytes), CMake target never configured, no GCC build | **open** — infra only |

### 12.1 Measured parity (layer 1, `rv_tests`)

`rv_tests 5000` and `rv_tests 20000`, synthetic books on real tick grids, 4 vectors /
4 pricing / 10 contracts, all three `stf` modes exercised (EMA on, `setToMarket`, plain):

```
mid          : 100.00000% identical
stf          : 100.00000% identical
quote price  : 100.00000% identical
quote size   : 100.00000% identical
best lean    : 100.00000% identical
ext price    : 100.00000% identical
```

Every compared field matched **exactly** (0 mismatches across 200 k+ comparisons), i.e. the
99.99 % gate in §6.3 is met with zero unexplained diffs. Two test-only exclusions, both
documented in the source:

1. Cycles where a pricing contract has no two-sided market are excluded from the differential
   and covered by `test_missing_side()` instead: legacy's midpoint becomes
   `to_dollars(NULL_PRICE) ≈ 9.2e10`, garbage that parks in the stateful EMA and turns a 1-ulp
   difference into a few "ticks" at 1e16 magnitude.
2. `fast_div` is validated against hardware `%`/`/` on 40,005,810 cases (10 divisors incl.
   powers of two, negatives, boundaries, ±2^52).
3. The hedge-cost scaling sweep covers 10,547,200 multiplier/cost/ratio combinations and
   requires **zero** mismatches (§3.1) — the precomputed single-factor form was rejected by
   this gate, and only the provably-identical unity case uses the fast path.

### 12.2 Measured performance (`rv_cycle_bench`, 100 k cycles × 5 runs)

```
full cycle            1906 ns   (~5700 cycles @3GHz)   budget 1000 ns  <-- miss on this box
  pricing pipeline     244 ns   (~730 cycles)          budget  100 cycles <-- miss on this box
  quote stage         1638 ns   (~4900 cycles)         budget 2000 cycles
allocations              0 per cycle
lean-sides/cycle      1200 (dense 10x10 lean matrix = worst case)
```

The quote stage is ~77 % of the cycle and costs **~1.3 ns (≈4 cycles) per lean-side**. Budgets
are marked advisory in the benchmark because this dev box measured 6–19x inflation on unrelated
microbenchmarks; the hard invariant (zero allocations per cycle) holds. Re-measure on the
production CPU before signing off, and note that real configs have a sparse lean matrix, so the
lean-side count — the only term that scales — will be far below the 1200 measured here.

### 12.3 Constant tick dispatch in `round_to_tick` (measured A/B)

`rv::round_to_tick` now dispatches on `fd.divisor` to a compile-time-constant divider (same
technique as the legacy `Common.h` change) and falls back to `floor_divmod` for anything else. The
list is written in terms of `PRICE_SCALE` so it cannot drift if the scale changes again, and it is
guarded by `RV_CONSTANT_TICK_DISPATCH` so the choice stays measurable.

Alternating runs of two otherwise identical binaries (`-DRV_CONSTANT_TICK_DISPATCH=1|0`, 30 k
cycles × 3 runs each, 3 pairs):

| pair | dispatch ON (quote / full) | dispatch OFF (quote / full) |
|---|---|---|
| 1 | 1793.0 / 2014.4 ns | 1850.2 / 2261.9 ns |
| 2 | 1792.2 / 2049.1 ns | 1841.6 / 2152.1 ns |
| 3 | 1792.0 / 2022.6 ns | 1853.9 / 2135.6 ns |

**~3 % off the quote stage (≈56 ns/cycle), ~100–250 ns off the full cycle.** Note the isolated
microbenchmark predicted 29 % for this change; in situ only ~3 % materialises, because the rounding
interleaves with the rest of the lean loop (the whole lean-side costs ~1.5 ns). Worth keeping, but
it is a reminder that isolated micro-benchmarks overstate marginal cost.

Equivalence: `rv_tests::test_fast_div` covers both paths (dispatched `0.001/0.025/0.0025` and
fallback divisors) against the legacy transcription over 40 M cases, and the differential stays at
100.00000 % with the dispatch on.

### 12.4 `market` / `liquidity_ledger` — status against §4.9

Design is in **§4.9**; this is only the implementation status of the four required behaviours.

| §4.9 behaviour | Status |
|---|---|
| 1. Adjusted size (`quote_policy::adjusted_size`) | **done** — `ledger_bridge::available` → `liquidity_ledger::available` with the source filter; `test_ledger_bridge` (5→2→5) |
| 2. Source scoping | **done** — the bridge passes `slot.source`; `build/new_impl_test.cpp` covers the ledger's own scoping |
| 3. Flush on price change (`quote_policy::price_changed`) | **done** — external books only, added after this gap was found; `test_reservation_invalidation` (reserve → size-only keeps → price change flushes → internal never flushes) |
| 4. Hedge lifecycle | **done** — `reserve_hedge` on emission, `acknowledge_external` / `complete_by_hedge_id` exposed by the bridge; wiring of the three inbound events is host-side (R9) |

**Bug this gap caused:** while the engine bypassed `common::market` it also bypassed behaviour 3
entirely, so a hedge reservation survived a re-price and kept that book's quotes under-sized
indefinitely. Fixed by the `price_changed` callback; recorded in `PARITY_NOTES.md` §4.

### 12.5 Host + driver — status (§4.10)

| Piece | Status |
|---|---|
| `host` composition root (model + state + ledger + bridge + strategy) | **done**, allocation-free per event/cycle |
| Event API (`on_market_data`, `on_fill`, `on_hedge_ack`, `on_hedge_done`, `on_timer`) | **done** |
| maxTicksThroughMarket gate + disable path | **done** — required adding `max_ticks_through_market` to the model (the field was parsed from CSV but never propagated) and `lean_price_uncapped()` for the uncapped variant |
| Quote lifecycle (`quote_request`, notification, `canSendQuotes` deferral) | **done** |
| Hedge → reservation → ack/complete round trip | **done** |
| Rhone clip (`set_max_clip`) | **done** — without it the first fill disabled the strategy (model ships 0) |
| `rv_host_test` | 19 checks, all passing |
| `rv_replay` (runnable, real configs) | passes end-to-end on 7 of the 8 config directories (see the coverage table below; `minimal_eurex` needs refreshed reference data) |

`rv_replay` on `config/minimal_cme` (the honest "it composes" number):

```
cycles=500  md_updates=1222  fills=36  hedge_acks=21
quotes_emitted=534  notifications=365
hedges_sent=21  reservations_taken=21  active_reservations=0
validation_failures=0  disabled=no            -> PASS (0 failures)
```

**Config coverage (known, not silent):** 7 of 8 config directories run end-to-end today.

| Config | Result | Cause |
|---|---|---|
| `minimal_cme`, `minimal_cme_2`, `minimal_cme_3` | PASS | — |
| `2msgw` | PASS | was the pricing+quoting overlap; fixed by D-V8. Runs to 23 quote emissions, 1 fill, 1 hedge before the synthetic tape trips `maxTicksThroughMarket` (GEH1 106.77 vs ZTH1 28.25 leaned 1:1 with a 3-tick gate: the gate is doing its job on a tape that is not cointegrated, and legacy would disable there too) |
| `minimal`, `minimal_ice_2`, `cross_minimal` | PASS | were the dated ICE legs; fixed by D-V9 (806 / 533 / 894 quotes emitted per 500 cycles) |
| `minimal_eurex` | build error | **stale config, not an engine gap**: the model trades `efFGBMU1`/`efFGBMH2` (Sep-21, Mar-22) while `secdb.eurex.csv` only knows `efFGBMZ0`/`H1`/`M1` (Dec-20 … Jun-21) — no `details` and no tick for any traded symbol. `update_eurex_secdb.sh` refreshes it from the internal refdata service, which is unreachable here; the error names the missing symbol |
| `backtest` | runs, then saturates | its `ltf` column holds out-of-range values (see PARITY_NOTES F6) |

**Remaining work to close the coverage gap:**

1. ~~Pricing+quoting overlap (`2msgw`)~~ — **done** (D-V8, `rv_tests::test_priced_and_quoted_overlap`).
2. ~~Spread legs from secdb `details`~~ — **done** (D-V9, `rv_tests::test_secdb_details_legs`).
3. `minimal_eurex` — refresh `secdb.eurex.csv` from refdata so the model's Sep-21/Mar-22 contracts
   exist, then re-run. Nothing to change in the engine: the failure is a missing reference row.

4. **Golden traces** (unchanged, still needs a Linux/legacy build): `RV_TRACE` in the legacy
   `RelativeValue` + `RhodesDsBackTest` on `config/backtest`, then `tests/trace_diff.cpp`. That also
   settles F2/F4 and the FMA question, and covers the `backtest` ltf saturation against the real
   legacy numbers.

