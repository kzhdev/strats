# RV parity notes

Every difference between the new engine (`new_impl/rv`) and the legacy strategy
(`src/RelativeValue.h`) that the parity gates can see, with its classification and evidence.
Required by PLAN.md §6.3: no unexplained diffs, and any change to these numbers needs a gate
re-run.

Legend: **D** = deliberate deviation (PLAN §5.2), **F** = floating-point association,
**O** = test-side defect.

---

## 1. Current gate status

`rv_tests 20000` (4 vectors / 4 pricing / 10 contracts, books on real tick grids, every
contract quoted — including the four pricing columns, so the priced+quoted overlap is inside
the differential too):

| Field | Identical | Mismatched |
|---|---|---|
| mid | 100.00000 % | 0 / 200 000 |
| stf (ticks) | 100.00000 % | 0 / 200 000 |
| quote price | 100.00000 % | 0 / 400 000 |
| quote size | 100.00000 % | 0 / 400 000 |
| best lean | 100.00000 % | 0 / 400 000 |
| external price | 100.00000 % | 0 / 200 000 |
| hedge cost (10.5 M multiplier/cost/ratio combos) | 100.00000 % | 0 |
| tick division (40 M cases) | 100.00000 % | 0 |

Zero unexplained diffs. The sections below record the *classes* of difference that were found
and removed, the deviations that were chosen, and the legacy behaviour that is preserved
deliberately.

## 1a. FMA contraction: measured, and the §6.4 fallback applied

Same suite, same machine (x86-64, GCC 11.5, `-O3 -march=native`), only the floating-point
contraction differs:

| Build | stf | quote price | full cycle | pricing pipeline |
|---|---|---|---|---|
| FMA contraction on (default) | 3 / 200 000 (99.99850 %) | 6 / 400 000 (99.99850 %) | 1969 ns | 260 ns |
| `-ffp-contract=off` on `valuation.cpp` **only** (shipped) | **0 / 200 000** | **0 / 400 000** | 1951–1968 ns | 254–289 ns |

The three stf diffs were all ±1 unit at 1e-9 ($1e-17) in the same cycle, i.e. a contraction-induced
last-bit flip before the `int64` truncation; the quote-price diffs were the same cycle's
downstream propagation. PLAN §6.4's fallback is therefore in force: AVX2 stays on, the fused
multiply-add is disabled for `valuation.cpp`, and the whole suite is bit-identical to the
scalar-ordered oracle. The pricing pipeline cost is unchanged within run-to-run noise
(±13 %), so nothing is traded away for it.

Note what this does *not* prove: both sides of the synthetic gate are compiled from the same
source with the same flags, so this bounds compiler-induced divergence, not divergence from
the legacy **binary**. That is the layer-2 trace gate's job (§6.2, still pending a
Linux/legacy build).

## 2. Floating-point association — found and avoided (F)

**F1. Hedge-cost multiplier scaling.** The plan's original optimisation folded
`leanPm/contractPm * leanQm/contractQm` into one precomputed factor per `(contract, lean)`.
Measured against the legacy four-division expression (`:2978`): **0.5 % (power-of-ten
multipliers) to 0.7 % (arbitrary multipliers) tick mismatch rate** — the association changes
the last bit before the `int64` truncation.

Resolution: the single-multiply path is used **only** when all four multipliers are exactly
`1.0`, where it is bit-identical by construction; otherwise the legacy expression order runs
inline. `rv_tests::test_hedge_cost_scaling` sweeps 10 547 200 combinations and requires zero
mismatches. Cost of the correction: 2–4 divisions per lean pair for non-unit multipliers.

**F2. `stf` division placement.** Legacy divides by `pricingMultiplier` *inside* the
`stf_vec · risk` accumulation; the engine divides once after the matvec. Identical for unit
multipliers (the whole fixture) and it is what allows a single Eigen matvec. Any config with
non-unit pricing multipliers must be diffed with the layer-2 trace gate (PLAN §6.2) before
cutover.

**F5. Price scale 1e9 (new) vs 1e-8 (legacy).** Requested change: `PRICE_SCALE` is now
`1'000'000'000`, matching `tachyon::DEFAULT_FIXED_PRICE_MULTIPLIER` (CME exponent −9), while the
legacy `hf::price_t` / `ToRhonePrice` scale is 1e-8.

Consequences, all bounded and explainable:

