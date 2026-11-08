# ArbieSpreader — performance-first re-implementation plan (`new_impl/as`)

Status: **Phases 0–8 complete** — v1 of the engine is built, gated and measured. Gates
`as_unit`, `as_state`, `as_paths`, `as_incremental`, `as_quote`, `as_hedge`, `as_host`,
`as_replay_smoke`, `as_bench_smoke` all green; 21/21 tests in the standalone suite; zero
allocations per cycle; 72.3 ns per full cycle (230.6 ns with the ledger attached). · Owner: TBD ·
What remains is the legacy shim of §10, which needs the Rhone/vcpkg toolchain.

This document plans and records a from-scratch, performance-first implementation of the
ArbieSpreader strategy. The reference implementation being replaced is `src/ArbieSpreader.h`
(1,143 lines) plus `ArbieSpreader.cpp` (467), `_MD.cpp` (283), `_MessageHandler.cpp` (332),
`_FillHandling.cpp` (134), `_Path.cpp` (47) and `_GUI.cpp` (347) — ~2.5k lines.

**For what the strategy actually does** — the basket maths, a worked quote tick by tick, the fill
flow and the traps — read `STRATEGY.md` in this directory. This file is about the engine that
implements it.

The goal is **lower per-cycle latency and a codebase that can be reasoned about**, not a
rewrite of the Viper framework. Everything off the per-cycle path (GUI, protobuf, config
grid plumbing, MD status handling, order transport, PnL/negative-scalp checkers) stays where
it is.

---

## 0. Recorded decisions

| # | Decision | Value |
|---|---|---|
| D1 | Parity standard | **≥ 99.99 % of cycles produce an identical decision; every remaining diff explained and logged** (not "bit-exact"), same as `rv`/`mm` |
| D2 | Price scale | **1e-9 (`PRICE_SCALE`)** with legacy-scale quantisation of every value derived from a config double (`to_legacy_price`, PARITY_NOTES F-AS1) |
| D3 | Storage | Cap-sized flat arrays (`as/limits.hpp`), zero allocation per cycle; exceeding a cap is an **init-time** failure |
| D4 | Path maintenance | **Incremental** delta updates with the legacy `_recalc/_resort/_dirty` flags. A full recompute exists only for `force` (enable, reload, STF reset) and as an oracle — see §4.3 |
| D5 | Sorting | Replicate the legacy `_resort` rules; insertion sort over the small per-(contract, side) index array; `std::sort` kept as a fallback if the gate ever shows an order deviation |
| D6 | Eigen | **Init-time only** (`risk = (P⁻¹)ᵀ`, consumed immediately into per-path `rv_stf_move`). No matrix work in the cycle |
| D7 | Seams | `order_port` (hedge/quote/notify/alert) and an `stf_source` input. The engine headers never include `rv`; only the cold config layer reuses `rv::rv_model_cfg` / `rv::build_model_inputs`. That is why `viper_as` links `viper_rv` |
| D8 | Concurrency | Single-threaded, cycle-driven; no atomics, no locks in the cycle |
| D9 | Adjusted size | `quote_policy` callback + `reservation_filter`; AS aggressive-quote = ledger *quote group*, FAK pre-adj = `ANTICIPATED_HEDGE`, FAK = `HEDGE` |
| D10 | Quantity signedness | Engine quantities are `int64_t` (legacy `hf::qty_t` is signed and the path size sentinel is `max()`); `tachyon::qty_t` only at the order-port boundary (`wire_qty_t`) |

---

## 1. What the legacy strategy is

One market-data update does not move a price directly — it moves a *path*, a signed basket of
lean contracts. Quoting is "pick the best path, subtract the edge":

