# MarketMaker parity notes

Every difference between the new engine (`new_impl/mm`) and the legacy strategy
(`src/SimpleMarketMaker.h`) that the parity gates can see, with its classification and
evidence. Required by PLAN.md §7 (D3): no unexplained diffs, and any change to these numbers
needs a gate re-run.

Legend: **D** = deliberate deviation (PLAN §0/§4), **F** = floating-point association,
**O** = test-side defect.

---

## 1. Current gate status

| Gate | Command | Result |
|---|---|---|
| `mm_unit` (phase 1: price scale, tick division, lot flooring, model derivation, validation, real-config parse) | `mm_tests` | **pass** (0 failures) |
| `mm_state` (phase 2: book change indices / lookup / trade adjustment, order queue branches, 5000-op property test, caps) | `mm_state_tests` | **pass** (0 failures) |
| `mm_quote` (phase 3: pricing block + updateQuote on the bypass paths, differential gate vs the legacy transcription) | `mm_quote_tests 200000` | **pass** (0 decision differences, see §3) |
| `mm_cycle` (phase 5: hedge size reconciliation, hedge pricing/payup, both cross checks, ordered request sequence) | `mm_cycle_tests` | **pass** (0 failures) |
| `mm_host` (phase 6: fill allocation, overfill clamp, position limits, cycle driver, maxTicksWide latch, deferred emission, fill → re-hedge) | `mm_host_tests` | **pass** (0 failures) |
| `mm_bench_smoke` / `mm_replay_smoke` (phase 7) | `mm_cycle_bench 20000 3` / `mm_replay config/backtest 20000 42` | **pass** (0 allocations per cycle; all replay invariants hold) |
| `mm_adapter` (phase 8: the integration seam driven through a recording `request_sink`) | `mm_adapter_tests` | **pass** (0 failures) |

The decision-level gates (mid / quote price / quote size / hedge price / hedge size / inPayUp /
ordered request sequence) arrive with phases 3-5 and will be recorded here as a table in the
shape of `new_impl/rv/PARITY_NOTES.md` §1.

## 2. Phase-1 evidence

`mm_unit` pins, so that a later refactor cannot drift silently:

- `to_mm_price(x) == (int64)(x * 1e8) * 10` for 200 000 values plus the config values actually
  present in `config/backtest/model.csv`;
- `round_to_tick` (both the constant-dispatch and `fast_div` paths) == a transcription of
  `Common.h:1196 RoundToTickSize`, over every multiple ±1 tick for nine tick sizes (one of them
  deliberately outside the constant list, to exercise `floor_divmod`) plus a strided sweep,
  including negative prices;
- `to_valid_qty` against `ViperConfig.h:85-87`;
- each derived field of `model::build` against its legacy expression from
  `SimpleMarketMaker.h:192-236` (commission ×10, hedge offset product, max-ticks-wide product and
  its double, the unbounded sentinel, the worsen-payup product, the EMA period in cycles);
- `model::round` against `RoundToTickSize(price ∓ commission)` for both sides (S2);
- the legacy validation rules: a lot-floored quote size of 0 is fatal (`:205-207`), a
  non-positive or `IsZero` worsen-payup percentage is rejected (`ViperConfig.h:901`), a
  non-positive tick is rejected, `queueImportance > 10` is zeroed (`ViperConfig.h:905`), and the
  `MAX_CONTRACTS` cap is enforced at init.

## 2b. Phase-2 evidence (`mm_state`)

`book` (transcribed from `getPriceLevelIndex` `:407-422`, the change-index bookkeeping in
`MarketDataConsumer.h:129-152`, and `handleMDTrade` `:175-222`):

- feed level numbers are 1-based with `0 = no change`, and only the *smallest* index since the
  last reset survives; an index beyond the book rejects the whole update and records nothing;
- level lookup returns the index, or `-1` for a NULL price, a price better than the top, an
  absent price, or anything past the first NULL level;
- `tracked_qty` subtracts the implied internal quantity (the `targetSize` input at `:795`);
- all four trade cases (same price, better than best, inside the book, through the book),
  including the fact that a trade leaves `ext.implied` untouched and that the mid-book case
  clears only index `LEVEL_NUM - i` — the tail level keeps a stale duplicate that
  `level_index` cannot reach because it stops at the first NULL level.