- The internal price grid is 10× finer. `to_rhone_price` still **truncates** (S1), so an
  intermediate fair value now keeps one more decade than legacy did: `stf` can differ from the
  legacy value by up to 1e-8 dollars *before* rounding. Everything downstream is rounded to the
  tick (`RoundToTickSize`), and every real tick (0.025 / 0.25 / 0.01 / 0.005) is an exact multiple
  of both 1e-8 and 1e-9, so quotes are unaffected except in the vanishing case where a value sits
  within 1e-8 of a tick boundary.
- Config values are in dollars (`RVWidth`, tick from secdb), so they scale automatically; nothing
  in the config parser carries a hard-coded scale.
- `fast_div`'s double-reciprocal fast path covers |price| < 2^52, i.e. $4.5m at this scale
  (was $45m at 1e-8). Above that it falls back to hardware division, so no correctness change.
- **Finest-tick instruments are where the change actually matters.** The smallest tick anywhere
  in `config/` is 1e-7 (CME 6J Japanese-Yen calendars in `config/minimal_cme/secdb.cme.all.csv`;
  86 `6J…` rows, outright tick 5e-7 at a price of ~0.0070). 1e-7 is exact on both grids (10 units
  at 1e-8, 100 at 1e9), but 1e9 *also* represents ticks below 1e-8 — a 5e-9 tick is 5 units at
  1e9 and rounds to 0 at 1e-8, which would fail init with "tick size must be positive". Prices in
  this class are tiny (7e6 units; a calendar at 5e3), i.e. 6e8× and 9e11× inside the fast path.
  Covered by `rv_tests::test_jpx_magnitude` (Nikkei / TOPIX / JGB / 6J outright / 6J calendar).
- Note a spread's tick is **not** its legs' tick (6J calendar 1e-7 vs outright 5e-7). The parser
  reads the spread's own secdb row, so this is correct today; deriving a tick from the legs is
  only a fallback for symbols absent from secdb.
- The oracle uses the same `PRICE_SCALE`, so the differential gate remains an apples-to-apples
  comparison; this difference is only visible against the **legacy binary**, which is exactly what
  the layer-2 traces in PLAN §6.2 are for. Recorded here so it is classified before, not after.

`rv_tests::test_price_scale` pins the invariants: `to_rhone_price($1) == PRICE_SCALE`, 1e-9
resolution, truncation (not rounding), round-trip through `to_dollars`, and `round_to_tick`
floor/ceil behaviour at the new granularity.

**F6. Range loss at 1e9: `ltf`/`stf` can overflow int64 (found by driving a real config).**
`config/backtest/model.csv` carries `ltf` values of `-2838568772.11` and `5677137518.51`. Legacy
treats those as dollars (`calculateShortTermFairs` → `ToRhonePrice`, :2690-2705), so at its 1e-8
scale the fair is `-2.8e17` units — nonsense economically, but comfortably inside `int64`.
At 1e9 the same fair is `-2.8e18`, and a spread's fair (`stf[leg]·(+1) + stf[leg]·(−1)`) then
exceeds `int64`, which is signed-overflow UB.

Resolution (engine, not config-specific):
- `to_rhone_price_saturated` / `saturate_price` clamp to ±9.2e18 (and map NaN → 0) instead of
  overflowing. In-range values are **bit-identical** to a plain cast, so the differential gate is
  unaffected (re-verified: 100.00000 % at 20 000 cycles).
- The spread sum in `compute_fairs` stage 6 accumulates in `__int128` before clamping, because two
  saturated legs cannot be added in 64 bits.
- Consequence: with `config/backtest` the fairs saturate and the `maxTicksThroughMarket` gate then
  disables quoting — which is the *correct* response to a config whose fairs are unsane, and is
  what `rv_replay config/backtest` reports. `minimal_cme*` (sane `ltf`) run clean.

This is the one place where "no `-ffast-math`" and "saturate, never rely on wrap-around" matter
for correctness rather than parity.

**F3. Risk matrix last bits.** `Eigen::FullPivLU` inverse vs ALGLIB `rmatrixinverse`: ~1e-16
relative (measured on these matrices). Absorbed by the price truncation in `ToRhonePrice` /
`to_rhone_price` in every tested state.


**F2/F4 status: unverified, tracked.** See F2 above — the engine divides once after the
`stf_vec · risk` matvec, legacy divides per term, and that is only exercised by unit multipliers
today. Closing it needs a config with non-unit `pricingMultiplier`, i.e. the layer-2 traces.

## 2a. Compiler-flag evidence (PLAN §6.4, D6)

The suite was built four ways and compared (the first two on the original dev box, the last two
here — see §1a for the numbers):

