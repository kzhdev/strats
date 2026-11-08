# How the ArbieSpreader (AS) strategy works

This document explains the strategy itself: what it trades, how it decides a price and a size, what
it does with a fill, and which parts of the shipped configuration actually exercise each mechanism.
It is deliberately about *behaviour*, not about the re-implementation — for the engine's design and
its parity evidence see `PLAN.md` and `PARITY_NOTES.md` next to this file.

Line references (`:123`) point at the legacy implementation in `src/ArbieSpreader.h` /
`ArbieSpreader*.cpp` so that every statement here can be checked against the code that runs today.

---

## 1. The idea in one paragraph

AS does not decide "I think QMQ2-QMV2 is worth 60.00". It decides **"the basket that reproduces
QMQ2-QMV2 currently costs 0.198 to buy"** and quotes that basket's price on the spread contract.
Each quoting contract owns a small set of **paths**; a path is a signed list of **lean contracts**
with integer multipliers, e.g.

```
path "QU+UV"  =  +1 × (QMU2-QMV2)  +1 × (QMQ2-QMU2)
```

The path price is the sum of its leans' best prices (each net of that contract's external
commission when the lean is on the external book), plus the path's **fair-value offset**, minus the
path's **commission**. The best path's price, minus the contract's **edge**, is the quote. The size
is whatever the tightest lean can actually support, capped by the contract's quote size.

So AS is a *relative-value quoter*: it takes liquidity on a spread only when the components can be
bought (and immediately hedged) at a price that leaves the edge. It never holds a position
voluntarily — every fill is hedged leg by leg out of the same books the path leaned on, or handed
to RelativeValue when the path is a "switch path".

---

## 2. Vocabulary

| Term | Meaning |
|---|---|
| **quoting contract** | A contract with an `ASConfigs` row. Only these are quoted. |
| **lean contract** | A contract whose book a path consumes. Any contract in the model can be a lean. |
| **multiplier** | Integer units of a lean in a path: `+2` means two of it, `-1` means short one. |
| **basket** | The set of leans of one path, with their multipliers. |
| **path price** | `Σ multiplier × lean price`, then `+ fair offset`, then `∓ path commission`. |
| **edge** | How far *inside* the basket price the quote is placed — the strategy's margin, in dollars. |
| **path commission** | The cost of doing the basket, applied per path (config, per path). |
| **external commission** | `asExtCommission` from `PricingConfigs`, charged when the lean trades externally. |
| **hedge offset** | An extra payup per lean when hedging, scaled by the lean's tick size. |
| **fair offset / STF offset** | `Σ ratio × short-term fair` of the basket's residual exposure. |
| **switch path** | A path whose fair offset is non-empty: its fill is handed to RV rather than hedged. |
| **best book** | Of a contract+side: the internal (IME/RV) or external book, whichever is better. |
| **adjusted size** | Displayed size minus what AS itself has already reserved on that book. |
| **aggressive quote** | A quote priced through the opposite side of the same contract's external book. |
| **FAK adjustment** | The reservation a hedge places on the lean's book until the hedge order completes. |

---

## 3. The configuration

Three inputs define a deployment.

### 3.1 `ASConfigs` (a section of the model CSV)

One row per quoting contract, from `config/minimal_cme/AS_model.csv`:

```
ASConfigs,enabled,quoteExternal,quoteSize,edge,hedgeOffset,maxTicksThroughMarket
QMQ2-QMU2,FALSE,TRUE,3,0.0005,0.0004,100
QMQ2-QMV2,TRUE,TRUE,7,0.0005,0.0004,100
```

| Column | Effect |
|---|---|
| `enabled` | Whether the contract quotes at all (its paths still exist and still price). |
| `quoteExternal` | Whether the external price may be published (also gated by the strategy and venue). |
| `quoteSize` | The maximum internal quote size. |
| `edge` | Dollar margin, multiplied by `ASEdgeMultiplier` from `GlobalConfigs`. |
| `hedgeOffset` | Payup used when hedging, scaled by the *contract's* tick size. |
| `maxTicksThroughMarket` | Quote validity bound, scaled by the contract's tick size. |

Two derived numbers are easy to misread, because legacy multiplies a config double by the tick size
*in price units* (`:61-62`), so the result is the **dollar product**:

```
QMQ2-QMV2   tick          = 0.025                              (inherited from its first leg)
            edge          = 0.0005 × 3  (ASEdgeMultiplier, US)  = $0.0015
            hedgeOffset   = 0.0004 × 0.025                      = $0.00001
            maxTicks      = 100     × 0.025                     = $2.5
            extCommission = 0.001  (PricingConfigs)             = $0.001
```

### 3.2 `AS_paths.csv`

Repeating blocks, one block per quoting contract. The first header cell names the quoting contract,
the next cells are the candidate leans, and the last two are `Commission` and `Enabled`:

```
QMQ2-QMV2,QMQ2-QMV2,QMU2-QMV2,QMQ2-QMU2,QMV2-QMX2,QMQ2-QMX2,QMU2-QMX2,Commission,Enabled
QU+UV,0,1,1,0,0,0,0.01,TRUE
QX-VX,0,0,0,-1,1,0,0.01,TRUE
QV,1,0,0,0,0,0,0.01,TRUE
QU+UX-VX,0,0,1,-1,0,1,0.01,TRUE
```

Read the second row: the path named `QU+UV` holds `QMU2-QMV2 +1`, `QMQ2-QMU2 +1`, everything else
zero, commission `$0.01`, enabled.

Every path definition becomes **two path instances**, one per side (bid and ask), so a contract with
4 paths has 8 path instances.

### 3.3 Which contracts exist (ids matter later)

Contracts are numbered with the `PricingMatrix` columns first, then the remaining model rows. For
`config/minimal_cme`:

| id | contract | | id | contract |
|---|---|---|---|---|
| 0 | QMQ2 | | 4 | QMQ2-QMU2 |
| 1 | QMU2 | | 5 | QMU2-QMV2 |
| 2 | QMV2 | | 6 | **QMQ2-QMV2** (quoted) |
| 3 | QMX2 | | 7 | QMQ2-QMX2 |
| | | | 8 | QMU2-QMX2 |
| | | | 9 | QMV2-QMX2 |

Spreads carry legs, e.g. `QMQ2-QMV2 = QMQ2 − QMV2`. That matters in §6: fair-value netting works in
terms of legs, not spreads.


---

## 4. The best book: what a lean is priced at

A lean is never priced off "the market"; it is priced off **the better of its two books** for the
side the path needs (`:313-341`):