`order_queue` (transcribed from `onOrderUpdate` `:1562-1609` and `onQueuePositionUpdate`
`:1611-1642`):

- the five `onOrderUpdate` branches, insertion-order iteration, `orders.back()` after an
  in-place quantity update, middle removal preserving order, price moves appending at the back,
  and a price move carrying `qty == 0` being a fill rather than a re-insertion;
- a *new* order with `qty == 0` is retained, because the legacy insert branch has no quantity
  test — an easy "fix" that would silently change behaviour;
- queue positions: unknown ids rejected, `NOT_SET == -1` preserved, anything below it clamped to
  `0`;
- caps: slot and per-side level exhaustion report `false` + `overflowed()` without changing the
  size or corrupting the levels, and the arena is recycled over 4 x `MAX_ORDERS` insert/cancel
  cycles;
- a 5 000-operation randomised run compared step-by-step against an independent transcription of
  the legacy containers (`std::unordered_map` + per-level `std::vector`): size, per-order
  presence/price/qty/qp/side, per-level iteration order and `count_at`, all identical.

## 2c. Type-fidelity notes (why `qty_t` is signed)

`hf::qty_t` is `rhone::common::QtyT = int_fast64_t` (`rhonehf/include_public/hf/hf.h:54`,
`neorhone/common/RhoneTypes.h:16`), so every legacy quantity is signed. Reading that as unsigned
would invert three checks: `qp >= 0` (`:835`), `qp == NOT_SET` (`:816`) and the position offsets
in `tryOffsetPosition` (`:28-48`). The engine therefore uses `int64_t` quantities and only
converts to `tachyon::qty_t` at the order port (PLAN.md D10).

## 2d. Deliberate behaviour notes found while transcribing

| # | Note | Classification |
|---|---|---|
| N1 | The order side is taken from the side recorded when the order was first seen, not from a later message (legacy uses `msg.side` for every `_priceOrders` operation, `:1591-1607`). Only differs for a self-contradicting feed; prevents a wrong-side removal from corrupting the other side's list | D11 |
| N2 | A mid-book trade leaves a stale duplicate at `LEVEL_NUM - 1` because legacy clears only `LEVEL_NUM - i` and only `pr`/`q` (`MarketDataConsumer.h:203-213`). Reproduced; unreachable through `level_index` | none (faithful) |
| N3 | A new order carrying `qty == 0` is stored (`:1585-1589` has no quantity test) | none (faithful) |


## 2e. Phase-3 evidence (`mm_quote`)

Hand-written cases (the branches a random tape rarely reaches):

- internal-only quoting: price = mid ∓ width, external price untouched, internal size capped
  by `pctTob x top size`, and an identical second cycle emits nothing;
- external quoting: join below/above the join size, shrink to the market cap above the leave
  size, leave the book below it;
- snapping to the touch, with the commission moved onto the internal price (`:762-766`);
- payup: internal price worsened to the touch less the allowance, external = rounded internal
  (`:683-699`);
- position caps for both internal and external sizes, including the "short position frees the
  room" branch (`:1748-1790`);
- gating: the `maxTicksWide` latch suppresses every recalculation (`:605`), and a deeper book
  change only triggers one when it is at or in front of our own quote level (`:603-628`).

Differential gate (200 000 random cycles x 2 sides) — the numbers are in §3. Additional
assertions that are not visible in the table:

- the established level (the EMA, including `exp(-alpha)` and the truncate-then-clamp) is
  bit-identical in the quantity domain;
- the *change decision* — "does this cycle need to emit a request" — matches in all 400 000
  evaluations, which is the field that actually drives order flow.

## 2f. `abs()` on a sentinel (documented UB avoidance)

Legacy compares the new price against the last price *sent* with
`std::abs(internalPrice - _quotes[Side]._lastInternalPriceSent)` (`:675`, and the same pattern
for hedges at `:648`). On the first cycle the sentinel is still `NULL_PRICE` (INT64_MAX), so the
`int64` subtraction overflows — undefined behaviour that in practice wraps to a huge magnitude
and therefore always reports "changed".

`types.hpp::abs_diff` computes the magnitude through `__int128` (with a two's-complement
fallback), which is well defined and yields the same decision. The oracle reproduces the same
construction, so the gate cannot paper over a difference here. No observable change; recorded
because a UBSan build of the legacy code would flag it.

