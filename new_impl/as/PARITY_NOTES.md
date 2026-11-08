# AS (ArbieSpreader) parity notes

Every difference between the new engine (`new_impl/as`) and the legacy strategy
(`src/ArbieSpreader.h` + `.cpp` files) that the parity gates can see, with its classification
and evidence. Required by PLAN.md D1: no unexplained diffs, and any change to these numbers
needs a gate re-run.

Legend: **D** = deliberate deviation (PLAN §5.2), **F** = floating-point / scaling,
**O** = test-side defect, **X** = finding outside this engine.

---

## 1. Current gate status

| Gate | Command | Result |
|---|---|---|
| `as_unit` (phase 0: primitives, config readers, derivations, indices, switch paths, rejections) | `as_tests` | **pass** (0 failures) |
| `as_state` (phase 1: book updates, best-book selection, price scale, path runtime/order, quotes, fairs, dirty set) | `as_state_tests` | **pass** (0 failures) |
| `as_paths` (phase 2: path calculation + ranking vs an independent legacy transcription) | `as_paths_tests 2000 42` | **pass** (100.00000 % of 32 000 fields, 0 mismatched; also at 20 000 × 2 seeds) |
| `as_incremental` (phase 3: incremental update vs the legacy transcription, per-event) | `as_incremental_tests 2000 42` | **pass** (100.00000 % on fields, flags, ranking and validity; also at 20 000 steps) |
| `as_quote` (phase 4: quote arithmetic, crossing clamp, maxTicks bound, emission detection) | `as_quote_tests 2000 42` | **pass** (100.00000 % on fields, status, last-sent and force flags; also at 20 000) |
| `as_hedge` (phase 5: release order, canHedge veto, per-lean orders, switch transfer) | `as_hedge_tests 500 42` | **pass** (100.00000 % on plan, legs and switch legs; also at 3 000 × 2 seeds) |
| `as_host` (phase 6: composition root -- lifecycle, deferral, disable path, hedge round trip, gates) | `as_host_test` | **pass** (0 failures) |
| `as_replay_smoke` (phase 6: real config tape + deployment invariants) | `as_replay config/minimal_cme 500 42` | **pass** (725 quotes, 40 hedges, 0 leaks, 0 violations) |
| `as_bench_smoke` (phase 7: cycle cost per stage, the A/B arms, zero allocations) | `as_cycle_bench 20000 3` | **pass** (0 allocations; 72.3 ns full cycle, 230.6 ns with the ledger) |

---

## 2. Phase-0 evidence

`as_unit` pins, so a later refactor cannot drift silently:

- **`to_legacy_price`** equals legacy `ToRhonePrice` widened (`(int64)(x * 1e8) * 10`) over
  40 001 values spanning ±620 in 1/1024 steps, plus the exact config numbers
  (`0.0015 → 1 500 000`, `2.5 → 2 500 000 000`, `-0.0015 → -1 500 000`, truncation toward zero).
- **`round_to_tick`** equals a transcription of `Common.h:1199-1213` over ±60 multiples of ten
  tick sizes with ±1-unit offsets and negative prices, both sides. The constant-dispatch and
  `floor_divmod` paths are one function, so both are covered.
- **side-signed helpers** equal `applyPriceOffset<Side>` / `calculateTicksThroughMarket<Side>` /
  `IsBetterPrice<Side>` / `BetterPrice<Side>` (`Common.h:335-453`), including `NULL_PRICE`
  losing to any real price.
- **config** (`config/minimal_cme`): 4 pricing contracts, 4 vectors, 2 `ASConfigs` rows with the
  documented values (`QMQ2-QMU2` disabled / `quoteSize 3`; `QMQ2-QMV2` enabled / `quoteSize 7`,
  `edge 0.0005`, `hedgeOffset 0.0004`, `maxTicksThroughMarket 100`) and the four path rows in
  file order with their non-zero multipliers.
