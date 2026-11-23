# Executor + implied engine — performance-first re-implementation plan (`new_impl/exec`)

The order-flow spine of the deployment: three strategies push order requests into three mailboxes,
one implied engine per contract drains them, collapses each stream to its **last** request, matches
what can match internally, and sends only the **remainder** to the exchange.

```
   MM --push--> mailbox[MM] --+
   RV --push--> mailbox[RV] --+--> executor::run_cycle()
   AS --push--> mailbox[AS] --+          |
                                         +- drain each mailbox once, route by contract
                                         +- flush each touched contract:
                                         |    apply staged -> cross -> residual to exchange -> publish
                                         +- fan fills out, close the batch with end_of_fills
```

## 0. Recorded decisions

| # | Decision | Value |
|---|---|---|
| D-EX1 | Location / style | `new_impl/exec`, namespace `tachyon::trade::exec`, framework-free (no Viper/boost/protobuf), C++20, `-O3 -march=native`, no `-ffast-math` -- same as `mm`/`rv`/`as` |
| D-EX2 | Queue ownership | **one mailbox per strategy** (3 total), fixed capacity, power of two, POD entries; drained by the executor, never by the strategies |
| D-EX3 | Collapse policy | per request **key** `(strategy, kind)` from one compile-time table; see §2 |
| D-EX4 | Threading | the mailbox is a template on `sync_mode::single` (no atomics: live single-thread loop, backtest, replay; **default**) or `sync_mode::spsc` (one producer thread, one consumer thread, release/acquire, cache-line separated cursors). Both are instantiated in every build (`exec/executor.cpp`) |
| D-EX5 | Match cadence | one cross per contract per drain; a FIFO request flushes the staged set first, which preserves the legacy per-stream ordering |
| D-EX6 | Deferred surface | quote/fill/hedge/throttle banners, GTC reconcile, pay-up matching (`matchPayUpOrder`), economic numbers, IOP tracking, rate checkers, `maxTicksOffTopLevel`. These are **external order management**, and they are being built as `new_impl/om` behind `exchange_gateway` -- phase 1 (quote ban, hedge ban, `maxTicksOffTopLevel`) is done, see `om/PLAN.md` |
| D-EX7 | Quantities | `int64_t`, matching the legacy signed `hf::qty_t` (same as `mm`/`as` D10) |
| D-EX8 | Parity standard | transcribed function by function from `InternalMatchingEngine.h`, then **proved** against an independent transcription of the legacy engine: zero mismatches over 20 000 steps (~1.06 M field comparisons, §8) |
| D-EX9 | Abstraction cost | the hot path is POD writes and index math: no virtual call and no allocation per request. Abstract interfaces are crossed once per emitted order or fill batch |
| D-EX10 | Dropped legacy field | `OrderRequest::Hedge::_stf` is not carried: the new strategies do not produce it (it only fed a fill-message field on the front end) |
| D-EX11 | Price collisions | legacy `internalOrderMap.emplace(price, &order)` silently keeps the **first** order at a price, so a second order at that price can never match internally. `price_collision::first_wins` reproduces it; the shipped default is `all_matchable` (ties break by arrival) |
| D-EX12 | Pay-up | `cross` does not run the pay-up pre-pass; it belongs to D-EX6 |
| D-EX13 | Aggressor attribution | when one flush applies requests from several strategies, the fill price is attributed to the **latest** strategy in the batch (`fill_price = bid_strategy == aggressor ? ask : bid`). Single-strategy cycles -- the normal case -- are exact |
| D-EX14 | Self-crossed book | when a non-AS self match aborts the cross, the contract is taken **out of the external market** (both residuals become NULL/0) until a cross succeeds again. Without it the engine would leave the crossed pair resting on the exchange; legacy does the same via `disableContractExternalQuoteOnAllStrategy` (:1928) |
| D-EX15 | Emission order | when both sides of a residual move in one flush, the two messages are ordered so a crossed pair never exists, not even between them: if the bid is not getting worse the ask is emitted first (legacy `synchronizeOrders`, :524-541). Found by `exec_replay`, which is exactly why that driver asserts it |

## 1. What the legacy implementation is