## 2g. Phase-4 evidence (`mm_quote`, queue model)

The S-curve and the order walk are covered by hand-written cases plus a second differential run
with `queueImportance = 3` (0..3 resting orders per side, random queue positions including
`NOT_SET`, three book levels with implied quantities):

- the logistic is centred on 0.5 — `qp = establishedLevel/2` yields an offset of exactly zero —
  and antisymmetric (front `+1 587 872`, back `-1 587 872` at width 2.5e6), clamping at both ends;
- queue importance 0 flattens it;
- with nothing resting, the back-of-queue price is evaluated and, when it prices worse than the
  target, legacy quotes the ordinary price **with zero size** (`:800-803`) — easy to get wrong;
- an order whose queue position prices it a tick worse stops the walk; `cost/qty` then covers the
  accepted orders only;
- `NOT_SET` is treated as the back of the level;
- the spray adds one quote size on top of the resting quantity, capped by `pctTob × topSize` and
  by the incremental-size rule (`:866-872`), and is skipped when the gap behind the last order is
  below `quoteGap`.

Measured (`queue model` rows in §3): every decision field is 100 % identical; only the unrounded
internal price carries the truncation deviation, now up to 24 units because the per-order offset
truncations compound through `cost / qty` — still a thousandth of a tick.

## 2h. Phase-5 evidence (`mm_cycle`)

The hedge half and the cycle assembly, transcribed from `modifyHedgeSize` (:1119-1215),
`calculateHedgePrices` (:1147-1163), `updateHedgePrice` (:630-659), `checkQuoteCross` (:512-537),
`checkHedgeCross` (:539-570) and the two drivers (`onMarketDataChange` :484-509,
`forceUpdateQuotesUpdateHedgePrice` :579-601 / `applyUpdates` :1194-1210):

- **size reconciliation**: a short position hedges with a bid for the whole net position; the same
  position again emits nothing; flipping long emits the ask hedge *first* and then an explicit
  size-0 bid hedge; going flat pulls only the side that carried size; internal and external
  positions that net to zero emit nothing;
- **hedge pricing**: a buy hedge references the more aggressive of our own ask quote and the ask
  touch, steps back by `hedgeOffsetPrice`, and never prices through the mid (`BetterPrice`, so a
  NULL quote falls back to the book); the ask side mirrors it; in payup the hedge chases the
  opposite touch and pays the commission;
- **cross checks**: the quote/quote check forces the untouched side out only when exactly one side
  moved *and* it would cross; the hedge/quote check pushes out the opposing instrument in both
  directions and is **skipped for a hedge in payup** (:541-543);
- **ordered sequence**: `apply_updates` emits the size-only hedge first (from the reconciliation),
  then both quotes, then the repriced hedge; a repeated update re-emits the quotes (forceUpdate is
  unconditional) but *not* the hedge when its price did not move; the md-change path quarantines
  all quoting before any hedging.

One structural point worth stating because the plan called it out: `intents.hpp` makes the
ordering part of the engine's contract. The host's only job is to forward the list in order and
emit one `notifyOrderRequest` per cycle — the sequence, not just the prices, is what the gate and
the eventual adapter verify.





## 2i. Phase-6 evidence (`mm_host`)

`position_book` (transcribed from `tryOffsetPosition` :26-48, `offsetPosition` :991-1000,
`allocateHedgingFill` :957-990, `allocateHedgingFillRemainder` :944-955,
`allocateExternalQuoteFill` :1058-1072, `onFill` :1013-1056, `takeOverFill` :238-317,
`forceTakeOverFill` :318-340, `isWithinPositionLimits` :1211-1225):

- an internal quote fill opens the internal position and consumes the quote's internal size; a
  fill larger than the shown internal size books the **excess as external** (:1023-1030);
- an external quote fill offsets an opposite position first and only then touches the external
  position; the displayed external size clamps at zero rather than going negative;
- a hedge fill offsets the positions and reports its allocation **with the fill's sign**;
- an overfill becomes external exposure up to `maxPosExternal`, then reports the remainder as
  untaken (the host turns that into "disable external quoting", which is what legacy does when no
  other strategy will take it, :288-309); a *forced* overfill ignores the limit and can still ask
  for the disable through `checkLimit`;