- **derivations** (legacy `:52-62`): `edge = ToRhonePrice(0.0005 × 3) = $0.0015`,
  `maxTicksThroughMarket = 100 × tick(0.025) = $2.5`,
  `hedgeOffsetPrice = 0.0004 × tick(0.025) = $0.00001`, `asExtCommission(0.001) = $0.001`,
  path commission `0.01 = $0.01`, and a spread inheriting its first leg's 0.025 tick.
- **model indices**: 8 path definitions → 16 slots, 32 affected edges (16 lean edges × 2 sides),
  every lean edge reachable from its book's affected list, and no path slots for a non-quoting
  contract.
- **switch paths** (synthetic, hand-derived): spread substitution collapses `{O1:-1, O2:+1}` into
  `{S5:-1}` with legacy id string `-1*11*2`, and `{O0:+1, O1:-1, O2:-1}` into `{O2:-1, S4:+1}`
  with id `1*0-1*1-1*2`; `rv_stf_move` = `-$1.00` and `-$1.50` per unit respectively, computed
  from `calculateRVSTFMove` by hand with an identity risk operator.
- **rejections**: paths-per-contract cap, unknown lean contract, a leg that does not precede its
  spread, a non-square model and a duplicate `ASConfigs` row all fail at init with an
  `as::model::build:` message.

---

## 2b. Phase-1 evidence (`as_state`)

- **Price scale**: `PRICE_SCALE == 1e9 == tachyon::DEFAULT_FIXED_PRICE_MULTIPLIER`, one unit is
  1e-9 dollars, `to_scaled(60.0) == 6e10`, and a one-unit book change survives the round trip —
  the state layer never rescales.
- **Book updates** (`Common.h:663-695`): an external price change runs the flush hook exactly
  once per change and *before* the new level lands; a size-only change is a change without a
  flush; internal updates never flush. Book state after each case is asserted.
- **Best book** (`ArbieSpreader.h:313-341`): internal-only, external-only, better price on both
  sides (BID higher / ASK lower), the equal-price tie broken by size with `<=` favouring the
  internal book, and the "both books empty" case taking legacy's first branch (external).
- **The `changed` predicate** (`:335`) is not "the best changed": an update to the old best
  returns true even after the best moved away, and an update to a book that is neither the old
  nor the new best returns false. Both directions are pinned.
- **Commission-adjusted best price** (`:270-275`): the external best is adjusted by
  `ext_commission` with the side's sign (BID net, ASK added); the internal best is untouched.
- **Adjusted size**: the policy seam is used when present (with the reservation filter passed
  through), and the raw displayed size when there is no ledger, so the engine runs standalone.
- **Path runtime / order / quotes / fairs / dirty**: per-slot independence, order mutation and
  `reset_order`, bid/ask isolation, and quote change detection that deliberately ignores
  `lean_price` and `hedge_path` exactly as `Quote::operator!=` (`:214-219`) does.

---

## 3. What the shipped configuration actually exercises

For `config/minimal_cme`, **every** path nets to zero short-term-fair exposure, so the model
builds **`n_offsets == 0`** and no path is a switch path. Worked example (`QMQ2-QMV2`):

```
path "QU+UV"  leans QMU2-QMV2(+1), QMQ2-QMU2(+1), quoted QMQ2-QMV2
  -1*(QMU2-QMV2) - 1*(QMQ2-QMU2) + 1*(QMQ2-QMV2) = 0
```

That is a property of the configuration, not of the engine: the basket exactly reproduces the
quoted spread, so the offset cancels. Consequences:

- the switch-path machinery (`offset_*`, `rv_stf_move`, `ShortTermFairOffset` handling) is
  covered by **synthetic** cases in `as_unit`, and must stay covered in `as_paths` /
  `as_incremental`;
- a deployment that *does* rely on switch paths (partially hedged baskets) needs its own config
  in the gate before P4 signs off, otherwise that half of the strategy ships untested against a
  real file.

## 3b. Phase-2 evidence (`as_paths`)