| Stage | Legacy reference |
|---|---|
| best book per contract/side (internal vs external, ties by size) | `ArbieSpreader.h:313-341` |
| incremental path delta on a best-price change | `:834-928` |
| full path calc: `price = Σ mult·leanPx(±extComm)`, `size = min(quoteSize, each adjusted size)` | `:711-831` |
| short-term-fair offset: `Σ ratio·stf`, spread substitution ("switch path") | `:199-243`, `:69-225` |
| path fair move (`rv_stf_move`) from the risk operator | `ArbieSpreader.cpp:234-310` |
| quote: `internal = path ∓ edge`, `lean = path ± commission`, external tick-rounded, maxTicksThroughMarket gate | `:1048-1122` |
| hedge along the best path's leans, AS→RV switch fills, FAK adjustments | `:552-708` |
| displayed-size accounting (AS quote adjustment, FAK pre-adjustment, ACK/undo) | `Common.h:611-696`, `new_impl/common/{market,liquidity_ledger}.hpp` |

Hot path: `runCycle` → `updateQuotes(contract)` → `updatePaths` (flagged paths only) →
`updateQuote<Side>` → `notifyOrderRequest`.

---

## 2. Sizing

`config/minimal_cme` (the only complete shipped AS configuration):

| Quantity | Value |
|---|---|
| contracts | 10 (4 outrights + 6 spreads) |
| vectors / pricing columns | 4 / 4 (square, so the risk operator exists) |
| AS quoting contracts | 2 rows (`QMQ2-QMU2` disabled, `QMQ2-QMV2` enabled) |
| paths | 4 per contract, ≤ 3 leans each |
| path slots | 8 definitions × 2 sides = 16 |
| lean edges | 16 (→ 32 affected path/side edges) |
| **switch paths** | **0** — every path in the shipped config nets to zero fair exposure (PARITY_NOTES §3) |

So the cycle is small: ~16 slot updates and ~32 lean-side price terms worst case, against RV's
~1200. The dominant cost is bookkeeping, not arithmetic — which is exactly what the
incremental formulation (D4) exists to keep small.

---

## 3. File layout

```
new_impl/as/
  limits.hpp        caps + static_asserts
  types.hpp         fixed point, side-signed helpers, tick divider, config/runtime PODs
  model.hpp/.cpp    immutable model: per-contract params, path CSR, STF-offset CSR, three
                    inverse indices, per-path fair move; model::build() validates and freezes
  config.hpp/.cpp   as_model_cfg: model CSV ([ASConfigs] + shared sections via rv), paths CSV,
                    secdb; edge multiplier per shift
  config_model.cpp  build_model_inputs: contract order/legs/ticks from rv, then the AS
                    derivations and the risk operator (the only Eigen use)
  tests/as_tests.cpp  phase-0 gate (`as_unit`)
  PLAN.md, PARITY_NOTES.md
```

Later phases add `state.hpp/.cpp` (P1), `paths.hpp/.cpp` (P2/P3), `quotes.hpp/.cpp` (P4),
`hedger.hpp/.cpp` (P5), `host.hpp/.cpp` + `ledger_bridge.hpp` (P6), `tests/as_cycle_bench.cpp`,
`tests/as_host_test.cpp`, `app/as_replay.cpp` (P6/P7).

Namespace `tachyon::trade::as`, `snake_case`, `#pragma once`, `noexcept` on the hot path, no
exceptions in the cycle.

---

## 4. Design

### 4.1 Object model (who owns what)

| Class | Owns | Calls | Legacy counterpart |
|---|---|---|---|
| `model` | immutable config, CSR path/offset tables, inverse indices, per-path fair move | — (factory `model::build`) | `initializeContracts`, `calculateRVSTFMove` |
| `cycle_state` (P1) | books, best-book index, path runtime + order, quotes, `stf[]`, dirty set | — | the strategy's member fields |
| `path_engine` (P2) | nothing but references | `model`, `cycle_state` | `Path::calculate`, `onBestMarketDataUpdate`, `updateBestMarket` |
| `quote_engine` (P4) | the quote policy seam | `path_engine` output | `updateQuote`, `checkMaxTicksThroughMarket`, `getExternalQuoteSize` |
| `hedger` (P5) | hedge id counter, reservations | `path_engine::can_hedge`, `order_port` | `updateHedge`, `sendHedgeOrder` |
| `strategy` (P4/P5) | enabled flag, cycle counter | the three collaborators | `ArbieSpreader::runCycle` |
| `host` (P6) | ports, ledger bridge, venue gates, stats | `strategy` | `ViperStrategyBase` plumbing |

