# MarketMaker — performance-first re-implementation plan (`new_impl/mm`)

Status: **Phase 0 + Phase 1 complete** (model/config/tick-primitive layer, gate `mm_unit`
green) · Owner: TBD · Target: v1 = quoting + hedging core + host, as `new_impl/rv` did for RV.

This document plans and records a from-scratch, performance-first implementation of the
MarketMaker strategy. The reference implementation being replaced is
`src/SimpleMarketMaker.h` (1,803 lines) plus its resource wrappers
(`src/MarketMaker.h/.cpp`, `_MD.cpp`, `_FillHandling.cpp`, `_MessageHandler.cpp`, `_GUI.cpp`).

The goal is **lower per-cycle latency and a codebase that can be reasoned about**, not a
rewrite of the Viper framework. Everything off the per-cycle path (GUI, protobuf, config
grid plumbing, MD status handling, order transport) stays where it is.

---

## 0. Recorded decisions

| # | Decision | Value |
|---|---|---|
| D1 | v1 scope | Quoting/hedging core + `host` (composition root). GUI/protobuf/grid parameter plumbing stays legacy behind the adapter (§8) |
| D2 | Price scale | **1e-9 (`PRICE_SCALE`), matching `new_impl/rv`**, with the truncation deviations enumerated in §4, documented in `PARITY_NOTES.md` §3 and bounded by the differential gate |
| D3 | Parity standard | ≥ 99.99 % of cycles produce an identical decision; every remaining diff explained and logged (not "bit-exact") |
| D4 | Order-queue model | In v1 (quote price/size come from queue position, `:756-864`); the `queueImportance == 0` and `establishedLevel == 0` bypasses are preserved |
| D5 | Internal IME book | In v1, including `impliedQtyNotTrackedByQueuePosition` (subtracted from the level size in `targetSize`, `:795`) |
| D6 | `std::exp` | Exact `std::exp` shipped. The polynomial `MM_FAST_EXP` variant was measured (PARITY_NOTES §4 F2): every decision stays identical but it introduces 4 unexplained intermediate deviations per 40 000 evaluations, so per this decision it does not ship by default |
| D7 | Emission ownership | Decision + sizing + emission live in the engine; only the ring-buffer primitive stays behind `host_ports` |
| D8 | Concurrency | Single-threaded, cycle-driven; no atomics, no locks in the cycle |
| D9 | Legacy arithmetic | Transcribed exactly; optimizations only where provably bit-identical (RV F1 lesson: folding the hedge-cost expression cost 0.5 % parity) |
| D10 | Quantity signedness | Engine quantities are `int64_t`, matching the legacy `hf::qty_t` (`int_fast64_t`). `tachyon::qty_t` (unsigned) is used only at the order-port boundary (`wire_qty_t`), because positions, queue positions (`-1 = NOT_SET`) and hedge sizes are all signed in legacy |
| D11 | Order-side ownership | The resting-order queue keys each order by the side recorded when it was first seen, not by the side in a later message. Identical for a consistent feed; prevents a contradictory message from corrupting the other side's list |

---

## 1. What the legacy implementation is

| File | Lines | Role |
|---|---|---|
| `src/MarketMaker.h/.cpp` | 157 / 113 | resource; owns `SimpleMarketMaker[]`, `runCycle` over `_updatedContracts` |
| `src/MarketMaker_MD.cpp` | 236 | MD update/trade/status fan-in, `copyBooks`, `onMarketDataChange` |
| `src/MarketMaker_MessageHandler.cpp` | 127 | fills, `InternalBookUpdate`, shift, economic numbers, `OrderUpdate`, `QueuePositionUpdate` |
| `src/MarketMaker_FillHandling.cpp` | 54 | fill fan-out, manual fills |
| **`src/SimpleMarketMaker.h`** | **1803** | **the strategy**: quotes, hedges, queue model, fills, positions, limits, parameter grid |

Hot path: `MarketDataConsumer::onMDBatch` → `copyBooks` (`MarketDataConsumer.h:120-173`,
`memcpy` of a 10-level `md::top_t`) → dirty-set insert → `MarketMaker::runCycle`
(`MarketMaker.h:36-41`) → `SimpleMarketMaker::onMarketDataChange` (`:424-510`) →
`resetChangeIndices` (`:572-577`).

## 2. Hot-path problems in the legacy implementation

