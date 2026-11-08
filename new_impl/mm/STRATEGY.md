# How the MarketMaker strategy works

This document explains the MarketMaker (MM) strategy that `new_impl/mm` implements: what it does
on every cycle, why each step exists, and which legacy function each piece was transcribed from.
It is written for someone who has to operate, review or extend the engine — the companion
documents are `PLAN.md` (decisions and phases), `PARITY_NOTES.md` (measured deviations) and
`new_impl/rv/` (the same treatment for the relative-value strategy).

Legacy references of the form `:123-456` are lines in `src/SimpleMarketMaker.h`.

---

## 1. What the strategy is for

The MM quotes a two-sided market on a set of contracts it is configured to market-make, and keeps
itself flat by hedging whatever position those quotes accumulate. Three ideas carry all of it:

1. **Quote around a fair value, at a width that pays for the risk.** The fair value is the mid of
the book; the width is half the spread scaled by a per-shift percentage. The quote therefore sits
*inside* the spread, a fixed fraction of the way from the touch to the mid.
2. **Prefer queue position to price.** Given a target price, the strategy asks where *its own*
resting orders are in the queue. Orders at the front of the queue are worth quoting a tick more
aggressively than orders at the back: a fill is worth more when fewer lots are ahead of you. The
queue position therefore moves the price continuously through an S-curve (see §4.3).
3. **Never carry risk you did not choose.** Everything the quotes accumulate is hedged by a single
resting order sized to the *net* position, repriced as the market moves, and explicitly cancelled
when the position returns to flat.

Two positions are tracked separately, and the distinction matters throughout:

| Position | Origin | Example |
|---|---|---|
| **internal** | we traded with ourselves through the internal matching engine | our bid quote is filled by another strategy's sell |
| **external** | we traded with the real market | our bid quote is hit on the exchange |

The hedge is sized on `internal + external`, but the split decides how a fill is booked (§7).

---

## 2. One cycle in one page

```
market data arrives (levels + which level changed + batch id)
        │
        ▼
┌─ dirty set ──────────────────────────────────────────────────────────┐
│ one entry per contract, deduplicated, cleared at the end of the cycle │
└──────────────────────────────────────────────────────────────────────┘
        │  for each dirty contract
        ▼
1. gates:  strategy enabled? contract tradable? book complete?
2. pricing (only if the top of book changed):
       established level (EMA) → mid-point → width
3. safety: spread > 2×maxTicksWide → disable contract
           spread >   maxTicksWide → external quoting off, both quotes pulled
4. quotes (per side):
       ordinary price = mid ∓ width
       → target price = round to tick, snapped to the touch
       → walk our resting orders by queue position → internal + external price + size
       → payup? (thin book) → chase the touch instead
       → size = f(position room, pctTob × displayed size, join/leave hysteresis)
       → emit only if the price moved more than minInternalPriceChange, or the size changed
5. cross checks:   quote/quote, then hedge/quote — one instrument may force the other out
6. hedges (per side, when we are carrying a position):
       size = −(internal + external), price from the opposite quote/touch, or chase the touch
7. emit the ordered request list to the transport, then one notification
```

Steps 4–6 run in that exact order, and the order is part of the behaviour: the cross checks
re-emit the *other* instrument, and a hedge flip resets the opposite hedge (§6, §8).

---

## 3. Inputs and state

### 3.1 What arrives from the feed

One snapshot per side per update, plus two small integers and a batch id:

| Field | Meaning |
|---|---|
| `level[0..9]` | price, size, order count and the *implied* size not tracked by queue position |
| `price_num`, `qty_num` | which level changed, **1-based**, with `0` meaning "this field did not change" |
| `batch_id` | monotonic; an update whose batch is not newer than the last applied one is **stale** and dropped |

Two derived facts drive the whole cycle:

* the **top of book changed** iff the *quantity* change index is 0 on either side (both sides then
  recalculate, because the size-weighted mid depends on both);
* the **tracked size** of a level is `size − impliedNotTrackedByQueuePosition`: liquidity that never
  entered our queue cannot be ahead of us, so it is not counted when we ask "how far back am I?".

The snapshot is copied only when the quantity index moved (`price_num != 0, qty_num == 0` records the
index but leaves the stored book alone).

### 3.2 What the contract carries between cycles