| File | Role |
|---|---|
| `src/OrderRequestManager.h` | per `(contract, orderId, side)` ring; `sendQuoteRequest`/`sendHedgeRequest`; `notifyOrderRequest` pushes a message into the executor ring |
| `src/Executor.h:456-556` | drains that ring and calls `ime.processOrderRequests(strategy)` per notification |
| `src/InternalMatchingEngine.h:1903-2026` | the drain: **collapse to last** or consume one at a time, per `(strategy, kind)`; stale MM token; disabled-strategy drop |
| `src/InternalMatchingEngine.h:1076-1230` | `matchOrders`: the internal cross, self-match handling, AS bookkeeping |
| `src/InternalMatchingEngine.h:881-957` | `tryMatch`: the FAK path for RV/AS hedges |
| `src/InternalMatchingEngine.h:835-869` | `getPriceAndSize` + `getPersistedPrice`: the aggregated **residual** |
| `src/InternalMatchingEngine.h:543-607` | `synchronizeQuote` + `sendHedge`: the residual leaves for the exchange |
| `src/InternalMatchingEngine.h:1232-1318` | `sendInternalMarketData` + `sendInternalFills` |

## 2. The collapse table (D-EX3)

Transcribed from `InternalMatchingEngine.h:1952-1971`, where the test is on the legacy order id
`orderId(strategy, kind)`:

| strategy | kind | legacy id | policy | apply | rests | published source |
|---|---|---|---|---|---|---|
| MM | quote | 1 | **last wins** | rest | yes | `MM_QUOTE` |
| MM | hedge | 0 | **last wins** | rest (LMT) | yes | `MM_HEDGE` |
| RV | quote | 3 | **last wins** | rest | yes | `RV_QUOTE` |
| RV | hedge | 2 | one at a time | FAK | no | -- |
| AS | quote | 5 | one at a time | rest + cross | yes | `AS_QUOTE` |
| AS | hedge | 4 | one at a time | FAK | no | -- |