- position limits are inclusive (`abs(position) > limit` fails), checked on both positions.

`host` (transcribed from `onMarketDataChange`'s outer loop, the `maxTicksWide` block :452-482, the
missing-MD path :375-394, `copyBooks`'s copy-on-quantity-change, `MarketMaker_MD.cpp:13-19` and
`MarketMaker_MessageHandler.cpp:79-92`):

- dirty tracking marks a contract **once** per cycle regardless of how many updates it received,
  and the set is cleared after the cycle;
- an equal or older batch is stale and rejected (counted); a price-only update records the index
  but does **not** copy the snapshot;
- the `maxTicksWide` latch: above the limit external quoting stops and **both quotes are pulled
  with an explicit NULL-priced request** on the transition (:475-482); the latch clears when the
  spread comes back; above 2x the contract is disabled and its orders are cancelled;
- gating: a disabled strategy or a halted contract emits nothing; `canSendQuotes == false` defers
  the whole cycle and leaves the contract dirty, so the change is emitted on the first cycle after
  sending resumes;
- fills end to end: a 5-lot internal buy re-hedges with an ask of 5 on the next `end_of_fills`, and
  a fill handed over by another strategy is taken by force and ends the cycle inline (:1000).

## 2j. Deliberate deviations in the host

| # | Deviation | Why |
|---|---|---|
| N4 | The queue-position re-quote triggers on "the last order **at our own price level**" instead of legacy's `std::next(orderIter) == _orders.end()` | Legacy's guard depends on `std::unordered_map` traversal order, which is not reproducible by construction. The deterministic interpretation targets the order whose queue position actually gates the spray |
| N5 | `canSendQuotes == false` defers the *cycle*, keeping the contract dirty | Legacy defers at the transport (the request is queued, the notification withheld). Deferring the decision produces the same emitted sequence once sending resumes, and avoids committing `lastSent` values that were never sent |
| N6 | Per-contract `_lastMDbid` is committed at cycle end, with the in-flight batch id kept separately | Legacy sets it in `onEndMDBatch`; without modelling batches, committing at the cycle boundary reproduces the same accept/reject pattern for every update in one batch |



## 3. Price-scale deviations (D2)

The engine works at 1e-9 while legacy works at 1e-8, with every config-derived price produced as
`to_mm_price(x) = (int64)(x * 1e8) * 10` so that it is exactly ten times the legacy value. This
keeps `+ - * /`-free arithmetic (comparisons, `%` tick rounding) decision-identical and confines
the deviation to the truncating sites below.

| Class | Meaning | Sites |
|---|---|---|
| D-scale-0 | eliminated by the ×10 conversion rule | `commission`, `minInternalPriceChange` (`:212`, `:216`) |
| D-scale-1 | ≤ 10 engine units (< 1 legacy unit) apart because an integer division or a `double × int64` truncation runs at a different granularity | `hedgeOffsetPrice` (`:220`), `worsenPayupAmount` (`:225`), S-curve `offset` (`:786`), `cost / qty` (`:853`, `:857`), weighted mid (`:916`), plain mid (`:918`) |
| D-scale-2 | one ulp in an interpolated `double` before truncation (±1 unit) | `width` (`:445`) |

Status: **measured** (phase-3 gate, `mm_quote` 200 000 cycles x 2 sides = 400 000 quote
evaluations). The engine and the legacy transcription were fed identical random markets; the
oracle runs at the legacy 1e-8 scale, so a deviation is exactly a D2 effect.

| Field | Exact | Sub-tick (≤24 units) | Decision breaks | Max deviation |
|---|---|---|---|---|
| mid | 89.2360 % | 10.7640 % | **0** | **9** units (0.9 at 1e-8) |
| width | **100 %** | 0 | 0 | 0 |
| established level | **100 %** | — | 0 | 0 (quantity domain) |
| internal price (unrounded) | 77.8298 % | 22.1702 % | **0** | **9** units |
| **external price (sent)** | **100 %** | 0 | **0** | **0** |
| **internal size** | **100 %** | — | **0** | 0 |
| **external size** | **100 %** | — | **0** | 0 |
| **"must re-send" decision** | **100 %** | — | **0** | 0 |