| # | Problem | Where | New implementation |
|---|---|---|---|
| P1 | `std::unordered_set<uint32_t>` dirty tracking: hash insert per MD batch, iterate + clear per cycle | `MarketMaker.h:20`, `MarketDataConsumer.h:34`, `MarketMaker_MD.cpp:234` | bitset + packed list (`cycle_state::mark_dirty`) |
| P2 | `unordered_map<orderId,OrderInfo>` + `unordered_map<price,vector<OrderInfo*>>` per side; heap traffic on every order/queue-position update; `find()` inside the price path | `:1745-1746`, `:1562-1642`, `:796` | flat slab arena + open-addressed `orderId→slot` + per-price intrusive list with tail (`order_queue`) |
| P3 | `std::exp` in the queue-position S-curve, per resting order per side per update | `:781` | reciprocal of the established level hoisted; `mm_fast_exp` behind a flag (D6) |
| P4 | Formatted `LOG`/`DLOG` inside the cycle | `:428`, `:632`, `:671`, `:820`, `:839`, `:845` | POD trace ring, no formatting in the cycle |
| P5 | One huge object per contract (`ConfigContract` with `std::string`/`std::vector` inline) | `:1682-1747` | immutable SoA `model` + small hot `contract_state` |
| P6 | 400-byte `md::top_t` copy per update | `MarketDataConsumer.h:158` | 24-byte levels, copy-on-change as today |
| P7 | Both sides + hedges recomputed for qty-only changes on non-quote levels | `:484-505`, `:603-628` | keep the gating, fuse into one pass per contract with identical emission order |
| P8 | `%` tick division and lot division | `:1792-1799`, `ViperConfig.h:85-87` | constant-tick dispatch + `fast_div` magic numbers |
| P9 | `int64` price math with `double` conversions | `:445`, `:781-786`, `:911-919` | transcribe exactly, optimise only bit-identical parts (D9) |
| P10 | FMA contraction flips the last bit before the `int64` truncation | compiler | `-O3 -march=native -fno-math-errno`, no `-ffast-math`, `-ffp-contract=off` on the price-math TUs |

## 3. Module layout

```
new_impl/mm/
  limits.hpp          caps (MAX_CONTRACTS, BOOK_LEVELS=10, MAX_ORDERS, ...)   [done]
  types.hpp           price scale, side, tick primitives, comparisons         [done]
  model.hpp/.cpp      immutable per-contract tables, built once               [done]
  config.hpp/.cpp     MMConfigs CSV + secdb reader, model_inputs              [done]
  book.hpp            L2 book + level change index + trade adjustment          [done]
  order_queue.hpp/.cpp arena + orderId hash + per-price list                  [done]
  state.hpp/.cpp      cycle_state: contract_state[] + dirty bitset             [done]
  quoting.hpp/.cpp    quote_engine: mid, width, EMA, updateQuote, size caps     [done, bypass paths]
  hedging.hpp/.cpp    hedger: hedge size/prices, payup, cross
  positions.hpp/.cpp  position_book: fills, offset, overfill, limits, exec token
  strategy.hpp/.cpp   strategy façade: run_cycle, apply_updates, on_md_change
  host.hpp/.cpp       composition root: ports, gating, disable, shift, econ numbers, ledger
  app/mm_replay.cpp   config-driven replay with deployment invariants
  tests/mm_tests.cpp           numeric + init gate                     [done]
  tests/mm_state_tests.cpp     book + order queue                     [done]
  tests/mm_quote_tests.cpp     pricing + differential gate            [done]
  tests/reference_oracle.hpp   transcription of the legacy quoting path [done, phase 3 part]
  tests/mm_cycle_bench.cpp     ns/cycle, per stage, zero-allocation assertion
  tests/mm_host_test.cpp       composition root behaviours
```

Class responsibilities map one-to-one onto legacy functions, so a parity review is a
mechanical diff:

| Class | Legacy source |
|---|---|
| `model` | `SimpleMarketMaker::initialize` `:192-236` |
| `book` | `md::top_t` + `getPriceLevelIndex` `:407-422` + `copyBooks` |
| `order_queue` | `_orders`/`_priceOrders` maintenance `:1562-1642` |
| `quote_engine` | `calculateMidPoint` `:911-919`, `calculateEstablishedLevel` `:921-942`, `updateQuote` `:661-738`, `calculateQuotePriceAndSizeByOrders` `:756-864`, size caps `:1748-1790`, join/leave `:133-141`, spray `:866-872`, `checkQuoteCross` `:512-537` |
| `hedger` | `isInPayUp` `:356-361`, `calculateHedgePrices` `:1147-1163`, `modifyHedgeSize` `:1119-1215`, `checkHedgeCross` `:539-570` |
| `position_book` | `onFill` `:1001-1100`, `takeOverFill` `:238-317`, `forceTakeOverFill` `:318-340`, `isWithinPositionLimits` `:1211-1225` |
| `strategy` | `onMarketDataChange` `:424-510`, `applyUpdates` `:1194-1210`, `start` `:1255-1277` |
| `host` | `MarketMaker` resource + `disable` `:1279-1295`, external-quoting gates `:363-373`, economic numbers `:1644-1671`, `InternalBookUpdate` |

## 4. Price scale and the deviation surface (D2)

Legacy works at 1e-8 (`Common.h:267 ToRhonePrice`); the engine works at 1e-9. The bridge is

```
to_mm_price(x) = (int64)(x * 1e8) * 10          // legacy truncation first, then an exact x10
```

**Not** `(int64)(x * 1e9)`. The difference is real and only shows on values with more than
eight decimals — exactly what `MMWorsenPayUpCondPct` (`0.65477777777`) holds: legacy
`65'477'777` ⇒ engine `654'777'770`, while a naive 1e-9 conversion yields `654'777'777`.
`mm_tests::test_to_mm_price_is_ten_times_legacy` pins this and sweeps 200 000 values.

With that rule every config-derived price is exactly 10× its legacy value, so
scaling-invariant arithmetic (+, −, ×, comparisons, `%` tick rounding) is decision-identical.
The complete remaining deviation surface is eight sites:

| # | Site | Legacy expression | Class |
|---|---|---|---|
| 1 | `:212`, `:216` (+ cold `:1459`, `:1475`) | `ToRhonePrice(double)` on `commission`, `minInternalPriceChange` | D-scale-0: eliminated by the ×10 rule |
| 2 | `:220` | `hedgeOffsetPrice = hedgeOffset * tickSize` | D-scale-1 |
| 3 | `:225` | `worsenPayupAmount = worsenPct * tickSize` | D-scale-1 |
| 4 | `:445` | `width = (price_t)(marketSpread / 2.0 * widthPct)` | D-scale-2 |
| 5 | `:786` | `offset = offsetScale * width` (implicit trunc) | D-scale-1 |
| 6 | `:853`, `:857` | `internalPrice = cost / qty` | D-scale-1 |
| 7 | `:916` | `(ask.pr*bidSize + bid.pr*askSize) / (bidSize + askSize)` | D-scale-1 |
| 8 | `:918` | `(bid.pr + ask.pr) / 2` | D-scale-1 |

**D-scale-1** = at most 10 engine units (`< 1` legacy unit = `1e-8 $`) apart: invisible after
tick rounding unless the legacy value sat within ~1e-8 of a tick boundary. **D-scale-2** = one
ulp in the interpolated `double` before truncation, i.e. ±1 unit.

Consequences to keep in mind in phases 3-6:
- Comparing two interpolation-derived prices is unaffected (both carry the same offset); only a
  comparison that mixes one with a raw config/book price can flip, inside that ≤10-unit window
  (e.g. `BetterPrice(applyPriceOffset(ref, -hedgeOffsetPrice), midPoint)` at `:1157`).
- `maxTicksWide`/2× (`:221`) and all quantity-domain maths (`getMaxQuoteSizeBasedOnMarket`,
  `getInternalQuoteSize`, the EMA, `toValidQty`) are immune: integer ticks × tick size and
  quantity arithmetic scale exactly.

**Safety valve.** `kLegacyTruncation` in `types.hpp` (default **false**, as decided) makes the
six truncating sites run `legacy_trunc(x) = (x / 10) * 10`, reproducing legacy bit-for-bit at a
cost of ≤6 extra divisions per contract-side-cycle. **Escalation rule:** if the differential
gate lands below D3's 99.99 % on `mid`/`quote price`/`quote size`/`hedge price`/`hedge size`/
`inPayUp`, flip the constant, re-run, and record which field forced it in `PARITY_NOTES.md` §3.