The gate runs the engine against `tests/path_oracle.hpp`, a transcription of
`Path::calculate` (`:711-831`), `PathSorter` (`:108-130`) and `Contract::updatePaths`
(`:931-985`) that shares no code with `as/paths.hpp`. Over randomised book states (20 000 per
seed, two seeds):

```
path fields : 100.00000% identical (0 mismatched of 320 000)
path order  : 100.00000% identical (0 mismatched of 320 000)
```

and identical with `AS_INSERTION_SORT=1`: the stable insertion sort produces the same ranking as
the `std::sort` call legacy makes. D5 therefore has evidence, not just an argument; the default
stays on `std::sort` until P7 measures which is faster in situ.

Explicit cases, each tagged with the legacy line it covers:

| Case | Legacy |
|---|---|
| basket sum, external-lean commission, path commission, both sides | `:758-761`, `:801-804` |
| quote-size cap and the size determiner | `:742-746`, `:795-798` |
| disabled path / disabled contract | `:715-723` |
| switch path with the fair value missing / present / cleared | `:725-732` |
| non-tradeable lean | `:749-752` |
| lean without a price | `:763-766` |
| zero size, with and without an AS aggressive quote | `:769-783` |
| change predicate: price, size down, size up, at the cap | `:806-808` |
| ranking: valid first, then price by side, then size | `PathSorter` |

### D-AS6 — the legacy staleness guard

`Path::calculate` writes the lean price it used into `_lastPriceInPathCalculation[_side]` where
`_side` is the **path's** side (`:792`), while `canHedge` reads
`_lastPriceInPathCalculation[market->getSide()]` — the **lean's** side
(`ArbieSpreader_Path.cpp:41-42`). For a lean with a negative multiplier the two differ, so the
guard reads a slot this path never wrote. Worse, the array has no initialiser (`:195`) and
`initMarkets` does not touch it, so the first read is indeterminate.

In legacy a path whose leans include a negative multiplier can therefore spuriously fail
`canHedge` (extra recalculation, possibly a different hedge path) or pass when it should not,
depending on stack garbage. The engine writes and reads the **lean's** side and initialises to
`NULL_PRICE`, which is the evident intent; `kLegacyLastPriceSide` reproduces the legacy index so
the P5 hedge gate can measure how often the choice changes a hedge decision.

### D-AS7 — the external commission's two signs

Legacy prices an external lean net of that contract's external commission in two places, with two
different sides:

| Site | Side used | Expression |
|---|---|---|
| `Path::calculate` (full recompute) | the **path's** side `Side` | `applyPriceOffset<Side>(price, -extCommission)` (`:758-761`) |
| `Contract::updateAffectedPathsBestMarket` (incremental) | the **lean's** side `Side` | `getExtCommissionAdjustedBestPrice<Side>()` (`:1126`, `:270-275`) |

For a lean with a positive multiplier the lean side *is* the path side, so the two agree. For a
lean with a **negative** multiplier they apply the commission with opposite signs, so every
external↔internal transition moves the accumulated price by `2·commission·|multiplier|` away from
what a fresh calculation would produce. The engine reproduces this exactly (the differential in
§3b is 100 %), and `as_incremental` states the invariant accordingly: exact equality where the two
signs agree, and the derived bound `2·Σ|multiplier|·commission` where they do not.

Why it is not "fixed": changing the sign at either site changes the quoted price of every
partially-hedged basket by up to two commissions, which moves quotes and fills. That is a
behaviour change requiring its own sign-off and gate run, not a refactor. Recorded here so the
question is visible rather than silently inherited.

---

## 3c. Phase-4 evidence (`as_quote`)

The gate drives the quote stage against `tests/quote_oracle.hpp`, a transcription of
`updateQuote<Side>` (`:1048-1113`), `getExternalQuoteSize` (`:1006-1013`),
`isAggressiveQuote` (`:1000-1004`) and `checkMaxTicksThroughMarket` (`:1115-1122`). Over 20 000
random book states per seed (two seeds):