| Flags | Result |
|---|---|
| `-O2` (no AVX, no FMA — matches the legacy build's `-mno-avx`) | 100 % identical, 0 failures |
| `-O3 -march=native` (AVX2 + FMA, no contraction override) | 99.9985 % stf / quote price, 0 gate failures |
| `-O3 -march=native -ffp-contract=off` (whole library) | 100 % identical, 0 failures |
| `-O3 -march=native` with `-ffp-contract=off` on `valuation.cpp` only (**shipped**) | 100 % identical, 0 failures |

So the engine is insensitive to AVX2, and sensitive only to FMA *contraction* in the pricing
pipeline — which is now disabled for that one translation unit (§1a).

**What this does not prove:** the oracle is compiled with the *same* flags as the engine, so both
sides move together under any flag. A flag-induced difference against the **legacy binary** (GCC,
`-mno-avx`) is invisible to this gate by construction, and still requires the layer-2 trace
comparison (PLAN §6.2) against numbers captured from the legacy build. That is the one class of
difference the synthetic layer structurally cannot see.

## 3. Deliberate deviations (D)

| # | Deviation | Why |
|---|---|---|
| D-V1 | Singular pricing matrix → init failure | ALGLIB returned `info=1` plus a risk matrix of magnitude ~1e15 for some rank-deficient inputs (measured: `[[1,2,3],[4,5,6],[7,8,9]]`). Refusing to start is correct for a config that cannot price. |
| D-V2 | Non-square pricing matrix → precise error | legacy threw `Matrix is not square` from `MathStructures.h:151`; same outcome, actionable message (PLAN D3). |
| D-V3 | Caps (`MAX_CONTRACTS`/`MAX_PRICING`/`MAX_LEANS`) → init failure | bounds the static hot arrays (PLAN §4.2). |
| D-V4 | Hedge requests processed FIFO | legacy iterated `std::unordered_map<uint32_t,double>` (`:3148`), order unspecified. "One contract per cycle" (S10) is preserved. |
| D-V5 | Lean ties broken by lowest lean index | legacy sorted by price only (`std::sort`, `:158-162`); tie order was implementation-defined. Affects `best lean`/`best size` only when two leans compute the *same* price. |
| D-V6 | Position update skips contracts outside the pricing set | legacy `addPositionToVector` (`:2629`) indexes `_riskMatrix` by contract id and reads past the matrix for a non-pricing outright (latent out-of-bounds). |
| D-V7 | `_canSendQuotes` blocking and blocked-quote re-send are callbacks (`order_port`), not engine state | keeps the transport out of the engine (PLAN §4.7). |
| D-V8 | A contract listed in **both** the PricingMatrix and RVConfigs is one contract (`traded[]` flag), not two | legacy `ViperConfig` merges the two rows into a single priced+quoted contract; `config/2msgw` depends on it. `first_traded` survives only as the boundary of the pricing columns, so every "is this contract traded" test reads `model::traded[]`. `rv_tests::test_priced_and_quoted_overlap` pins the layout and the quoting path. |
| D-V9 | Spread legs come from secdb's `details` JSON; the name splitter is the fallback and must agree | secdb is what the exchange publishes (`{"legs":[{"sym":"BRN FMQ0022!","qty":1}, ...]}`). The splitter cannot resolve ICE's `!` suffix (`config/minimal`), EUREX's `efFGBMU1:Z1` shorthand, or any ratio ≠ 1. When both are available and disagree, init fails rather than picking one. `rv_tests::test_secdb_details_legs` covers the quoted and the unquoted export forms. |

## 4. Legacy behaviours deliberately *preserved* (not "fixed")

Reproduced on purpose — changing any of these changes trading decisions:

- `ToRhonePrice` **truncates**; the `+0.5` variant exists only under `VIPER_UNIT_TEST` (`Common.h:267`).
- `RoundToTickSize` with negative prices (bids floor, asks ceil, negative-remainder branches).
- Per-leg truncation when accumulating spread fair values (`:2717-2735`).
- EMA recursion and operation order, including the `isfinite` guard (`:1974-1984`).
- `_bestPrice` is the **unrounded** lean price; `_externalPrice` is rounded to tick.
- Club aggregation, including "stop at the first different rounded price".
- Hedging walks only the club; at most one hedge contract per cycle; the 2-tick self-match
  guard with the 50-skip fail-safe; hedge-id stays a monotonic counter.
- A missing market side produces a garbage midpoint (`to_dollars(NULL_PRICE) ≈ 9.2e10`) that
  flows into fair values — legacy behaviour, covered by `rv_tests::test_missing_side`.
- `rhone_max_clip` defaults to **0** (the value `_rhoneMaxClipSize` has until `SetRhoneLimits`
  arrives), so a fill before limits disables RV via S12. `test_hedge_path` pins both branches:
  clip 0 → disable, clip 100 → hedge emitted (`contract=0 side=ask size=1 id=1`).
- **Reservations are dropped when a displayed price level is replaced** — legacy
  `Market::update` flushes its FAK adjustments on a price change (`Common.h:636-644`) and
  `updateInternalMarket` never does (`:673-679`). The engine originally had no such rule at all
  (it kept its own POD `market_state` and bypassed `common::market`), which would have left quotes
  permanently under-sized after a hedge followed by a re-price. Now carried by
  `quote_policy::price_changed` (external books only) → `ledger_bridge::invalidate` →
  `liquidity_ledger::invalidate_market`; `test_reservation_invalidation` pins all four cases
  (reserve reduces size, size-only update keeps it, price change flushes it, internal never flushes).

## 5. Test-side defects found (O)

**O1. Correlated oracle bug — ODM weighting.** The legacy operator is
`(Aᵀ Wᵀ W A)⁻¹AᵀWᵀW`, i.e. the weight enters **squared** (`W` diagonal ⇒ `Wᵀ = W`). Engine and
oracle both used `W` unsquared, agreed with each other, and therefore reported 100 % parity
while both **disagreed with legacy** for any weight ∉ {0,1} — the fixture uses `0.5` for
spreads, so the shipped operator was wrong.

Fixed in `model.cpp` and `reference_oracle.hpp`, and pinned by a property test that does not
reuse the ODM-building code (the least-squares residual must be `W²`-orthogonal to the columns
of `A`, in `rv_tests.cpp`). This is exactly risk R8 in the plan: a differential gate cannot
catch a defect shared by both sides, so each numeric convention needs one independent property
test.

**O2. Degenerate-input cycles.** Feeding `NULL_PRICE` into a pricing contract makes the
midpoint ~9.2e10 (legacy behaviour, §4). Comparing such cycles tick-for-tick is meaningless: a
1-ulp difference becomes several ticks at 1e16 magnitude, and the stateful EMA then carries it
forward. The differential therefore runs with two-sided books only; the missing-side case has
its own single-cycle test compared in *relative* terms.

**O3. Correlated oracle bug — quote size.** The engine and the oracle both used the **raw**
displayed market size for `quote.size`, whereas legacy takes the *computed* lean size
(`calculateSize`, `:2990-3004`): `min(adjustedMarketSize / hedgeRatio, quoteSize)`, then
`+0.0001` and `toValidQty`. With no reservations and a large quote size the two coincide, which
is why the differential never flagged it — the ledger test did ("reservation shrinks the quote
size" failed because the size was ignoring the adjustment path entirely).

Fixed in `quotes.cpp` (`find_best` now calls `lean_size`, i.e. reservation-adjusted and capped)
and in the oracle (`legacy_lean_size`, a transcription of `calculateSize`). Same lesson as O1:
whenever the oracle and the engine share a shortcut, one of them has to be derived from the
legacy text instead of from the other.

**O4. Hedge reservation target.** Legacy calls `hedgeMarket.addFAKAdjustment(...)` (`:2512`) on
the **display** market (the lean book on the quote side), not on the market the hedge order is
sent to (the opposite side). `hedge_order::market_index` already carries the display market, so
`ledger_bridge::on_hedge` now reserves through that index; resolving it from `side_` would have
claimed liquidity on the wrong book.

**O5. Correlated oracle bug — midpoints for spreads.** Legacy updates the midpoint of **every**
contract with `_usedInPricing` (`!IsZero(weight)`), spreads included (`:1875-1881`), and the ODM
consumes all of them. The engine computed mids only for the pricing contracts, so the ODM saw
zeros in the spread rows; the oracle did the same, so the differential reported 100 % while both
disagreed with legacy.

Found by the *real-config* test, not by the differential: with consistent books the chain
`ODM → P → risk^T` must round-trip to the fed mids exactly, and it was off by 3 ticks. Fixed by
`model::used_in_pricing` + computing mids for all weighted contracts; the round trip is now
**0 ticks** on `config/backtest/model.csv`. Third instance of the same pattern — the
round-trip/property test is what catches shared shortcuts, so each one is worth keeping.

every tested state.