### 4.2 Inverse indices (the performance core)

Three CSR tables are built once in `model::build` so an event touches only what it must:

| Index | From → to | Replaces |
|---|---|---|
| `side_path_*` | (contract, side) → that contract's own path slots | the per-contract `std::vector<Path*>` |
| `affected_*` | (contract, side) → path slots leaning on that book | `Contract::_affectedPaths` + the `std::unordered_set<Market*>` fan-out |
| `offset_path_*` | offset → path slots sharing it | `ShortTermFairOffset::_paths` (`unordered_set<Path*>`) |
| `offset_term_*` | offset → (contract, ratio) terms | `std::vector<ShortTermFair>` + linear `findSTF` |

`path_slot = path_def * 2 + side`, so a slot decodes to (definition, side) without a map.

### 4.3 Why incremental, not a matrix recompute

The path price is linear in the lean prices, so `M · leanPx + const` is a tempting
formulation. It is **not** the right one here, for three independent reasons:

1. **The size is not linear.** `size = min(quoteSize, min_j adjustedSize_j)` comes with the
   legacy asymmetric update rule (`:907-923`: decrease immediately; increase only when the
   recorded size-determiner is the one that grew, then rescan all leans) and a zero-size
   fallback that must distinguish "0 because an AS aggressive quote reserved it"
   (`:769-783`). A from-scratch recompute is *more optimistic* than legacy and changes
   `quote size`, hence fills.
2. **`_lastPriceInPathCalculation` is a hedge veto** (`:926`, `:35-47`): it is refreshed only
   when that lean's price actually moved, and `canHedge` rejects a path whose recorded price is
   worse than the current one. A recompute always refreshes it, removing the veto.
3. **Emission semantics** (`_oldPriceSize` → "need to quote less" / "can quote more",
   `:806-808`) are transitions, not states.

So incremental *is* the specification, not merely an optimisation. A full recompute is kept for
`force`/resync and as the P3 oracle. Eigen stays confined to the init-time risk operator (D6):
at 16×10 integer MACs a matvec is noise next to the gather and `min`/branch bookkeeping, and
`Eigen::SparseMatrix` would break the zero-allocation rule.

### 4.4 Config: two readers, one model

`rv::rv_model_cfg` already parses the shared sections and already skips `[ASConfigs]` as a
block, so `as_model_cfg` adds only: the `ASConfigs` rows (header-driven), the AS paths CSV
(repeating blocks with `Commission`/`Enabled` trailers) and the shift-resolved
`ASEdgeMultiplier`. `rv::build_model_inputs` then supplies contract order, tick sizes, lots,
venues and spread legs — one implementation of the secdb/leg rules, not two.

`rv::model_inputs` gained a `names` field (behaviour-neutral) so the AS layer can map
`AS_paths.csv` columns onto the same contract ids the rv layer assigned, instead of
re-implementing the ordering rule.

### 4.5 STF offsets and switch paths

Per path, transcribed from `initializeContracts`:

```
leans contribute -multiplier of their fair (spread legs expanded by leg ratio)
the quoted contract contributes +1 (or its legs' ratios)
zero ratios are erased
a spread whose legs are all present with a consistent integer ratio collapses into the
  spread's own fair (tryReplaceSTFsWithSpreadSTF)  -- that is what a "switch path" is
an empty term map means the shared zero offset (not a switch path)
```

Deduplication uses the **pre-replacement** term list, because legacy computes its `stfId`
string before substituting (two paths that differ before substitution stay distinct objects
even when they coincide after it). The legacy id string is kept on the model as gate evidence.