**Measured outcome (phase 3, `mm_quote`, 200 000 cycles x 2 sides).** Only the midpoint's
integer division deviates, by **at most 9 units at 1e-9 (0.9 legacy units = 9e-9 $)**; the
emitted external price, both sizes and the re-send decision were **identical in all 400 000
evaluations** (PARITY_NOTES §3). The escalation rule was therefore not triggered and the engine
keeps pure 1e-9 semantics. Note also that D-scale-2 (width) did not occur with these inputs —
the ten-times product rounded identically every time — so the taxonomy is currently stricter
than the measurement requires.

## 5. Semantics contract

Behaviours that must be reproduced exactly, each with its legacy reference. This table is the
checklist for `tests/reference_oracle.hpp`.

| # | Behaviour | Reference |
|---|---|---|
| S1 | `RoundToTickSize` integer semantics, bids floor / asks ceil, negative remainders included | `Common.h:1196-1245` |
| S2 | **Two** commission treatments: `applyPriceOffset` (bid +offset, ask −offset) *and* `roundToTickSize` folding `±commission` in before rounding | `Common.h:433-440`, `:1792-1799` |
| S3 | Weighted mid only when `bid + tick == ask`, with `min(size, establishedLevel)` per side; else `(bid+ask)/2` | `:911-919` |
| S4 | `width = (spread / 2.0) * widthPct`, truncated; recomputed only when the top book changes | `:444-446` |
| S5 | Established-level EMA: `alpha = dt/period`, `mu = exp(-alpha)`, `nu = (1-mu)/alpha`, `isfinite` guard, `max(est*pct, 1)`, seeded from `EstLevelEMA`, only on top-of-book qty change | `:921-942`, `:436-441` |
| S6 | Payup predicate `(book.q <= max(payUpQty, ourSize*payUpMultiple)) or (book.o <= payUpOrders)` | `:356-361` |
| S7 | Change detection `abs(internal - lastSentInternal) > minInternalPriceChange` (strict) plus size-change rules; candidate size may always decrease, increases must exceed one quote size | `:675-676`, `:741-753` |
| S8 | Join/leave thresholds with their `>=` / `<=` asymmetry | `:133-141`, `:700-716` |
| S9 | Quote/hedge cross prevention with `IsPriceBetterOrEqual`, forcing a re-send of the *other* instrument; hedge skipped while in payup | `:512-570` |
| S10 | Hedge size `-(internal + external position)`, always persist LMT with the payup flag; the opposite hedge is reset and sent as size 0 | `:1102-1215` |
| S11 | Fill allocation: offset against existing opposite positions internal-first then external; quote fills reduce `_internalSize`/`_externalSize`, overfill beyond the internal quote is booked external | `:1010-1100` |
| S12 | Disable paths: no MD ⇒ disable + external off + GTC off; 2× maxTicksWide ⇒ disable; maxTicksWide ⇒ external off + GTC off (latched); external overfill ⇒ external off; position limit breach ⇒ external off | `:375-394`, `:452-482`, `:1279-1295` |
| S13 | `toValidQty` on every emitted quantity | `ViperConfig.h:85-87` |
| S14 | `maxTicksWideBreached` latch and the immediate zero-quote emission on the transition | `:475-482` |
| S15 | Economic number: save/clear/restore `quoteExternal`, and `CanEnableUseGTC` on the end event | `:1644-1671` |
| S16 | Shift update changes only the active width percentage | `:1540-1543` |
| S17 | Stale MD rejected (`> lastMDbid` for batches, `>=` for internal book updates, which also reset both change indices) | `MarketMaker_MD.cpp:13-19`, `MarketMaker_MessageHandler.cpp:79-92` |
| S18 | Top-book changed ⇔ qty-change index is 0 on **either** side (both sides recalculate because the weighted mid may move) | `:436`, `:603-612` |

## 6. Phases and status

| Phase | Deliverable | Gate | Status |
|---|---|---|---|
| 0 | this document + `PARITY_NOTES.md` skeleton | review | **done** |
| 1 | `limits/types/model/config` + tick primitives, `to_mm_price` | `mm_unit` | **done** (green) |
| 2 | `book` + `order_queue` (arena, orderId hash, per-price list) | `mm_state` | **done** (green) |
| 3 | `quote_engine` bypass paths (`queueImportance == 0`, `establishedLevel == 0`, `!quoteExternally`) + `reference_oracle.hpp` | `mm_quote` (differential: mid, width, price, size, change flag) | **done** (green, 0 decision diffs) |
| 4 | S-curve queue-position path + spray (driven by the phase-2 `order_queue`) | `mm_quote` second differential (queue model on) | **done** (green, 0 decision diffs; D6 resolved by measurement) |
| 5 | `hedger` + `strategy` cycle assembly (cross checks, ordered intent log) | `mm_cycle` | **done** (green) |
| 6 | `position_book` + `host` (cycle driver, gates, maxTicksWide latch, deferral, fills, order/queue-position tracking) | `mm_host` | **done** (green). Deferred to phase 6b: the `liquidity_ledger` bridge, economic numbers, shift updates — flagged in §8 |
| 7 | `mm_cycle_bench`, `mm_replay`, final `PARITY_NOTES` | `mm_bench_smoke`, `mm_replay_smoke` | **done** (green; §7.1) |
| 8 | `adapter.hpp` + `request_sink` (framework-free seam) | `mm_adapter` | **done** (green). The legacy shim itself is specified in §8.1 but needs the Rhone toolchain to build |