1. if the internal book (IME / RV's own quotes) has no size → the external book;
2. else if the external book has no size → the internal book;
3. else the better price wins (higher bid / lower ask);
4. on equal prices the **smaller** size wins, and a tie goes to the internal book.

If the winner is the **external** book, the price is adjusted by that contract's external commission
(`:270-275`, per side: a bid lean is netted down, an ask lean is grossed up). The internal book
carries no commission.

Sizes are not "displayed size" but **adjusted size**: displayed size minus what AS itself has already
reserved — aggressive-quote reservations on the opposite book, hedge reservations on the lean's own
book. A path can therefore shrink simply because AS is already using that liquidity.

---

## 5. The path price, worked

```
price(path) = Σ multiplier × leanPrice  +  fairOffset  −  commission
```

with the side applied at the end: a bid path subtracts the commission, an ask path adds it.

### 5.1 A one-lean path

Contract `QMQ2-QMV2`, path `QV` = `+1 × QMQ2-QMV2`, commission `$0.01`; its own books are
`60.00 / 60.25` internally.

| | bid path | ask path |
|---|---|---|
| lean price (best book, internal) | 60.00 | 60.25 |
| multiplier | ×1 | ×1 |
| fair offset | 0 | 0 |
| path commission | −0.01 | +0.01 |
| **path price** | **59.99** | **60.26** |

### 5.2 A two-lean path with an external lean

Path `QU+UV` = `+1 × QMU2-QMV2` `+1 × QMQ2-QMU2`, commission `$0.01`, on the bid:

| lean | best book | price | adjustment | contribution |
|---|---|---|---|---|
| QMU2-QMV2 (id 5) | internal | 0.15 | — | +0.15 |
| QMQ2-QMU2 (id 4) | **external** | 0.06 | −0.002 (its `asExtCommission`) | +0.058 |
| | | | **sum** | 0.208 |
| | | | path commission | −0.010 |
| | | | **path price** | **0.198** |

The size of that path is `min(quoteSize, tightest lean)` = `min(7, 9, 4) = 4`, and the lean that
capped it (id 4) is remembered as the **size determiner** — see §7.3.


---

## 6. Fair offsets and switch paths

A path's basket rarely matches its contract exactly: `QU+UV` reproduces `QMQ2 − QMV2` but it
*trades* `QMU2-QMV2` and `QMQ2-QMU2`. The residual exposure becomes a fair-value offset: leans
contribute **minus their multiplier**, the quoted contract contributes **+1**, everything expanded
into outright legs (`:69-225`).

### 6.1 The shipped config has no switch paths

For `QMQ2-QMV2` / `QU+UV`:

```
−1 × (QMU2 − QMV2)  −1 × (QMQ2 − QMU2)  +1 × (QMQ2 − QMV2)   =   0
```

Every path in `config/minimal_cme` nets to zero this way, so **no path is a switch path** and the
whole offset machinery is dormant. That is a property of the configuration, not of the engine: the
baskets were chosen to reproduce the quoted contract exactly.

### 6.2 When it does not net to zero

A hypothetical path on `QMQ2-QMV2` leaning only on `QMQ2-QMU2` (+1):

```
−1 × (QMQ2 − QMU2)  +1 × (QMQ2 − QMV2)   =   +QMU2 − QMV2   =   QMU2-QMV2
```

The residual is exactly another spread, so instead of carrying two legs the path leans on that
spread's own short-term fair ("spread substitution", `:180-204`). The path is then a **switch path**:
its fill is transferred to RV (§8.4), with the offset applied, because RV is the strategy that owns
fair-value positions.

### 6.3 What the offset does to the price

The offset is recomputed from incoming short-term fairs and applied as a delta to every path that
shares it (`:199-243`):

- offset value moves → each path's price moves by the difference, and the ranking is re-checked;
- offset becomes unavailable (RV disabled, or a fair value missing) → the path is **withdrawn** and
  quotes nothing;
- offset becomes available again → the path asks for a full recalculation.

A per-path **fair move** (`rv_stf_move`) is precomputed from the risk matrix: how much the path's
fair value moves per unit of exposure. It is used only for the RV transfer price (§8.4).

---

## 7. From path to quote

### 7.1 The two prices

```
internal price = path price ∓ edge        (bid: minus, ask: plus)
external price = round_to_tick(internal)  (bid: floor, ask: ceil)
```

For `QMQ2-QMV2` (edge `$0.0015`, tick `$0.025`):

| | bid | ask |
|---|---|---|
| path price | 59.99 | 60.26 |
| internal price | 59.9885 | 60.2615 |
| external price (rounded) | **59.975** | **60.275** |

The *lean* price (`path ± commission` — 60.00 / 60.25 here) is recorded too, for telemetry only: it
is what the basket "really" cost before the edge was taken.

### 7.2 The external size and the crossing clamp

The external size equals the internal size **except** when the external price would trade through
the opposite side of the same contract's external book. Then it is clamped to what that book shows
(`:1006-1013`):

```
external_size = min(opposite external adjusted size, internal size)   if the quote crosses
              = internal size                                          otherwise
```

A crossing quote can never advertise more than the liquidity it would immediately consume.

### 7.3 The size, and why it is asymmetric

A full recalculation sets `size = min(quoteSize, every lean's adjusted size)` and records which lean
capped it. Incrementally the rule is deliberately conservative (`:907-923`):

- **any decrease applies immediately** — a lean lost size, shrink now;
- **an increase applies only if** the path is still below its quote size **and** the lean that grew
  is the recorded size determiner (or there was none); then all leans are re-checked.

That asymmetry is why a path does not oscillate as lean sizes flicker, and it is why the incremental
update is not equivalent to a fresh recompute (§11.2).

### 7.4 Which path wins

Paths are ranked per contract and side (`:108-130`): valid paths first, then the better price (higher
for a bid, lower for an ask), then the larger size. Rank 0 is the quote, and its id travels with the
quote as the **hedge path**, because that is the basket a fill will be hedged with.

### 7.5 The validity gate

Before a quote is sent, its external price is checked against the opposite side's best price
(`:1115-1122`):

```
ticksThrough = bid: externalPrice − oppositeBest
               ask: oppositeBest − externalPrice
quote is valid  iff  ticksThrough < maxTicksThroughMarket        (strict!)
```

A violation is fatal by design: the strategy **disables itself and its external quoting**, raises a
critical alert, cancels everything, and logs the offending path. It means a path is pricing through
the market — usually a broken lean, a stale fair value, or a config error.


---

## 8. What happens on a fill

A fill is never left unhedged: it is matched leg-for-leg against the basket of the **best path of
that side**, i.e. the exact path the quote came from (`:552-708`).

### 8.1 First, release what AS reserved

An internal fill also releases AS's own aggressive-quote reservations on the same contract (internal
and external), and every fill releases the *opposite* side's external book, up to the fill quantity.
Those releases change adjusted sizes, so they happen **before** the hedge is chosen.