`rv_stf_move` is `to_legacy_price(stf_move / 2)` with
`factor_move[v] = -net_pos[v] * range[v] / maxPos[v]`, contracts aggregated in ascending id
order (legs must precede their spread — validated at init).


---

## 5. Semantics contract

### 5.1 Reproduced exactly (each becomes an `as_paths` / `as_quote` case)

Best-book tie-breaks; `size == 0` → `getAdjustedSizeExcludeFAKPreAdj()` then path reset; the STF
`NULL` reset-vs-recalc distinction; the `_oldPriceSize` change rules; external price tick
rounding (BID floor / ASK ceil) and the crossing external-size clamp; the strict `<`
maxTicksThroughMarket gate that disables AS *and* external quoting; hedge side by multiplier
sign; the AS→RV switch fill that mirrors the hedge (quoted contract plus one fill per lean);
`lean_on_rv` when a lean is RV's internal quote market.

### 5.2 Deliberate deviations

| # | Deviation | Reason |
|---|---|---|
| D-AS1 | 1e-9 scale with legacy-scale quantisation of config-derived numbers | D2; pinned by `test_primitives` and F-AS1 |
| D-AS2 | Config problems fail at **init** (edge multiplier outside [1,10], zero `maxPos`) instead of at enable time | a config that can never run should not start |
| D-AS3 | `ASConfigs` without an `enabled` column means *enabled* | the pre-flag format (`config/minimal`) expects to trade |
| D-AS4 | Ordering among non-best paths may differ from `std::sort` (insertion sort) | only if the P3 gate shows it; `std::sort` fallback available |
| D-AS5 | An offset whose terms net to empty is the shared zero offset | identical to legacy `_zeroStfOffset` |
| D-AS6 | `last_path_price` is written and read on the **lean's** side, and initialised to `NULL_PRICE` | legacy writes the *path's* side but reads the *lean's* side (`:792` vs `_Path.cpp:41`), and never initialises the array at all (`:195`). For a negative multiplier the guard inspects an unrelated slot, and the initial read is indeterminate memory. Defined here as the evident intent; `kLegacyLastPriceSide` reproduces the legacy index for measurement |
| D-AS7 | The external commission keeps legacy's two different signs: the full calculation uses the **path's** side (`:760`), the incremental update uses the **lean's** side (`:1126`) | preserved, not corrected. For a positive multiplier the two agree; for a negative one every external↔internal transition shifts the accumulated path price by `2·commission·|multiplier|` relative to a fresh calculation. Correcting it would move quotes on every partially-hedged basket, so it is gated and bounded rather than changed (`as_incremental` asserts exact equality where it must hold and that bound elsewhere) |
| D-AS8 | The switch-path fill price handed to RV is `stf + rv_stf_move × qty` (`:635`), i.e. a per-unit fair move multiplied by the fill quantity | transcribed as-is: it is a price in the RV transfer message, so "fixing" it would change the position RV books against the transfer. Pinned by `as_hedge` (`adjusted_fill_price`) |
| D-AS9 | After a `canHedge` veto the forced recalculation is accepted on **validity alone** (`:585-586`), not by a second `canHedge` | transcribed: a path whose lean displays less than the fill quantity still hedges. Pinned by `as_hedge` |

---

## 6. Validation strategy

| Gate | Phase | What it proves |
|---|---|---|
| `as_unit` | 0 | primitives (incl. `round_to_tick` vs `RoundToTickSize`, `to_legacy_price` vs `ToRhonePrice`), config readers, per-contract derivations, indices, switch paths / spread substitution, fair move, init rejections, two real configs |
| `as_paths` | 2 | path full calc + best-book selection vs a transcription of `:711-831` |
| `as_incremental` | 3 | **key gate**: 100 k-op random stream (md, STF, FAK ack/undo, quote adjustments) — incremental state must equal the legacy-logic oracle field by field (price, size, determiner, flags, order) |
| `as_quote` | 4 | quote + external size + maxTicksThroughMarket differential ≥ 99.99 % |
| `as_hedge` | 5 | `can_hedge` recalc, hedge prices/sides, FAK reservation, switch fills, ordered request sequence |
| `as_host` | 6 | lifecycle: emit only on change, deferred emission, disable path, ledger round-trip |
| `as_replay` | 6 | real `config/minimal_cme` deterministic tape + deployment invariants, exit code |
| `as_bench` | 7 | ns/cycle per stage, allocations = 0, lean-sides/cycle; A/B incremental vs full, insertion vs `std::sort` |