Queue model enabled (`queueImportance = 3`, **200 000** cycles x 2 sides, 0..3 resting orders):

| Field | Exact | Sub-tick (≤64 units) | Decision breaks | Max deviation |
|---|---|---|---|---|
| internal price (unrounded) | 70.1817 % | 29.8183 % | **0** | 24 units |
| **external price (sent)** | **100 %** | 0 | **0** | 0 |
| **internal size** | **100 %** | — | **0** | 0 |
| **external size** | **100 %** | — | **0** | 0 |
| **"must re-send" decision** | **100 %** | — | **0** | 0 |

Interpretation:

- The **only** deviating computation is the midpoint's integer division —
  `(ask.pr*bidSize + bid.pr*askSize) / (bidSize + askSize)` (`:916`) and its plain sibling
  `(bid.pr + ask.pr) / 2` (`:918`). Scaling the numerator by ten does not commute with the
  truncation, so the engine's mid can sit up to 9 units (0.9 legacy units = 9e-9 $) away.
- That deviation propagates into the *unrounded* internal price, and disappears completely at
  the tick rounding: the **emitted** external price, both sizes and the re-send decision are
  identical in every one of the 400 000 evaluations. D-scale-1 is therefore *visible but not
  observable* in any decision.
- **D-scale-2 (width) did not materialise** with these inputs: `spread / 2.0 * widthPct` at the
  ten-times scale rounds to the same integer every time in the sweep. It stays in the taxonomy
  as a precaution (a `widthPct` that lands within one ulp of a truncation boundary would show
  it) but there is no evidence it occurs.
- Because every decision field is 100 % identical, the escalation rule below was **not**
  triggered: `kLegacyTruncation` stays `false` and the engine keeps pure 1e-9 semantics.

### 3.1 The `kLegacyTruncation` valve

`types.hpp` exposes `kLegacyTruncation` (default **false**). When flipped, the six truncating
sites run their intermediate value through `(x / 10) * 10`, which reproduces the legacy 1e-8
truncation exactly and makes the engine bit-identical to legacy at those sites.

It may only be flipped with a recorded gate run, and this section must then state which field
forced it and what the diff rate was before/after — the same discipline `rv` applied in its
§1a FMA decision.

## 4. Floating-point association (F)

Nothing outstanding on the quoting path. Hazards that were checked, with the outcome:

**F1. Hoisting `1 / establishedLevel` out of the per-order loop.** Legacy divides per order
(`:784`); the engine multiplies by a reciprocal computed once per `price_of()` call. Measured
with the queue-model gate: `MM_HOIST_EST_DIV=OFF` reproduces the baseline **exactly**
(26 371 exact / 13 629 sub-tick / 0 breaks, identical maxima), so the hoist is free and stays ON
by default.

**F2. `MM_FAST_EXP`: the D6 experiment.** A 5th-order polynomial `2^k · exp(r)` replaces the libm
call in the S-curve. Measured over the same 40 000 evaluations (20 000-cycle run):

| Build | internal price exact | decision breaks | max deviation | decision fields |
|---|---|---|---|---|
| `std::exp` (shipped) | 65.9275 % | 0 | 24 units | 100 % identical |
| `exp()` (MM_FAST_EXP=ON) | 65.8125 % | **4** | 84 units | 100 % identical |

The fast variant leaves every *decision* identical, but it is not clean on the intermediate price
(4 deviations per 40 000, i.e. inside the D3 budget but unexplained by the D-scale taxonomy). D6
says it ships "only if the gate stays clean", so **`std::exp` remains the default** and
`MM_FAST_EXP` stays behind the option for a latency-constrained deployment, with these numbers on
record. Its latency benefit is to be quantified by `mm_cycle_bench` in phase 7 before it is ever
enabled in production.

**F3. `-ffp-contract=off`** is applied to `mm/model.cpp` and `mm/quoting.cpp` for the reason
measured in `new_impl/rv/PARITY_NOTES.md` §1a (FMA contraction changes the last bit before the
`int64` truncation that produces a price).

## 5. Test-side defects found (O)

None yet. Standing rule from `rv` §5: whenever the oracle and the engine share a shortcut, one
of them must be derived from the legacy text instead of from the other, otherwise a
differential gate reports 100 % parity on a shared bug.