### 8.2 Then check the basket can still be hedged

Each lean must still be quoted at the price the path was priced from, must be tradeable, and must
display at least the fill quantity. A lean that has moved *worse* since the calculation vetoes the
hedge ("the path is stale"), which forces a recalculation and a second look.

### 8.3 The hedge orders

One order per lean, all carrying the same **hedge id**:

| | direction | price |
|---|---|---|
| lean with multiplier **> 0** | the **opposite** side of the fill | the lean's *near* book price ∓ hedge payup |
| lean with multiplier **< 0** | the **same** side as the fill | the lean's *near* book price ± hedge payup |

Both prices are sent: the internal one (with the payup) and the external one (the raw lean price).
Example — a 4-lot bid fill on `QMQ2-QMV2` filled at `59.975`, hedged on the ask at
`60.00 − 0.00001 = 59.99999`: sell at the bid, crossing to get out.

Sizes are the **whole fill quantity per leg**, not scaled by the multiplier — a basket of two leans
trades the fill quantity of each.

### 8.4 Switch paths: hand the position to RV

If the best path has a fair offset, the fill is not held: AS sends the quoted contract and every lean
to RelativeValue as internal fills at their short-term fairs, so RV books the position and manages it
against the factor model. The transfer price of the quoted contract is `fair + fairMove × qty`.

### 8.5 Reserve the liquidity the hedge just took

Only **after** the hedge orders are out (so they are not delayed), AS reserves the fill quantity on
each lean's book until the hedge completes. Those reservations are what makes other paths see less
liquidity, and they are released by the hedge acknowledgement and completion.

### 8.6 If nothing can be hedged

Then AS does not pretend: it raises a critical alert and forwards the fill to the MarketMaker, which
takes the position over (`FillSource::ASUnableToHedge`). A fill that arrives while AS is disabled is
forwarded the same way.

---

## 9. The event flow

```
market data ──► best book per (contract, side) ──► delta to every path leaning on it
                                                      │
fair value (from RV) ──► offsets recomputed ──────────┤
                                                      ▼
                              cycle: the touched contracts only
                                      │
                                      ├─ re-price the paths whose flags asked for it
                                      ├─ rank the paths
                                      ├─ build the quote from the best path
                                      └─ emit if changed (price or size), else nothing
```

Two properties are worth internalising:

- **Only touched contracts are revisited.** A market-data update marks the contracts whose paths
  lean on that book (plus their own), and the cycle walks exactly those.
- **A delta, not a recompute.** A price move on a lean is applied as `multiplier × priceDelta` to the
  paths that lean on it; the full basket sum is only recomputed when something forced it (path
  became valid, fair value changed availability, parameters reloaded).

The events that reach the strategy are: external top-of-book, internal (IME) top-of-book, fills,
short-term fairs, hedge acknowledgement/completion, and operator actions (enable/disable, external
quoting, venue gates, transaction open/closed).


---

## 10. A worked minute, tick by tick

`QMQ2-QMV2`, path `QV` (one lean, `$0.01` commission), edge `$0.0015`, quote size 7, tick `$0.025`.
The contract's own books are shown as `internal / external`.

| Event | Best bid | Best ask | Bid path | Bid quote | Ask path | Ask quote | Sent? |
|---|---|---|---|---|---|---|---|
| start | 60.00 / 59.75 | 60.25 / 60.50 | 59.99 | 59.9885 → **59.975** | 60.26 | 60.2615 → **60.275** | both |
| external ask falls to 60.15, becomes best | unchanged | 60.25 / **60.15** | 59.99 | **59.975** | 60.161 | 60.1625 → **60.175** | ask only |
| external bid rises to 59.90, still worse than internal | 60.00 / 59.90 | 60.25 / 60.15 | 59.99 | **59.975** | 60.161 | **60.175** | nothing |
| internal bid loses its size (0) | **0 / 59.90** | 60.25 / 60.15 | 59.889 | 59.8875 → **59.875** | 60.161 | **60.175** | bid only |
| a 2-lot bid fill | | | | | | | hedge: sell 2 at the bid |