Budgets (advisory, as `rv` measured them on this box): full cycle ≤ **300 ns** for the
`minimal_cme` shape; quote stage dominant; **zero allocations** is the hard invariant.

---

## 7. Phases

- **P0** primitives, config, model/indices, `as_unit` — **done**
- **P1** `cycle_state` + best-book selection → `as_state` — **done**
- **P2** `path_engine` full calc → `as_paths` (differential vs the legacy transcription) — **done**
- **P3** incremental updates + property gate vs the legacy-logic oracle → `as_incremental` — **done**
- **P4** `quote_engine` + maxTicks gate + external sizing → `as_quote` — **done**
- **P5** `hedger` + fills + FAK/switch fills → `as_hedge` — **done**
- **P6** `host` + `ledger_bridge` + lifecycle + replay → `as_host`, `as_replay_smoke` — **done**
- **P7** bench + A/B measurements + final PARITY_NOTES → `as_bench_smoke` — **done**
- **P8** legacy shim mapping table (§10) + docs — **done**

---

## 8. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Incremental path state drifts from the legacy rules | `as_incremental` property gate; full-recompute debug switch |
| R2 | Sort order parity | the gate compares the full path order; `std::sort` fallback |
| R3 | STF sentinel / NULL handling | explicit oracle cases for reset vs recalc |
| R4 | Ledger quote-group semantics vs legacy per-market counters | gate the observable adjusted size |
| R5 | No legacy binary here ⇒ no layer-2 golden traces (same position as `rv`) | layer-1 oracle first; trace gate when a legacy build exists |
| R6 | Duplicated price primitives between `rv` and `as` | byte-identical apart from the namespace; consolidation into `common/price.hpp` pending (mm R6) |

---

## 9. P0/P1 status (measured)

```
as_unit                       pass (0 failures)
  primitives                  40 001 to_legacy_price cases + tick sweep, bit-exact
  config                      config/minimal_cme: 10 contracts, 2 AS rows, ASEdgeMultiplier US = 3
  model                       8 path defs, 16 slots, 32 affected edges, 0 offsets
  stf offsets                 synthetic switch paths: spread substitution + fair move
  rejections                  5 malformed inputs rejected at init
  other configs               config/minimal (pre-`enabled` format) also loads

as_state                      pass (0 failures)
  price scale                 PRICE_SCALE == 1e9 == tachyon's multiplier; one unit == 1e-9
  books                       external price change flushes, size-only and internal do not
  best book                   internal/external only, better price, tie by size (<=), both
                              empty -> external, and the legacy `changed` predicate
  best price                  external best carries the commission with the right sign per side
  adjusted size               ledger seam vs raw fallback, filter mask pass-through
  path runtime / order         per-slot state, order mutation and reset, ask/bid isolation
  quotes / fairs / dirty       change detection excludes lean price and hedge path; first-touch
                              dirty order; init() is a full reset

full suite                    15/15 tests pass (rv + mm unchanged)
```

### 9.1 P2 status (`as_paths`, 20 000 book states × 2 seeds)

```
path fields : 100.00000% identical (0 mismatched of 320 000)
path order  : 100.00000% identical (0 mismatched of 320 000)
```

with both sort implementations (`std::sort` as legacy calls it, and `AS_INSERTION_SORT=1`)
reporting zero differences — so D5 is decided on measurement, not on faith.