## 7. Build and gates

```
cmake -S new_impl -B new_impl/build -DCMAKE_BUILD_TYPE=Release
cmake --build new_impl/build -j
cd new_impl/build && ctest -R mm_
```

Current gate (`mm_unit`, `mm/tests/mm_tests.cpp`):

- price scale invariants; `to_mm_price` ≡ 10 × legacy truncation over 200 000 values plus the
  `0.65477777777` trap;
- `round_to_tick` (constant dispatch and `fast_div`) == transcription of `RoundToTickSize` over
  every boundary and a strided sweep for nine tick sizes, negatives included;
- `to_valid_qty`;
- every derived field of `model::build` against the legacy expression, including the two
  commission treatments, the unbounded `maxTicksWide` rule and the EMA period;
- the legacy validation rules (zero quote size, non-positive worsen-payup, zero tick, queue
  importance clamp, contract cap);
- a parse of the real `config/backtest/model.csv` and `secdb.cme.csv`, including the quoted
  `details` JSON that precedes `order_increment_qty` (the lot column).

Later gates follow the RV precedent: a differential oracle transcribed from
`SimpleMarketMaker.h`, ≥ 99.99 % identical decisions with every diff explained, plus a property
test per numeric convention (RV's O1/O3/O5 lesson: two copies of the same shortcut agree while
both disagree with legacy), and an ordered-request-sequence comparison because ordering is part
of the observable behaviour (cross-check re-emissions, spray insertion, opposite-hedge reset).

## 7.1 Measured performance and replay (phase 7)

`mm_cycle_bench 200000 5 8` (8 contracts, 200 000 cycles x 5 runs):

| Stage | ns / cycle | cycles @3 GHz |
|---|---|---|
| full host cycle (dirty set → pricing → quotes → hedges → cross → emit) | **272.8** | 818 |
| pricing block only (EMA, mid, width) | 111.9 | 336 |
| quotes + hedges + cross checks, no host | 188.2 | 565 |
| allocations in the timed region | **0** | — |

The allocation count is the hard invariant; the 1000 ns budget is advisory (it assumes an unloaded
production-class CPU). The host path costs ~85 ns over the engine-only path — the dirty-set walk,
the gates, the `maxTicksWide` block and the port calls.

`mm_replay config/backtest 200000 7` (real MMConfigs + secdb, 6 contracts, deterministic tape of
book updates, fills, end-of-fills and enable/disable toggles) asserts, over 200 040 cycles and
561 744 emitted requests:

- every emitted external price is a tick multiple, and every removal is NULL-priced with zero size;
- the emitted book never crosses (bid < ask) for any contract;
- every emitted size is non-negative and lot-aligned;
- after each end-of-fills the hedge equals `|net position|` **and** sits on the side the position
  needs (a short book hedges with a bid);
- no stale batch, no intent-log overflow, and `intents_emitted` equals what reached the sink.

## 8. Integration seam

`new_impl/mm/adapter.hpp` is the object the legacy host talks to. It owns the model, the cycle
state and the host, and it is **framework-free** — no Viper, boost or protobuf header — so it is
built and gated here (`mm_adapter_tests`) while the legacy side only writes a thin shim.

```cpp
class adapter {                       // one per strategy resource, all contracts
    adapter(const mm_model_cfg&, const secdb_table&, double cycles_per_second,
            uint32_t initial_shift, request_sink&);

    bool on_market_data(contract, side, const market_data_view&);   // levels + change numbers + bid
    bool on_internal_book(contract, side, const level*, count, batch_id);
    void on_order_update(contract, order_id, price, qty, qp, side);
    void on_queue_position_update(contract, order_id, qp);
    fill_outcome on_fill(contract, const fill&);
    void end_of_fills(contract);
    void run_cycle(now_cycles);

    void enable(bool); void set_external_quoting_allowed(bool); void set_venue_enabled(v, bool);
    void set_can_send_quotes(bool); void set_contract_can_trade(c, bool);
    void set_contract_enabled(c, bool); void set_shift(uint32_t);
};
```