Why the asymmetry: MM/RV quotes are *state* -- only the newest matters, and applying all of them
would flicker the order ("skip to the last request, to avoid order flickering", `:1956`). AS quotes
and RV/AS hedges are *events* -- each one has to be applied and crossed in order (`:1967` "not skip
AS quote because the Aggressive Quote adjustment").

## 3. Layout and responsibilities

```
exec/
  limits.hpp               caps and the static assertions the design relies on
  types.hpp                side/kind/strategy vocabulary, the per-key table, the port payloads
  order_request.hpp        64-byte POD (static_asserted) + the two legacy builders
  request_queue.hpp        request_queue<sync_mode>: reserve/publish/drain, back-pressure
  implied_book.hpp/.cpp    per-contract book: apply/remove, cross, aggress, best/external queries
  implied_engine.hpp/.cpp  per-contract pipeline: stage -> flush -> cross -> residual -> publish
  ports.hpp                exchange_gateway + strategy_endpoint (+ recording implementations)
  executor.hpp/.cpp        composition root: mailboxes, engines, registries, run_cycle
  adapters/                mm/rv/as -> mailbox
  tests/                   the gates and the benchmark
  app/exec_replay.cpp      deterministic request tape -> pipeline -> invariants
```

| Class | Responsibility | Legacy source |
|---|---|---|
| `request_queue<Mode>` | the mailbox: `reserve`/`publish`/`drain`, full-ring counting | `RingBufferQueue.h`, `OrderRequestManager` |
| `implied_book` | one contract's internal book and its matching | `_bids`/`_asks`/`_bidLevels`/`_askLevels`, `matchOrders`, `tryMatch`, `modifyOrder` |
| `implied_engine` | one contract's pipeline and its emission | `processOrderRequests`, `synchronizeQuote`, `sendHedge`, `sendInternalMarketData`, `sendInternalFills` |
| `basic_executor<Mode>` | drain, route, flush, fan out, gates | `Executor::processMessages` + `OrderRequestManager` |
| `exchange_gateway` | where the residual goes | `OrderManager` / `sendExternal*` |
| `strategy_endpoint` | what a strategy is told | `sendFillToStrategy`, `sendMarketUpdateToStrategy`, `onEndOfFills` |
| `mm_request_sink`, `rv_adapter`, `as_adapter` | port -> mailbox | the `sendQuoteRequest`/`sendHedgeRequest` call sites |

## 4. Data structures and the measured cost

| Structure | Legacy | New | Size |
|---|---|---|---|
| mailbox entry | union `OrderRequest` | 64-byte POD, one cache line (`static_assert`) | 1024 x 64 B = 64 KB per strategy |
| request key -> order | `std::map<price, Order*>` + `unordered_map` | direct index `(strategy*2 + kind)*2 + side` | no lookup |
| internal book | two `std::map`s + two level maps with `std::vector<Order*>` per price | 12-slot array; best-of-side is a 6-element scan | ~700 B per contract |
| residual bookkeeping | `PriceLevel` + `Order*` levels | one 3-word snapshot per side | 0 |
| invalidation | `_notifyInternalMarketChange` + per-strategy caches | per-side dirty mask + per-(subscriber, side) change detection | 0 |

### Measured cost

Wall clock, `exec_bench`, **min of 7 runs** on this (shared) machine, `-O3 -march=native`. Single
samples swing by up to 40 % here, so the bench is the tool to run on a production-class box:

| Stage | min over 7 | Note |
|---|---|---|
| mailbox push (reserve + fill + publish) | **6.0 ns** | single-threaded mode |
| empty cycle (drain 3 mailboxes, nothing pending) | **9.5 ns** | the floor of `run_cycle` |
| mixed steady state, 8 contracts x 4 requests/cycle | **53.9 ns per request** | what `exec_bench` reports |
| crossing cycle (2 requests, one fill) | **379 ns** | the full match path |
| allocations inside the cycle | **0** | asserted by `exec_engine_tests`, `exec_bench` and `exec_replay` |

Wall clock on a shared machine cannot attribute a change, so the optimisation pass was driven by
**deterministic work counters** instead: build with `-DEXEC_PROFILE` and `work_counters` counts the
slot visits of every hot loop. Same tape (8 contracts x 4 requests/cycle), before and after:

| Loop | before | after | What changed |
|---|---|---|---|
| `best_in_map` | 36 | **12** | a per-side membership bitmask, so the walk visits occupied slots only |
| `best_for` / `best_pair` | 12 | **10** | same mask, both sides in one walk |
| `level_find` | 12 | **4** | each order caches its level index; `level_drop` rewrites the indices of the level it relocates |
| `apply` / `cross` / `emit` | 15 | 15 | unchanged |
| **total per contract-cycle** | **75** | **41** | **-45 %**, which shows up as 92.6 -> 53.9 ns per request |

Both caches are validated by the differential gate after *every* step (`mask_consistent`,
`level_indices_consistent`), so a drifting bitmask or a stale level index fails `exec_match` rather
than hiding. What remains, in order: `best_in_map` 12 (two walks per cross), `emit` 8 (the external
level scan), `best_for` 10, `apply` 4, `cross` 3. Going below this means maintaining incremental
per-subscriber tops and a cached external top -- the two pieces deliberately left out of the first
version, now measurable against a proven reference.

## 5. Semantics contract (pinned by the gates)

| # | Behaviour | Reference | Gate |
|---|---|---|---|
| S1 | the collapse table of §2 | `:1952-1971` | `exec_queue_tests` |
| S2 | stale MM exec-token drop, except a hedge with no external price | `:1980-1993` | `exec_engine_tests` |
| S3 | disabled-strategy drop | `:1996` | `exec_engine_tests` |
| S4 | a resting order enters the book only with a price **and** a size | `:350-355` | `exec_book_tests` |
| S5 | fill price = the passive side's price, chosen by the aggressor | `:1143` | `exec_book_tests`, `exec_engine_tests` |
| S6 | internal cross first, exchange second; the residual is the external leg after the cross | `:1076-1211`, `:835-852` | `exec_engine_tests` |
| S7 | an exhausted order keeps matching at its own external price | `:1158-1211` | `exec_book_tests` |
| S8 | both orders' external sizes are reduced by every internal fill | `:1043-1056` | `exec_book_tests` |
| S9 | non-AS self match: alert + disable external quoting + threshold | `:1127-1138`, `:1926-1933` | `exec_engine_tests` |
| S10 | AS self match allowed, reported per side | `:1129`, `:1225-1228` | `implied_book::cross` |
| S11 | FAK: match internally, then the remainder goes out (or comes back as MissedHedge) | `:1868-1901`, `:2028-2043` | `exec_engine_tests` |
| S12 | the published internal top excludes the subscriber's own liquidity and lean-on-RV orders | `:1249-1272` | `exec_engine_tests` |
| S13 | one `end_of_fills` per strategy per batch | `:1310-1318` | `exec_engine_tests` |
| S14 | price collisions (D-EX11) | `:381-384` | `exec_book_tests` |
| S15 | a self-crossed book takes the contract out of the external market (D-EX14) | `:1928`, `:559` | `exec_engine_tests`, `exec_replay` |
| S16 | the residual messages of one flush are ordered so a crossed pair never exists (D-EX15) | `:524-541` | `exec_replay` |

## 6. Gates and how to run them

| Phase | Binary | What it proves |
|---|---|---|
| 0 | `exec_queue_tests` | POD layout, the collapse table, FIFO/wraparound/back-pressure, `single` == `spsc` |
| 0 | `exec_spsc_stress` | one producer thread + one consumer thread, 2 M requests, no loss, no reorder (TSan target) |
| 1 | `exec_book_tests` | apply/modify/remove, persisted prices, both collision policies, external-leg continuation, FAK `aggress` |
| 2/3 | `exec_engine_tests` | last request wins, cross then residual, self match, FAK, stale/disabled gates, publication rules, cancel, routing, determinism, zero allocation |
| 4 | `exec_adapter_tests` | the real `mm::request_sink` / `rv::host_ports` / `as::host_ports` mapping, field by field |
| -- | `exec_bench` | ns per stage + the zero-allocation invariant |
| -- | `exec_replay` | a deterministic tape through the real pipeline: nothing lost, no crossed residual resting, no allocation |

```bash
cmake -S new_impl -B new_impl/build -DCMAKE_BUILD_TYPE=Release -DEIGEN3_INCLUDE_DIR=<eigen>
cmake --build new_impl/build -j16
ctest --test-dir new_impl/build -R exec_ --output-on-failure
./new_impl/build/exec_bench 20000 5 8 4

# the cross-thread mailbox instead of the default:
cmake -S new_impl -B build_spsc -DEXEC_QUEUE_SPSC=ON -DEIGEN3_INCLUDE_DIR=<eigen>
cmake --build build_spsc -j16 --target exec_queue_tests exec_engine_tests exec_book_tests

# ThreadSanitizer on the spsc mailbox:
cmake -S new_impl -B build_tsan -DCMAKE_CXX_FLAGS=-fsanitize=thread -DEIGEN3_INCLUDE_DIR=<eigen>
cmake --build build_tsan -j16 --target exec_spsc_stress && ./build_tsan/exec_spsc_stress 2000000
```

## 7. Not in this phase

| Item | Where it belongs |
|---|---|
| banners (quote/fill/hedge/throttle), `maxTicksOffTopLevel` | `new_impl/om`: they gate **external** orders, not internal matching |
| GTC reconciliation, pay-up matching | `new_impl/om` (pay-up is `matchPayUpOrder`, `:972-1041`) |
| economic numbers, IOP tracking, rate checkers | executor-level policy, unchanged from today |
| -- | (done) the differential gate against a legacy transcription | **green, see §8** |
| a fair-consistent tape for AS in the integration driver | **done**: RV and AS now load the *same* model file (`AS_model.csv` is a full RV model), so ids, tick grids and fairs line up, and the driver warm-starts the strategies' internal books and short-term fairs. On 3 000 cycles the loop closes with 12 137 internal fills (AS 12 013, MM 124) |
| incremental per-subscriber tops | only if the measured publish cost becomes the bottleneck (PLAN §4) |

## 8. The differential gate

`exec/tests/match_oracle.{hpp,cpp}` is an independent transcription of the legacy matching engine
that keeps the legacy containers (`std::map` keyed by price, a per-price `PriceLevel` holding an
incrementally built `std::vector<Order*>`) and the legacy control flow. `exec/tests/exec_match_tests`
pushes one random request tape through both and diffs **eleven observables after every step**: slot
price/size/external leg/map membership, the aggregated external top, the persisted price, the
published internal best per subscriber, the ordered fill list, the cross flags and the FAK result.

The gate is **exact and green**: zero mismatches over 20 000 steps (~1.06 M field comparisons) and
five further seeds. It runs as `ctest -R exec_match`.

What it found -- eight real defects, four in the engine and four in the oracle, none of which the
hand-written gates had caught:

| # | Finding | Side | Fix |
|---|---|---|---|
| F1 | Legacy keeps an order in the internal map after its size reaches zero, and erases it when a cross reaches it (`:936-940`) | engine | map membership modelled separately from having size (`order::in_map`, **D-EX16**) |
| F2 | Legacy returns *before* touching either side when one map is empty (`:1079-1087`), so it never burns a dead entry then | engine | same early-out on map emptiness |
| F3 | Legacy drops an order from the map at the end of a cross whose external leg was still in play (`:1206-1211`) | engine | end-of-cross cleanup |
| F4 | `best_for` treated `internal_top::empty()` (a *size* test) as "no candidate yet", so a dead best entry was overwritten by a worse price | engine | explicit "have a candidate" flag |
| F5 | `as_self_match` was computed and never copied into the result | oracle | fixed |
| F6 | A map entry could survive a modify (stale pointer) | oracle | fixed (erase the entry's own order) |
| F7 | Legacy's `tryMatch` counts a self match on a *dead* own entry and keeps it; a dead entry of another strategy is erased | engine | branch order mirrored |
| F8 | `PriceLevel` is sorted once at insertion and never re-sorted, so the size it reports is its **head's current** size, not the largest | engine | the level list is modelled directly (`level_add`/`level_remove`, ~40 lines) |

Two deviations are therefore deliberate and documented, and the comparator handles them explicitly:

| # | Deviation | Why |
|---|---|---|
| D-EX16 | map membership (`in_map`) is modelled separately from having size, and a dead entry is erased as soon as a cross reaches it | the alternative -- keeping dead entries around indefinitely -- is what leaks a size-0 price into the published book; the engine reproduces the *reachable* legacy state exactly, which the gate proves |
| D-EX17 | no zero-size fill records | legacy emits a 0-lot fill when `tryMatch` reaches a dead entry (`:929-938`); a zero-size trade is never reported to a strategy, and the gate ignores those records |

The instrumentation is permanent and cheap: `exec_match_tests <steps> <seed> [trace_until]` prints,
for every step in the window, both books and both external level lists, and on the first divergence
the complete request history as a paste-ready array. That is what turned each of the eight findings
above from a guess into a two-minute diagnosis.

## 9. The integration driver and what it does not model

`exec/app/exec_pipeline.cpp` wires the three real engines through the executor: `mm::adapter` ->
`mm_request_sink`, `rv::host` -> `rv_adapter`, `as::host` -> `as_adapter`, and the engine's fills and
internal book back in through `mm_bridge` / `rv_bridge` / `as_bridge`. It asserts the deployment
invariants a live run depends on (nothing lost, fills delivered == fills produced, no crossed
residual resting, the MM never published an internal market) and runs as `ctest -R exec_pipeline`.

Two things it deliberately does not model, both of which make a *strategy* gate itself out rather
than the engine misbehave:

| Symptom | Cause | Where it is covered properly |
|---|---|---|
| `RV MaxTicksThroughMarketCheckFailed` (2 cycles, then RV disables itself) | RV prices a contract off its *lean* (`price_without_cost_cap`); the synthetic tape drives every contract from one walk, so a lean on a contract with a different scale trips the bound. The driver already floors the configured bound (a configured 0 rejects every quote) | `rv_host_test` for the gate, `rv_parity` for the pricing |
| `AS Unable to hedge. No valid path.` (AS disables itself after filling) | AS hedges into its lean contracts and needs *internal* liquidity there; the engine's internal book only holds what the three strategies quote, so some leans are unfilled | `as_hedge_tests` for the hedge stage |

Neither affects the engine invariants: the driver reports them explicitly and the run still proves
that requests flow, match, and come back as fills (12 137 of them). Modelling a lean-consistent tape
for both strategies is a *replay* concern, not an engine one, and it is the natural next step if the
driver is to be used as a strategy benchmark rather than a plumbing gate.

## 10. The multi-threaded shape

`sync_mode::spsc` exists for the deployment where each strategy owns a thread. The shape is:

```
  strategy thread MM  --submit-->  mailbox[MM]  --.
  strategy thread RV  --submit-->  mailbox[RV]  --+-->  executor thread: run_cycle()
  strategy thread AS  --submit-->  mailbox[AS]  --'
```

One producer per ring, one consumer for all three, no locks: the producer side is a `reserve` +
release-store, the consumer drains a published prefix with an acquire load (see `request_queue`).

`exec/tests/exec_threaded_tests.cpp` (ctest: `exec_threaded`) runs exactly that shape and asserts it:

| Invariant | How it is checked |
|---|---|
| nothing is lost | requests drained == requests pushed == 3 x N |
| nothing is reordered / no stale read | every request overwrites its (strategy, kind, side) slot, so the final book must equal **the last request each producer pushed** -- a missing release/acquire shows up as a stale price or size in that comparison |
| fills survive concurrency | fills delivered to the endpoints == fills produced by the engines |
| the exchange leg stays sane | no crossed residual pair, and no price published with no size |
| back-pressure is real | the mailbox is deliberately small (1024) and the run counts 10k-26k producer retries per strategy |

Verified at 20 k / 200 k / 500 k requests per strategy and at 1 / 3 / 8 / 64 contracts, three runs
each. Alerting on a sustained overflow counts **episodes** (rising edges), not cycles, so a strategy
fighting a full mailbox raises one alert per episode rather than one per cycle.

**ThreadSanitizer could not be run here**: this machine has no TSan runtime (`libtsan.so.0.0.0` is
missing and the link fails), so the recipe is documented instead of claimed:

```bash
cmake -S new_impl -B build_tsan -DCMAKE_CXX_FLAGS=-fsanitize=thread -DEXEC_QUEUE_SPSC=ON
cmake --build build_tsan -j --target exec_threaded_tests exec_spsc_stress
exec_threaded_tests 200000 8 && exec_spsc_stress 2000000
```

### The return path (`feedback.hpp`)

The engine calls `strategy_endpoint` on the *engine* thread. With one thread per strategy that would
be a cross-thread call into a thread-confined host, so the return direction gets the same treatment as
requests: a per-strategy SPSC ring, `strategy_link<Mode>`, carrying a 48-byte `feedback_event`.

```
  engine thread    --post-->  link[MM]  --> strategy thread: link.drain(link_dispatch(endpoint))
                   --post-->  link[RV]         ... on_fill / on_internal_market / end_of_fills /
                   --post-->  link[AS]             on_hedge_done / on_as_match_done / disable
```

Two glue pieces keep both sides unchanged: `endpoint_via_link` *is* a `strategy_endpoint` that posts
(so it is registered with the executor instead of the real endpoint), and `link_dispatch` replays a
drained event onto the real endpoint. The link also carries the **enabled mirror** an `std::atomic<bool>`
the engine reads before routing work to a strategy (`:1996`), flipped immediately by `disable()` so the
engine stops routing at once rather than at the strategy's next drain.

`exec/tests/exec_feedback_tests.cpp` (ctest: `exec_feedback`) proves:

| Invariant | How |
|---|---|
| routed mode == direct mode | the same 500-cycle tape through two executors, one with the real endpoints and one with `endpoint_via_link`; the flattened event streams must be byte-identical (they are: 14 529 characters of events per strategy) |
| the enabled mirror is immediate | `disable()` is visible to the engine before any drain, and the strategy can re-enable from its own thread |
| overflow is reported, not hidden | a 4-event ring with a stalled drain counts exactly the 6 events that did not fit |
| the two-thread shape works | engine posts on one thread, strategy drains on another; drained == published, nothing overflowed |

With that, a live multi-threaded cutover has both directions: `mailbox` strategy -> engine and `link`
engine -> strategy, each an SPSC ring with one producer.

## 11. Where the OM layer hooks in

Nothing in this document changed when `new_impl/om` arrived. The engine still emits one quote intent
per (contract, side) whose external top moved and one hedge intent per hedge request; the OM sits on
the *other* side of `exchange_gateway`:

```
  implied_engine --> exchange_gateway --> om::bannered_gateway --> venue
                                          (bans, max ticks off top level)
```

so the engine stays allocation-free and oblivious, and the gates get the two things they need that the
engine must not own: the external book top and a clock. `om/tests/om_banner_tests.cpp` pins the last
property that makes the cutover safe -- with the gates configured off, the decorated gateway forwards
every intent byte for byte (800 quotes on a 200-cycle tape), so the layer can be introduced **before**
it is enabled and then switched on per contract.