Covered by explicit cases, each tagged with its legacy line: the basket sum with the external
commission and the path commission on both sides; the quote-size cap and the size determiner;
every reset branch (disabled path, disabled contract, non-tradeable lean, missing price, zero
size, and the AS-aggressive-quote fallback with and without a size behind it); the change
predicate's asymmetry around the quote size; a switch path with the fair value missing, present
and cleared; the ranking invariants. The randomised differential then compares every path field
and the whole ranking against `tests/path_oracle.hpp` — an independent transcription that shares
no code with `as/paths.hpp`.

**Bug this gate caught in the engine:** the non-switch-path case looked up
`offset_value(INVALID_INDEX)` instead of adding legacy's zero offset, which read past the array.
The oracle had it right, which is exactly why the oracle is written from the legacy text rather
than from the engine.

### 9.2 P3 status (`as_incremental`, 20 000 steps × 2 models × 2 seeds)

```
incremental stream, real config   : fields 100.00000%, flags 100.00000%,
                                    order 100.00000%, validity 100.00000%
                                    (0 mismatched of 320 000 each)
incremental stream, switch paths  : same, 0 mismatched of 80 000
price invariant, exact paths      : 100.00000% (incremental price == recomputed price)
price invariant, negative mult.   : 100.00000% inside the derived bound
size after full recompute         : reported, not asserted (legacy asymmetry, §4.3)
```

The stream mixes four event kinds — external/internal book moves, fair-value arrivals (which
recompute the offsets and re-price the paths that share them), ordinary cycles, and forced
recalcs — and compares *every path field, every pending flag, the whole ranking and validity*
against the transcription after every single event. The flags are included deliberately: a
divergent `recalc`/`resort` would only surface as a diverging decision two cycles later.

The price invariant is stated the way legacy actually behaves (D-AS7): exact equality where the
two commission signs agree (positive multipliers), and the derived bound where they do not. The
bound is not a fudge factor — it is `2·Σ|multiplier|·commission` per path, derived from the two
legacy call sites, and it holds at every step of the stream.

### 9.7 P7 status (`as_cycle_bench`, 20 000 cycles × 5 runs, median)

```
full cycle (md + paths + quotes)     72.3 ns   217 cycles    budget 300 ns   pass
  path stage, nothing flagged        29.0 ns    87 cycles
  quote stage (both sides)           35.9 ns   108 cycles
  ranking (one sort call)            16.4 ns    49 cycles   [insertion, shipped]
  host cycle (md + cycle + ledger)  230.6 ns   692 cycles   budget 300 ns   pass
lean-sides/cycle                        32     1.12 ns each
allocations                              0     over 20 000 cycles
```

Budgets are advisory on this dev box (rv measured 6-19x inflation on unrelated microbenchmarks
here); the hard invariant is zero allocations, and it holds.

**A/B 1 — incremental vs forced full recompute (D4, §4.3).** One lean moves:

| Path | ns | cycles |
|---|---|---|
| incremental delta | 39.4 | 118 |
| forced full recompute | 243.4 | 730 |
| **ratio** | **6.17x** | |

This is the number §4.3 predicted qualitatively: the delta touches one path's price, the recompute
re-walks the basket, re-derives the size and re-checks every lean's adjusted size. It also means
the incremental machinery pays for itself even at this tiny shape (4 paths, ≤3 leans).

**A/B 2 — the ranking (D5).** Same bench, one compile flag apart:

| Sort | ns | cycles |
|---|---|---|
| `std::sort` (legacy call) | 22.8 | 68 |
| insertion sort (shipped) | 16.4 | 49 |

Insertion sort is 28 % cheaper, and `as_paths` proves the two produce the **identical** ranking
over 320 000 randomised comparisons (including the tie cases), so the cheaper one ships and
`AS_INSERTION_SORT=0` restores the legacy call for measurement.