Notes on the rows:

- the third row is **change detection** doing its job: the best bid is still the internal book, so
  neither price nor size changed, and nothing is re-sent even though a book moved;
- the fourth row shows the best book switching to external: the bid lean is now `59.90` net of its
  `$0.001` external commission, so the path costs `59.90 − 0.001 − 0.01 = 59.889` and the quote drops
  to `59.875`;
- the last row is §8: a 2-lot bid fill is hedged by selling 2 of the lean at its bid minus the payup,
  and reserving 2 on that book until the order completes.

---

## 11. Things that surprise people

### 11.1 "The quote is not the mid"

AS quotes a *basket* price minus an edge; it does not track the spread's own mid. If the spread
trades away from the sum of its legs, AS sees an opportunity, not a reference price.

### 11.2 A fresh recompute and the live price can differ

Two legacy behaviours make "recompute everything" not equal to "what the path is doing now":

1. the **size asymmetry** of §7.3 (a recompute is more optimistic), and
2. two different commission signs: the full calculation applies the external commission with the
   *path's* side, while the incremental update applies it with the *lean's* side. For a lean with a
   negative multiplier those disagree, so each external↔internal transition drifts the accumulated
   price by `2 × commission × |multiplier|` relative to a recompute.

Both are legacy behaviour and are reproduced deliberately (`PARITY_NOTES.md` D-AS7); the second is
bounded and asserted in the gate rather than papered over.

### 11.3 AS holds no position voluntarily

Every fill is hedged immediately, or handed to RV, or pushed to the MarketMaker with an alert. If
you see an AS position that is not a switch-path transfer, something failed.

### 11.4 The fair-value offset can withdraw a whole contract

If RV stops publishing a fair value that a switch path depends on, those paths are withdrawn and the
contract keeps quoting only through its non-switch paths — which, in `config/minimal_cme`, means all
of them (they are all non-switch).

### 11.5 The quote size is not the order size you will trade

A path can display 7 and be filled for 20, because a fill is hedged for its full quantity regardless
of the displayed size (the shortfall is handled by the FAK reservation and, if needed, by the
MarketMaker fallback).

### 11.6 Enabling is not instant

A (re-)enable marks every contract dirty and forces both sides to re-quote; a disable withdraws every
quote and cancels everything. Between those, a disabled strategy still maintains its paths and quote
fields, but emits nothing.

---

## 12. Where to look in the code

| Concept | Legacy | New engine |
|---|---|---|
| path construction, fair offsets | `ArbieSpreader.cpp:30-232` | `as/config_model.cpp`, `as/model.cpp` |
| best book | `ArbieSpreader.h:313-341` | `as/state.cpp` (`refresh_best`) |
| path price (full) | `:711-831` | `as/paths.cpp` (`calculate`) |
| path price (incremental) | `:833-928` | `as/paths.cpp` (`on_best_market_update`) |
| ranking | `:108-130` | `as/paths.hpp` (`better`, `sort_side`) |
| quote construction + gate | `:1000-1122` | `as/quotes.hpp/.cpp` |
| fill → hedge | `:552-708` | `as/hedger.cpp` |
| fair-value propagation | `_MessageHandler.cpp:153-243` | `as/paths.cpp` (`update_offset`) + `as/host.cpp` (`on_stf`) |
| displayed-size accounting | `Common.h:611-696` | `as/ledger_bridge.hpp` |
| cycle, disable, enable | `ArbieSpreader.cpp:318-435` | `as/host.cpp` |

---

## 13. One-paragraph summary

AS finds, for each contract it is allowed to quote, the cheapest way to *manufacture* that contract
out of the books it can actually see — a signed basket of leans, each priced off its better book net
of commission, netted against the strategy's fair-value view and the basket's own cost. It then
quotes that construction price minus a margin, sized to the tightest component, rounded to the tick,
and refused if it would sit through the market. When it is filled, it immediately executes the same
basket in reverse out of those same books (or hands the position to RV, when the basket does not
reproduce the contract), and pushes anything it cannot hedge to the market maker with an alert. It
makes money when the construction is cheaper than the fill, and it holds no position it did not
choose to hand over.