```
quote fields : 100.00000% identical (0 mismatched of 80 000)
status       : 100.00000% identical (0 mismatched of 80 000)
last sent    : 100.00000% identical (0 mismatched of 80 000)
force flags  : 100.00000% identical (0 mismatched of 80 000)
```

Points worth pinning, all covered explicitly:

- the **edge** widens the quote (`internal = path ∓ edge`) while `lean_price` recovers the
  un-adjusted basket (`path ± commission`), so the two must not be confused: the commission is
  removed by the path calculation and re-added here, the edge is a separate widening;
- the **external price** is the internal price rounded per side (bid floors, ask ceils), not the
  path price rounded;
- the **crossing clamp** uses the *opposite external* book's adjusted size and is itself bounded
  by our own internal size, so a crossing quote can never be larger than the displayed liquidity
  it would take;
- the **maxTicks bound** is strict (`ticks < bound`), compares against the opposite side's *best*
  book (internal or external), and accepts when either price is missing — which is why a quote
  with external quoting switched off can never violate it;
- the **violation stops the ask side**: legacy disables the strategy inside the bid call, so the
  ask call runs with `enabled == false` (fields maintained, nothing emitted, no second violation);
- **change detection** compares only the four emitted fields (legacy `Quote::operator!=`,
  `:214-219`), so a new lean price or hedge path alone does not re-send;
- a **cancellation** is only emitted while a quote is outstanding (`_hedgePath != -1`), and not at
  all while the strategy is disabled.
---

## 3d. Phase-5 evidence (`as_hedge`) and the hedge deviations

`as_hedge` drives the hedger against `tests/hedge_oracle.hpp`, a transcription of
`updateHedge<Side>` (`:552-708`). Over 3 000 random states per model (two models, two seeds):

```
real config   : plan 100.00000% (6 000), legs 100.00000% (14 848), switch legs 100.00000% (6 000)
switch paths  : plan 100.00000% (3 000), legs 100.00000% ( 5 941), switch legs 100.00000% (5 941)
```

**D-AS8 — the switch-path transfer price.** Legacy hands RV `stf + rv_stf_move × qty` (`:635`) as
the fill price of the quoted contract. `rv_stf_move` is a per-unit fair move, so multiplying it by
the quantity makes the transferred price grow with the fill size. That is preserved: the number
ends up in an inter-strategy transfer, and changing it would change the position RV books against
it. `as_hedge` pins `adjusted_fill_price` so the behaviour is visible and cannot drift unnoticed.

**D-AS9 — the veto is not re-checked after the recalculation.** When `canHedge` rejects the best
path, legacy re-prices and then accepts the new best path on `_priceSize.valid()` alone
(`:585-586`). A path whose lean displays *less* than the fill quantity therefore still hedges, and
the shortfall is discovered later by the FAK reservation rather than here. Transcribed and pinned.

**X3 — the front notification and the RV transfer disagree on the lean sides.** In the same
switch-path block, the front `FillMessage` for each lean carries `multiplier > 0 ? Side : otherSide`
(`:657-658`) while the AS→RV fill carries `multiplier > 0 ? otherSide : Side` (`:672`) — the two
messages report *opposite* sides for the same hedge. The RV transfer is the one that moves the
position, so it is almost certainly the front message that is wrong; both are reproduced verbatim
(the plan exposes `front_side` and `rv_side` separately) and flagged here rather than "fixed" in
one place only.

---


---

## 3e. P7: the sort decision and the measured cost

**D5 is settled on evidence.** The ranking is the one place where "reproduce the legacy call
exactly" and "be fast" disagreed. `as_paths` settled it: the stable insertion sort and the
`std::sort` call legacy makes produce the **identical** permutation over 320 000 randomised
comparisons, tie cases included; `as_cycle_bench` measures 16.4 ns against 22.8 ns per call. The
insertion sort ships, and `AS_INSERTION_SORT=0` restores the legacy call for measurement or if a
future config ever produces a divergence.