| Group | Fields |
|---|---|
| books | two sides × 10 levels, plus the two change indices |
| pricing | `mid_point`, `width`, active `width_pct`, `ema_est_level`, `last_tob_qty`, `last_ema_time`, `established_level` |
| quotes | per side: internal/external price and size, plus the **last values actually sent** |
| hedges | per side: internal/external price, `size_to_quote`, `in_payup`, last sent price |
| positions | `internal_pos`, `external_pos` |
| flags | `enabled`, `quote_external`, `max_ticks_wide_breached`, `contract_can_trade`, `gateway_active` |

The *last sent* prices are what the change detection compares against, so they are state, not
scratch: an order that was never emitted must never be remembered as sent.

---

## 4. Pricing: from two book sides to a target quote

### 4.1 Established level — how deep the queue usually is

`established_level` is a smoothed estimate of the typical top-of-book size. It is an exponential
moving average over the *average of the two top sizes*, and it exists for one reason: when we ask
where our order sits in the queue, the answer is a **fraction** of the level, and that fraction has
to be normalised by something.

```
alpha = elapsed_cycles / est_level_ema_period        // period from EstLevelEMASeconds
mu    = exp(-alpha)
nu    = (1 - mu) / alpha
ema   = mu*ema + (nu - mu)*last_tob_qty + (1 - nu)*qty
established_level = max(trunc(ema * EstLevel%EMA), 1)
```

Three details are load-bearing and are reproduced exactly:

* the EMA only advances when **both** sides show size; the first observation only seeds the clock;
* the recursion uses the elapsed-cycle ratio rather than a per-sample constant, so the estimate is
  independent of how often the book updates;
* the result is truncated (not rounded) and clamped to at least 1 — a fresh session quotes against
  an established level of **1** until the second top-of-book change.

### 4.2 Mid-point and width

```
if bid.price + tick == ask.price:                 // spread is exactly one tick
    mid = (ask.price*bid_size + bid.price*ask_size) / (bid_size + ask_size)
else:
    mid = (bid.price + ask.price) / 2

width = trunc((ask.price - bid.price) / 2 * width_pct)
```

The weighted branch is deliberate: with a one-tick spread there is no "middle" price, so the mid is
pulled towards the **heavier** side (a large bid raises the mid). Both sizes are clipped to
`established_level` so one outsized print cannot drag the fair value away.

`width_pct` is the percentage for the **current trading shift** (Asia / Europe / US / Safe mode); a
shift change swaps it and nothing else.

### 4.3 The target price, and why queue position matters

The ordinary price is the mid moved *away* from the touch by the width:

```
bid: price = mid - width          ask: price = mid + width
target     = round_to_tick(price)          // bids floor, asks ceil
target     = min(target, bid touch)        // a bid may never be better than the touch
```

Rounding is done with the commission folded in first (`price − commission` for a bid, `+
commission` for an ask), which is one of the two places the commission appears — the other is the
price-offset helper, which moves a *bid up* and an *ask down*.

If we are already resting at the target price, the price we send is not `target` but the
**queue-position-weighted average** of what our own orders would be worth:

```
pct_through_queue = clamp(1 - queue_position / established_level, 0, 1)
offset_scale      = 2 / (1 + exp(-queue_importance * (pct_through_queue - 0.5))) - 1
offset            = trunc(offset_scale * width)          // in price units
internal_price    = apply_offset(price, offset)          // bid +offset, ask -offset
external_price    = round_to_tick(internal_price)
```

The logistic is centred on the middle of the queue: at `queue_position = established_level/2` the
offset is exactly zero, at the front it is `+offset_max` (quote more aggressively) and at the back
`−offset_max` (quote less aggressively, because a fill there is worth less). `queue_importance` (0…10)
controls how steep that is; `queue_importance = 0` disables the whole mechanism.

The walk over our resting orders, all at the **target price**, is:

1. no orders there → assume we would join at the **back of the level** (`qp = tracked level size`);
2. otherwise walk them in insertion order, and for each order compute its price from its own queue
   position (`qp = NOT_SET (-1)` also means "back of the level");
3. an order whose rounded price is no longer better-or-equal to the target stops the walk: it (and
   everything behind it) is unwanted;
4. if the last order in the level has at least `quoteGap` lots of room behind it, add one more
   "spray" order at the back, sized by the same rules as an external quote;
5. the price we send is the **size-weighted average** of the accepted orders' internal prices
   (`cost / qty`), with the total accepted quantity as the size.

Finally the result is compared back to the target, and there are three outcomes:

| Condition | Internal price sent | Size |
|---|---|---|
| the target is *better* than what we could achieve | the ordinary price | **0** (show the level, no size) |
| our price rounds to a *different* tick than the target | the ordinary price | the accumulated size |
| our price rounds onto the target | the weighted internal price | the accumulated size |

The middle row is subtle: if the queue says we should be a tick away from the target, the strategy
keeps the target as the price and sends the size there, rather than quoting at the worse level it
computed.

### 4.4 Sizes

Everything is derived from three numbers: the **position room**, the **market cap** and the
**configured quote size**.

```
market cap   = trunc(pctTob * displayed size at the level)      // pctTob = "percent of top of book"
               (unlimited when the quote is not at/through the touch)
room_bid     = maxPosInternal - internal_pos        (0 when the cap is reached)
room_ask     = maxPosInternal + internal_pos
internal size = lot_floor(min(room, market cap))

external size = lot_floor(min(quoteSize, room_external, market cap))
```

The external size has one extra rule — **the increment rule**: a *decrease* is always allowed, but
an *increase* smaller than one quote size is suppressed while the quote already sits above
`min(quoteSize, market cap)`. The point is to stop the external size oscillating by a lot or two
with every book update, which would mean a modify storm on the exchange.

Two hysteresis thresholds control whether we are in the book at all:

* **join** — with no external size resting, we join only once the displayed size at our price is at
  least `joinSize`;
* **leave** — with size resting, we pull it when the displayed size drops below `leaveSize`.

`joinSize = 0` therefore means "always join", and the asymmetric thresholds stop us flickering in
and out of a level that sits exactly on the boundary.

### 4.5 Payup — when the book is too thin to wait

If the opposite side of *our own* book is thin, queue position is worthless: there is nothing to
queue behind and the fill will happen without us. The strategy then abandons the width and chases
the touch:

```
threshold    = max(payUpQty, trunc(our_size * payUpMultiple))
is_payup     = (opposite_size <= threshold)  or  (opposite_order_count <= payUpOrders)

internal_price = worse_of(ordinary_price, touch ∓ worsen_payup_amount)
external_price = round_to_tick(internal_price)
```

In payup the hedge (and the quote) is re-sent **unconditionally** every time it is evaluated — there
is no change detection, because the point is to be at the touch as fast as possible.

`MMWorsenPayUpCondPct` is how far past the touch we are willing to go, in ticks; the configuration
layer rejects a non-positive value.

### 4.6 Change detection — what actually gets sent

A quote is emitted only when

* `|internal_price − last_sent_internal_price| > minInternalPriceChange` (strictly greater), or
* the internal or external size changed, or
* the caller forces it (position change, queue-position update, enable).

The comparison is against the last price **sent**, not the last price computed: a book wobble that
never reached the exchange must not suppress the next real move.

---

## 5. Hedging: one order, sized to the net position

### 5.1 Size

```
hedge_size = -(internal_pos + external_pos)
```

A positive number means we are short and must **buy**: that is a **bid** hedge. A negative number
means we are long and must **sell**: an **ask** hedge. Only one side ever carries size, and the
flip is explicit: the new side is sent first, then the old side is sent with size 0.

### 5.2 Price

```
if payup(hedge_size, opposite book):
    external_price = opposite touch                        // chase it
    internal_price = touch ± commission                  // (bid: +, ask: -)
else:
    ref            = more aggressive of (our quote on the opposite side, opposite touch)
    internal_price = better_of(ref ∓ hedgeOffsetPrice, mid)   // a bid ≥ mid, an ask ≤ mid
    external_price = round_to_tick(internal_price)
```

So a buy hedge prices from the ask side (our ask quote or the ask touch, whichever is more
aggressive), steps back by `hedgeOffset` ticks, and is never worse than the mid; a sell hedge mirrors
it. The mid clamp is what stops the hedge from chasing a market that has already run away.

### 5.3 When it is re-sent

* payup → always;
* otherwise when the internal price moved more than `minInternalPriceChange`, or the payup state
  flipped.

A hedge keeps its last reconciled size between fills — this is why a hedge emitted on a market-data
update may not match the current position: the size is only recomputed when fills end.

---

## 6. Cross checks: nothing we send may cross anything else we have

Two checks run every cycle, and both can force the *other* instrument out:

| Check | Fires when | Forced re-emission |
|---|---|---|
| **quote / quote** | exactly one side's quote changed, and the *other* side's last sent price would cross it | the untouched side is re-sent with its existing price |
| **hedge / quote** | exactly one of (hedge, opposite quote) changed, and they now touch or cross | if the hedge moved → the quote is re-sent; if the quote moved → the hedge is re-sent |