**A/B 3 — the constant tick dispatch.** `AS_CONSTANT_TICK_DISPATCH=1` (shipped) 36.3 ns vs
`=0` 34.5 ns on the quote stage: within run-to-run noise (~5 %), unlike rv's ~3 % win where the
rounding runs 1200× per cycle instead of twice. Kept on (it is a switch either way), recorded as
"no measurable difference at this shape".

**Where the remaining time goes.** The host cycle costs 230.6 ns against the engine's 72.3 ns, so
the ledger bridge accounts for ~160 ns: two `available()` queries per book update and the
`invalidate_market` on every price change, each a scan over the ledger's reservation vector. That
is the top follow-up candidate, and it lives in `common/liquidity_ledger` — shared with rv and mm,
so it needs its own measured change rather than an AS-side workaround.


### 9.6 P6 status (`as_host`, `as_replay_smoke`)

`as_host` drives the composition root through the legacy lifecycle: quote emission and change
detection, deferred emission while a transaction is open, the maxTicks disable path (alert +
cancel + no further quotes), the hedge round trip (order → reservation → ack → done → drained),
an unhedgeable fill forwarded to the market maker, the external-quoting gates (strategy, contract
and venue) and the aggressive-quote reservation being taken and released on a fill.

`as_replay config/minimal_cme 500 42` replays a coherent tape — two-sided books around a fair
value derived from each spread's legs, drifting outrights, occasional internal crosses, periodic
fair values and fills — and asserts the deployment invariants:

```
cycles=500  quotes=725  notifications=467  hedges=40  fills=40  stf=50
alerts=0  disabled=no  reservations_taken=40  active=0

every requested cycle ran                            pass
quotes were emitted                                  pass
quote notifications were raised                      pass
the tape filled quotes                               pass
the fills were hedged                                pass
every hedge reserved liquidity                       pass
all reservations drained after the acks (no leak)     pass
an adjusted size never exceeded the displayed size   pass
the run did not have to disable the strategy         pass
```

Two bugs the host gate caught, both real:

1. **`quote_external` in the emitted request ignored the venue gate.** The host reported the
   strategy-wide flag, so a venue-blocked contract would have been shown as quoting externally
   while its external price was NULL. It now reports the full per-contract gate, which is what
   legacy passes to `sendQuoteRequest`.
2. **The hedge ack had no market to release against.** My first API carried only the hedge id, so
   the reservation could never be completed. The transport knows the book (it sent the order), just
   as the legacy ack message carries the instrument and side, so `on_hedge_ack`/`on_hedge_done`
   take the ledger key and the `hedge_order` now carries it as `target`.


### 9.4 P5 status (`as_hedge`, 3 000 states × 2 models × 2 seeds)

```
real config   : plan 100.00000% (6 000), legs 100.00000% (14 848), switch legs 100.00000% (6 000)
switch paths  : plan 100.00000% (3 000), legs 100.00000% ( 5 941), switch legs 100.00000% (5 941)
```

`plan` compares the hedge/no-hedge decision, whether the veto forced a recalculation, the path id,
the hedge id sequence, the switch-transfer flag, the RV transfer price and the transfer leg count;
`legs` compares every field of every hedge order (contract, side, both prices, size, external flag,
market); `switch legs` compares the transfer's per-lean legs.

Explicit cases pin the legacy behaviours that are easy to get wrong:

- **direction**: a bought bid is hedged with an **ask** order and vice versa, priced off the book
  the path *leaned on* plus that lean's hedge payup, while the external price is the **raw** lean
  price (no payup, no commission);
- **the payup sign**: `applyPriceOffset<order_side>` means an ask hedge subtracts it and a bid
  hedge adds it, so the same number widens both sides;
- **the release order** on a fill: an internal fill releases the same-side internal book, then the
  same-side external book (capped at the fill quantity), then — for any fill — the opposite side's
  external book, and only books with a live adjustment are touched;
- **the veto** fires when a lean has moved *worse* since it was priced (the path would be hedged on
  a better book than exists), and the forced recalculation is then accepted on validity alone
  (D-AS9);
- **the hedge id** is one per event, shared by the legs and their FAK reservations, and monotonic.