`request_sink` is the transport seam: `send_quote`, `send_hedge`, `notify`, `cancel_orders`,
`alert`, `report_fill`. The adapter implements `host_ports` with trampolines onto it, so ordering
is preserved end to end and a test can record the exact stream (which `mm_adapter_tests` does).

### 8.1 What the legacy shim has to do (and nothing else)

| Legacy event | Shim call |
|---|---|
| `MarketDataConsumer::copyBooks` (after the change numbers are known) | `on_market_data(c, sd, {levels, LEVEL_NUM, priceNum, qtyNum, batch.bid})` |
| `InternalBookUpdate` message | `on_internal_book(c, sd, book.l, LEVEL_NUM, msg._lastMDbid)` |
| `Fill` message | map the enums (below) → `fill` → `on_fill` |
| `EndOfFill` message | `end_of_fills(c)` |
| `OrderUpdate` / `QueuePositionUpdate` | `on_order_update` / `on_queue_position_update` |
| `ShiftUpdate` | `set_shift(msg._number_1)` |
| operator Enable/Disable, `QuoteExternallyOn/Off`, per-venue gate | `enable`, `set_external_quoting_allowed`, `set_venue_enabled` |
| executor transaction open/closed | `set_can_send_quotes(bool)` |
| `runCycle(now)` (the resource's cycle) | `run_cycle(now)` |

Enum mapping for the fill (legacy `Common.h:963-979` → `positions.hpp`):

| `FillType` | `fill_type` | | `FillSource` | `fill_source` |
|---|---|---|---|---|
| `Internal` | `internal` | | `Quote` | `quote` |
| `External` | `external` | | `Hedge` | `hedge` |
| `Phantom` | `phantom` | | `Overfill` | `overfill` |
| `NegativeScalpCheck` | `negative_scalp_check` | | `MissedHedge` | `missed_hedge` |
| | | | `SelfMatch` | `self_match` |
| | | | `ViperStart` | `viper_start` |
| | | | `Manual` | `manual` |
| | | | everything else | `other` |

`fill.from_other_strategy` is `msg._strategy != StrategyId::MarketMaker`; `force_over_fill` and
`check_limit` map straight through. `request_sink` maps onto
`OrderRequestManager::sendQuoteRequest` / `sendHedgeRequest` / `notifyOrderRequest` /
`sendCancelRequest`, `sendErrorMessage(Critical)` and `sendFillToFront`.

The legacy `MarketMaker` keeps the GUI (`populateGUIInitialization`, `sendStateToGUI`) and the
`handleParametersUpdate` grid plumbing — cold, and not worth re-implementing. The parameters that
the engine does own (`set_shift`, `set_contract_enabled`, the venue gate) are setters above.

**Limitation, stated plainly:** the shim itself is not in this repository build — the legacy target
needs the full Rhone/vcpkg toolchain, which is not configured here. Everything on the engine side
of the seam is compiled and gated; the shim is a ~100-line translation table that the mapping
above specifies unambiguously.

## 9. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | The S-curve `exp` is the largest numeric hazard (float, per order) | exact `std::exp` first; any fast approximation is gated and documented (D6) |
| R2 | Legacy order-list semantics are subtle (`orders.back()->qp`, `NOT_SET == -1`, insertion order, spray at the tail) | phase-2 tests pinned directly to `:814-851` before the quoting engine consumes it |
| R3 | Emission order is behaviour, not an implementation detail | the gate compares the ordered request sequence, not just final prices |
| R4 | `kLegacyTruncation` could mask a real bug if flipped without evidence | flipping requires a recorded gate run in `PARITY_NOTES.md` §3 |
| R5 | Shared cap `MAX_ORDERS` can be exceeded by a pathological spray pattern | init-time sizing from the config + cold overflow path (disable contract + alert), never an allocation |
| R6 | Duplicated price primitives between `rv` and `mm` could drift | they are byte-identical apart from the namespace; consolidation into `common/price.hpp` is scheduled for phase 7 and is behaviour-preserving |