A hedge **in payup is exempt** from the hedge/quote check: it is chasing the touch deliberately, so
crossing the opposite quote is intended.

---

## 7. Positions and fills

A fill is classified by two attributes — where it came from (`quote` / `hedge` / an overfill) and
whether it traded internally or externally — and the allocation is:

```
quote fill, internal type:
    offset against an opposite-signed position first (internal, then external)
    remaining = min(remaining, our internal quote size)      // the rest is booked EXTERNAL
    internal_pos += remaining  (signed by side)
    our internal quote size -= remaining

quote fill, external type:
    offset first, then book the rest as external exposure
    external_pos += remaining (signed by side), our external quote size -= size

hedge fill:
    offset against the existing positions; whatever is left OPENS the position
    the allocation is reported with the fill's sign (so a sell hedge reports negative)
```

Two behaviours are easy to get wrong and are both deliberate:

* **The displayed external size clamps at zero.** If the exchange fills more than we thought we were
  showing (possible with a racing modify), the size becomes 0 and an error is logged; the position
  still takes the whole fill.
* **A quote overfill becomes external exposure.** An internal fill larger than our displayed internal
  size means we were showing it internally and got filled beyond it; that excess is external risk.

### Overfills

An overfill (a fill we did not ask for, typically another strategy handing us its risk) is taken as
**external** exposure, up to `maxPosExternal`. The remainder is not silently swallowed: the legacy
design offers it to RV/AS, and when nobody takes it the MM takes it anyway and **stops quoting
externally**, so it can never keep accumulating unseen risk.

---

## 8. Safety and gating

| Gate | Effect |
|---|---|
| strategy disabled | the cycle runs but emits nothing |
| contract not tradable (halt, expiry, gateway down) | no quoting, no hedging |
| book incomplete (either side has no price) | the contract is **disabled**, external quoting and GTC are turned off, and an alert is raised |
| spread > `maxTicksWide` | external quoting off + GTC off, and both quotes are **pulled with explicit zero-size requests**; the latch clears when the spread returns |
| spread > 2 × `maxTicksWide` | the contract is disabled outright and its orders are cancelled |
| position limit breached | external quoting off (the internal quote is still allowed) |
| economic number (news) | `quote_external` is saved, cleared, and restored when the event ends |
| canSendQuotes = false (executor transaction open) | the cycle is deferred and the contract stays dirty, so the change is emitted on the first cycle that can send |

The `maxTicksWide` latch is worth spelling out because it is state, not a predicate: on the
*transition* into the breach the two quotes are pulled explicitly, and while it is latched no quote
is recalculated at all. That is what keeps a wide, illiquid market from producing hundreds of
modifies a second.

---

## 9. Parameters, and what each one does

One row of the `MMConfigs` section in the model CSV per market-made contract
(`config/backtest/model.csv` shows the shape):

| Column | Effect |
|---|---|
| `enabled` | initial per-contract enabled flag (configs usually ship `FALSE`; the operator enables at run time) |
| `quoteExternal` | may this contract quote on the exchange (also needs the strategy- and venue-level gates) |
| `quoteSize` | the size of one external quote, and the quantum of the increment rule |
| `quoteGap` | how much room behind our last order in the level is needed before a spray order is added |
| `quotePctTob` | cap on both sizes as a fraction of the displayed size at the level |
| `AsiaWidthPercent` / `EuroWidthPercent` / `USWidthPercent` / `SafeModeWidthPercent` | `width_pct` per trading shift |
| `hedgeOffset` | how many ticks the hedge steps back from the reference price |
| `joinSize` / `leaveSize` | display thresholds to join / leave a level (hysteresis) |
| `payupQty` / `payupOrders` / `payupMultiple` | the payup trigger (§4.5) |
| `maxPosInternal` / `maxPosExternal` | position limits; also cap the quote sizes |
| `commission` | folded into every rounding and every price offset |
| `minInternalPriceChange` | the price-change threshold for re-sending a quote or hedge |
| `EstLevelEMA` / `EstLevelEMASeconds` / `EstLevel%EMA` | the established-level EMA (§4.1) |
| `maxTicksWide` | the spread band; negative means unbounded |
| `MMWorsenPayUpCondPct` | how far past the touch a payup quote may go |
| `queueImportance` | steepness of the queue-position S-curve (0 disables it; values above 10 are clamped to 0) |
| `internalPosition` / `externalPosition` | positions restored from the previous session |