### 9.3 P4 status (`as_quote`, 20 000 book states × 2 seeds)

```
quote fields : 100.00000% identical (0 mismatched of 80 000)
status       : 100.00000% identical (0 mismatched of 80 000)
last sent    : 100.00000% identical (0 mismatched of 80 000)
force flags  : 100.00000% identical (0 mismatched of 80 000)
```

`status` is the emit / violation / nothing decision, `last sent` the record that drives the next
change detection, and `force flags` the consumption of the per-side re-quote flag — the three
things that would silently change emission behaviour if they drifted. Explicit cases cover the
edge and commission arithmetic on both sides, tick rounding, the crossing-size clamp (including
its bound by our own internal size), the strict `<` maxTicks bound with its one-unit boundary,
external quoting switched off, a disabled strategy (fields maintained, nothing emitted) and the
cancellation branch.


---

## 10. Legacy shim: what the Viper host has to call

The engine is complete and gated; what remains for deployment is the ~120-line adapter that maps
the legacy `ArbieSpreader` resource onto `as::host`. This is the same split `mm` documents in its
§8: everything on the engine side of the seam is compiled and tested, the shim is a translation
table.

| Legacy event | Shim call |
|---|---|
| `onMDUpdate` (external top of book) | `on_market_data(c, sd, true, price, size)` |
| `processInternalMarketUpdate` (IME best) | `on_market_data(c, sd, false, price, size)` |
| `Fill` message | `on_fill(c, sd, qty, msg._type == FillType::Internal)` |
| `STFUpdate` (from RV) | `on_stf(c, fair)` |
| `HedgeMDAdjsutAck` | `on_hedge_ack(hedge_id, target, order_id, actual)` |
| `InternalHedgeDone` / `FAKDone` | `on_hedge_done(hedge_id, target)` |
| `runCycle(now)` | `run_cycle(now)` |
| `Enable`/`Disable`, `QuoteExternallyOn/Off`, per-venue gate | `enable`, `set_external_quoting_enabled`, `set_venue_enabled` |
| executor transaction open/closed | `set_can_send_quotes(bool)` |
| GUI rows (`ArbieSpreaderParameters`, `ArbieSpreaderPaths`) | stays legacy; the engine takes a rebuilt model |

Outbound, the shim implements the eight `host_ports`:

| Port | Legacy call |
|---|---|
| `send_quote` | `OrderRequestManager::sendQuoteRequest(..., canQuoteExternal, leanOnRV, ...)` |
| `notify_quote` | `notifyOrderRequest(internalId, ArbieSpreader, ...)` |
| `send_hedge` | `sendHedgeRequest(hedge_contract, orderId(AS, Hedging), sd, internal, external, size, external, ..., marketId, hedgeId)` |
| `send_fill` | the `sendFillToFront` lambda (`sendMessageToFront<FillMessage>`) |
| `send_switch_fill` | the `ASFillsToRV` bundle plus the per-lean front `FillMessage`s (`:615-680`) |
| `forward_fill` | `sendMessage<FillMessage>` to the MarketMaker with `FillSource::ASUnableToHedge` / `AsDisable` |
| `on_alert` | `sendErrorMessage(..., ErrorType::Critical)` |
| `cancel_all` | `sendCancelRequest(StrategyId::ArbieSpreader)` |

Enum mapping: `FillType::Internal` → `is_internal`; `hf::SIDE` → `as::side`; the venue code is the
host's own (`secdb.activate`), the same mapping `rv` uses.

**Then delete** from the shim: the path machinery, the market-adjustment bookkeeping, the quote
arithmetic and the hedge decisions — the ~2.5k legacy lines this replaces. What stays is the
resource wrapper, the GUI, the config-grid plumbing and the transport.

**Limitation, stated plainly:** the shim is not in this build (the legacy target needs the full
Rhone/vcpkg toolchain). Everything on the engine side of the seam is compiled, gated and
benchmarked here; the shim is the table above.