| Stage | ns/cycle | budget |
|---|---|---|
| full cycle (md + paths + quotes) | 72.3 | 300 |
| path stage, nothing flagged | 29.0 | — |
| quote stage, both sides | 35.9 | — |
| ranking, one call | 16.4 | — |
| incremental delta (one lean moves) | 39.4 | — |
| forced full recompute | 243.4 | — |
| host cycle (md + cycle + ledger) | 230.6 | 300 |

Zero allocations per cycle over 20 000 cycles; 1.12 ns per lean-side. The incremental delta is
**6.17× cheaper** than the full recompute it replaces — the quantitative answer to PLAN §4.3.

---

## 4. Scaling (F)

**F-AS1. Legacy-scale quantisation of config-derived numbers.** Legacy `hf::price_t` is 1e-8;
the new engine is 1e-9. Every value legacy produced by scaling a config double (`ToRhonePrice` at
`ArbieSpreader.cpp:48`, `:58`, `ArbieSpreader_Path.cpp:11`, `calculateRVSTFMove` `:303`) is
therefore computed as `to_legacy_price(x) = (int64)(x * 1e8) * 10`: truncated at the **legacy**
resolution, then widened. A plain 1e-9 conversion truncates one decade finer and can differ by up
to 9 units at 1e-9 (0.9 legacy units). This is pinned by `as_unit` and applies to `edge`,
`ext_commission`, `path_commission`, `max_ticks_through_market`, `hedge_offset_price` and
`rv_stf_move`.

**F-AS2. `-ffp-contract=off`** is applied to `as/config_model.cpp` and `as/model.cpp` for the
reason measured in `new_impl/rv/PARITY_NOTES.md` §1a (FMA contraction changes the last bit before
the `int64` truncation that produces a price). Both files are init-time only, so there is no
runtime cost.

**F-AS3. The legacy tick unit quirk is preserved, not corrected.**
`ArbieSpreader.cpp:61-62` multiplies the config double by the tick size *in price units*, so the
result in dollars is the **dollar product** (`hedgeOffset × tick`, `maxTicks × tick`), not
`dollar × 1e8-integer`. `as_unit::test_legacy_quirks` pins `$0.00001` and `$2.5` for the shipped
values. "Cleaning this up" would change every hedge price, so it is documented rather than fixed.

---

## 5. Findings outside this engine (X)

**X1. `rv::rv_model_cfg` reads the wrong `GlobalConfigs` column.** The file lays out
`GlobalConfigs,Value,Asia,Europe,US,SafeMode`, and legacy fills `GlobalConfig::_shifts` from
**column 2** onward (`ViperConfig.h:738-741`) with `Shifts` starting at Asia (`Common.h:170`).
The rv reader stores every value from column 1 onward (`rv/config.cpp:350-359`), so `values[shift]`
is one column early: `shift_column::us` (2) returns the **Europe** column. The AS layer does not
inherit this — `as_model_cfg::edge_multiplier_for` uses `values[shift + 1]` with a fallback to the
`Value` column, pinned by `as_unit` (Asia 1 / Europe 2 / US 3 / SafeMode 4).

Not fixed here on purpose: changing `rv::rv_model_cfg::width_multiplier` changes RV's width and
therefore its quotes, so it needs its own recorded gate run. This note is the record.

**X2. Two `ASConfigs` layouts exist in the tree.** The canonical section starts
`Contract,Enabled,QuoteExternal,QuoteSize,Edge,HedgeOffset,MaxTicksThroughMarket`
(`ViperConfig.h:923-933`), but `config/minimal/BRN_small_model.csv` predates the `enabled` column.
The legacy reader is positional, so it cannot read that file at all (it would take `TRUE` for
`enabled` and then `lexical_cast` the edge into `quoteSize`). The AS reader is header-driven and
treats an absent `enabled` column as *enabled* (D-AS3), which is what the file intends; both
shapes are parsed in `as_unit`.