---

## 10. Conventions that everything depends on

* **Prices are fixed-point `int64` at 1e-9.** Configuration values are converted through
  `to_mm_price`, which truncates at the legacy 1e-8 scale and then multiplies by exactly ten, so
  every configured price is exactly ten times its legacy value (see `PARITY_NOTES.md` §3).
* **Quantities are signed `int64`.** Positions, queue positions and hedge sizes all go negative;
  an unsigned type would silently invert three comparisons.
* **Tick rounding is integer arithmetic**: a bid floors, an ask ceils, and negative prices round
  away from zero correctly. It is never done in floating point.
* **Truncation, not rounding**, wherever the legacy code truncated: `width`, the queue offset, the
  weighted price, the established level.
* **`NOT_SET` queue position is `-1`**, and it means "assume the back of the level" — not "zero".

---

## 11. Worked examples

All examples use a 0.025 tick (= 25 000 000 units) and `commission = 0.00001` (= 10 000 units).

### 11.1 Mid, width, and the rounded bid

Book: bid **60 000 000 000** (2 400 ticks), ask **60 050 000 000** (2 402), `width_pct = 0.1`.

```
spread      = 50 000 000                       (2 ticks)
mid         = (60 000 000 000 + 60 050 000 000) / 2 = 60 025 000 000
width       = trunc(50 000 000 / 2 * 0.1)      = 2 500 000
bid ordinary= 60 025 000 000 - 2 500 000       = 60 022 500 000
round(bid)  = floor((60 022 500 000 - 10 000) / 25 000 000) * 25 000 000
            = floor(2400.8996) ticks = 2400 ticks = 60 000 000 000   → the bid touch
```

So with this width the bid quote lands on the touch (and would be snapped there if it had rounded
better than the touch).

### 11.2 The weighted mid on a one-tick spread

Book: bid 2 400 ticks × 300 lots, ask 2 401 ticks × 100 lots, `established_level = 200`.

```
bid_size = min(300, 200) = 200      ask_size = min(100, 200) = 100
mid = (ask*200 + bid*100) / 300 = 60 016 666 666
       16 666 666 above the bid, 8 333 334 below the ask  → pulled towards the heavier bid
```

### 11.3 The queue-position S-curve

`queue_importance = 3`, `established_level = 64`, `width = 2 500 000`:

| Queue position | Fraction through the queue | Offset applied to the price |
|---|---|---|
| 0 (front) | 1.00 | **+1 587 872** (bid more aggressive) |
| 32 (middle) | 0.50 | **0** (exactly the ordinary price) |
| 64 (back) | 0.00 | **−1 587 872** (bid less aggressive) |
| 1 000 (beyond the level) | clamped to 0 | −1 587 872 |

`0.635148952 × 2 500 000 = 1 587 872.4`, truncated to 1 587 872 — the truncation is part of the
parity contract.

### 11.4 Hedge from a long position

Long 10 lots: `hedge_size = -10` → an **ask** hedge of 10. With our bid quote at 60 022 500 000 and
the bid touch at 60 000 000 000, `hedgeOffset = 1` tick, mid 60 025 000 000:

```
ref            = max(our bid quote, bid touch) = 60 022 500 000
ref - 1 tick   = 59 997 500 000
better ask     = min(59 997 500 000, mid) = 59 997 500 000    (an ask wants the lower price)
round(ask)     = ceil((59 997 500 000 + 10 000)/25 000 000) * 25 000 000
               = ceil(2399.9004) ticks = 2400 ticks = 60 000 000 000
```

An aggressive sell at the bid — which is exactly what it should be to get out of a long on a thin
book.

---

## 12. Where each piece lives

| Behaviour | New code | Legacy origin |
|---|---|---|
| market data, dirty set, stale batches | `host.cpp` `on_market_data`, `on_internal_book_update` | `MarketDataConsumer.h:120-222`, `MarketMaker_MD.cpp:7-19` |
| change indices, level lookup, tracked size | `book.hpp` | `:407-422`, `:795` |
| established level, mid, width | `quoting.cpp` `refresh_market`, `mid_point`, `update_established_level` | `:436-446`, `:911-942` |
| S-curve | `quoting.cpp` `price_at_queue_position` | `:779-788` |
| queue walk, spray, weighted price | `quoting.cpp` `price_of` | `:756-872` |
| quote sizes, joins, payup | `quoting.cpp` `internal_quote_size`, `external_quote_size`, `candidate_external_size`, `enough_size`, `in_payup` | `:105-141`, `:356-361`, `:741-753`, `:1748-1790` |
| change detection, emission | `quoting.cpp` `build_quote`, `emit_quote` | `:661-739`, `:878-891` |
| quote/quote cross check | `quoting.cpp` `cross_quote` | `:512-537` |
| hedge size, price, payup, re-send | `hedging.cpp` `reconcile_size`, `hedge_prices`, `update_price` | `:1102-1215`, `:1147-1163`, `:630-659` |
| hedge/quote cross check | `strategy.cpp` `cross_hedge` | `:539-570` |
| cycle order | `strategy.cpp` `on_md_change`, `apply_updates`, `force_update` | `:484-509`, `:1194-1210`, `:579-601` |
| fills, overfills, limits | `positions.cpp` | `:238-340`, `:944-1100`, `:1211-1225` |
| gates, latching, cancel, alerts | `host.cpp` `run_cycle`, `has_market_data`, `disable_*` | `:375-394`, `:452-482`, `:1279-1295` |
| order queue (arena + per-price lists) | `order_queue.{hpp,cpp}` | `:1562-1642` |
| the transport seam | `adapter.hpp`, `intents.hpp` | `OrderRequestManager`, `modifyQuoteOrder`, `modifyHedgeOrder` |

## 13. Things that look like bugs and are not

| Behaviour | Why it is correct |
|---|---|
| A quote is sent with **size 0** at a price | the strategy is showing the level it wants without taking risk there — the queue said it cannot defend the target price |
| The hedge size does not match the position | the size is only recomputed when a fill cycle ends; an md-driven reprice keeps the last size |
| An *unchanged* cycle can still emit | `apply_updates` forces both quotes out (that is what a position change is for); only the hedge is price-gated |
| The external size sits below `quoteSize` | it is capped by the position room and by `pctTob × displayed`, and increases are suppressed by the increment rule |
| The bid quote can be at a *worse* price than the mid − width would suggest | queue position, or the payup branch deliberately chasing the touch |
| Two commission treatments | one moves a bid up / an ask down (`applyPriceOffset`), the other is added *before* tick rounding; both are legacy and both matter |
| An order with quantity 0 can exist | the order book records what the exchange reports; a size-0 update is how a cancel is described on some paths |
| `queue_position = -1` behaves like "back of the level" | not "zero", and not "unknown": it means we joined and nothing told us otherwise |

## 14. Where the liquidity ledger fits (a note)

The shared `new_impl/common/liquidity_ledger` is **not used by the MM engine**, and that is
faithful: legacy MM performs no reservation accounting (the `Market`/FAK-adjustment machinery in
`Common.h` is RV/AS only). MM sizes against raw displayed size.

MM matters to the ledger on the *provider* side. An MM hedge is the only hedge that is a **resting
LMT** order, so it becomes part of the internal market other strategies lean on; the internal book
carrying our hedge is tagged `LIQUIDITY_SOURCE_MM_HEDGE` so their reservations can be source-scoped.
That tagging is done where the internal market is wired (the executor/adapter side), not inside the
MM engine.

Making MM *reservation-aware* — sizing from `available()` instead of the raw display — would be a
behaviour improvement rather than a re-implementation, and is therefore proposed as an opt-in flag
(`MM_LEDGER_AWARE_SIZING`, off by default) with its impact measured by the existing differential
gate.

## 15. How this document is kept honest

Every behaviour above is pinned by a gate rather than by prose:

| Claim | Gate |
|---|---|
| price scale, tick rounding, lot flooring, config derivation | `mm_unit` |
| book semantics, order queue, queue positions | `mm_state` |
| pricing, queue walk, sizes, payup, change detection | `mm_quote` (differential vs a legacy transcription at 1e-8: **0 decision differences** over 400 000 evaluations) |
| hedge sizing/pricing, cross checks, request ordering | `mm_cycle` |
| fills, overfills, limits, gating, latching | `mm_host` |
| latency and the zero-allocation invariant | `mm_bench_smoke` |
| deployment invariants on a real config | `mm_replay_smoke` |
| the transport seam | `mm_adapter` |

If you change a behaviour described here, one of those gates should fail — and if it does not, the
gate is the thing that needs fixing first.
