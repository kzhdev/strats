# How the RV strategy works

A complete, code-referenced walkthrough of the RelativeValue engine in `new_impl/rv/`: what it
computes, in what order, with which formulas, and which behaviours are load-bearing enough that you
must not "clean them up".

Companion documents: `PLAN.md` (why it is built this way, phase gates) and `PARITY_NOTES.md` (every
known difference from the legacy `src/RelativeValue.h`, with evidence). The legacy file is the
specification; this document describes the implementation of it.

---

## 0. The idea in one page

RV does not predict anything. It prices one instrument off other instruments and hedges the
difference.

* A set of **pricing contracts** (outright futures) define a small linear model. Their market prices
  are explained by a handful of **vectors** (factors). The model is a `P` matrix: `price ≈ Pᵀ ·
  factor`.
* Inverting that model tells you, for each factor, how much of each contract it is made of, and
  therefore how to build a **hedge ratio** between any two tradable instruments.
* RV then **quotes** a spread (or any traded contract) at a fair value plus a width, where the fair
  value is the *short-term fair* `stf`, and where the price is derived by *leaning* (in legacy
  language) on another contract's book: "quote the spread at what it costs to replicate it out of
  the hedge instrument, plus costs".
* When one of those quotes **fills**, RV does not hold the risk: it immediately sends a **hedge
  order** into the lean instrument, sized by the residual position and capped by displayed liquidity.

Everything else in the engine is bookkeeping around those two actions: computing the market, keeping
positions, avoiding self-trades, and refusing to quote when the price has run away.

```
                 market data (2 books per contract, per side)
                                   |
                        midpoints  v
   pricing contracts  ------>  ODM  ------> pricing prices
                                   |              |
                                   |              v
                                   |            P matrix ------> factor prices
                                   |                                  |
                                   |                            EMA / set-to-market
                                   |                                  v
                                   |                             ltf (long-term fair)
                                   |                                  |
                                   |                       - position x fare_move
                                   |                                  v
                                   |                        stf_vec (per vector)
                                   |                                  |
                                   +--------- risk matrix ------------+
                                                                      v
                                            stf per pricing contract (int64 ticks)
                                                                      |
                                              spreads: stf = sum(leg stf x ratio)
                                                                      v
                                    quotes (per traded contract, per side)
                                                                      |
                                             fills ---> hedge requests ---> hedge orders
```

---

## 1. Vocabulary

| Term | Meaning in this code |
|---|---|
| **contract** | A tradable instrument: an outright (a pricing column) or a spread (a container of legs). `model::n_all` of them. |
| **pricing contract / outright** | One of the first `n_pricing` contracts. Its price is an input to the model, not a quote. |
| **spread** | A traded contract with more than one leg (`model::is_spread`). Its fair value is derived from its legs. |
| **traded** | A flag (`model::is_traded`), not a range. A contract can be priced *and* quoted (see §3). |
| **vector / factor** | One dimension of the linear model. `n_vectors == n_pricing` (square, D3). |
| **lean** | "Quote contract *c* off contract *j*'s book." Row `c` of the LeanMatrix. |
| **club** | The set of leans whose rounded price equals the best rounded price; their sizes are pooled for the external quote. |
| **stf** | Short-term fair value, in price units (int64). |
| **ltf** | Long-term fair value per vector (double), an EMA of the factor prices. |
| **ODM** | Over-determined operator: weighted least squares from all contract mids to the pricing-contract prices. |
| **risk matrix** | `P⁻¹`, i.e. the exposures that turn fair values into a vector position. |
| **reservation** | A temporary claim on displayed liquidity, so two quotes do not both assume they can consume it. |

Notation: `c` = quoted/traded contract, `j` = lean contract, `v` = vector, `o` = pricing contract.
Prices are `int64` at 1e9 (`PRICE_SCALE`); `to_dollars()` / `to_rhone_price()` convert at the boundary
(see PLAN §4.3b for why the middle band is `double`).

---

## 2. The two market books

Every contract has **two books per side** (`cycle_state::markets`, indexed by
`market_index(contract, side, external)`):

* **internal** — what Viper itself is quoting/resting (published by the internal matching engine).
* **external** — what the venue shows.

Both are needed because the quote you send may rest on the internal book while the hedge you need
lives on the external one. Two derived rules shape everything downstream:

* `cycle_state::best_market(c, sd)` — higher bid / lower ask wins, `NULL_PRICE` always loses
  (legacy `BetterMarket<side>`). Ties go to the **internal** book, because the comparison is
  `internal >= external` for bids and `<=` for asks.
* `cycle_state::best_is_external(c, sd)` — which of the two won. This decides whether a lean is priced
  off the external book (`_externalPrice` path) and which liquidity source a reservation belongs to.

Midpoints are always computed from the **external** books, even if the internal book is better. That
is legacy behaviour and it matters: the model describes the market, not our own quotes.

---

## 3. Contract layout (the one ordering rule)

The model is a flat array of contracts. Indices are assigned at init by `build_model_inputs`:

1. every `PricingMatrix` column, in header order → indices `[0, n_pricing)`;
2. every `RVConfigs` row not already present, in file order → indices `[n_pricing, n_all)`.

Two consequences that are easy to get wrong:

* A PricingMatrix column **must** keep its index: the ODM operator, the `P` matrix and the risk
  matrix are all keyed by it.
* A contract listed in **both** matrices (`config/2msgw` lists GEH1 and ZTH1 in both) is **one**
  contract that is priced *and* quoted, so it stays at its pricing index, below `first_traded`.
  Therefore `first_traded` only marks "the first index that cannot be a pricing column"; **anything
  that means 'is this quoted?' must test `model::is_traded(c)`**. Using `c >= first_traded` for that
  is the bug this layout exists to prevent (it silently skipped all quoting for 2msgw).

Validated at init (`model::build`): square model, compile-time caps, invertible `P`, no spread in a
pricing column, every leg resolves to a pricing contract, and every quoted contract is either a
pricing column or a spread with legs.

---

## 4. Inputs

### 4.1 The model CSV (`rv_model_cfg::load`)

Section-based CSV. Only the RV sections are read; the others (MMConfigs, StackerConfigs, ...) are
skipped as blocks so their contract-named rows are not mistaken for data.

| Section | Column(s) used | Becomes |
|---|---|---|
| `PricingMatrix` | header = pricing contracts; rows = vectors | `P[v][o]` loadings |
| `FactorConfigs` | `range`, `maxPos`, `ltf`, `EMA`, `EMALength(secs)`, `SetToMarket`, `Hedged` | per-vector fair-value behaviour |
| `RVConfigs` | `quoteExternal`, `commission`, `RVWidth`, `%stfMove`, `maxTicksThroughMarket`, `staticQuote`, `quoteSize` | per traded contract |
| `PricingConfigs` | `weights`, `weightedMid`, `establishedLevel`, `pricingMultiplier`, `quantityMultiplier` | per contract |
| `LeanMatrix` | rows = quoted contract, columns = lean contract, `1` = can lean | the lean CSR |
| `GlobalConfigs` | `RVWidthMultiplier` (per session column) | width scaling |

Column order differs between configs, so every column is resolved by header name, never by position.

### 4.2 The secdb (`load_secdb`)

Header-driven, quote-aware CSV. Per symbol it yields:

* `tick` → tick size in dollars (the tick grid everything rounds to),
* `order_increment_qty` → lot size used by `to_valid_qty`,
* `activate` → venue (0 CME, 1 ICE, 2 EUREX, 3 JPX, 4 other), used by the per-venue quoting gate,
* `details` → **the authoritative spread legs**: `{"legs":[{"sym":"BRN FMQ0022!","qty":1}, ...]}`.

Legs come from `details` when present; the contract-name splitter (`A-B`, EUREX `A:B` with product-root
prefixing) is only a fallback, and if both are available they must agree or init fails. A name cannot
resolve ICE's `!` suffix or a ratio other than ±1 — `details` can.

---

## 5. Init: from config to model

Everything in this section happens **once** (`model::build`), not per cycle. `model` is immutable
afterwards, which is why the hot path can index it without locking or copying.

### 5.1 The three operators

With `A` = contract-to-pricing-column incidence (`A[c][o]` = 1 for an outright, the leg ratio for a
spread), `W` = diag(weights):

```
ODM  = (Aᵀ Wᵀ W A)⁻¹ Aᵀ Wᵀ W            [n_pricing x n_all]
risk = P⁻¹                              [n_pricing x n_vectors], stored transposed as
                                        risk[contract][vector] for one row-major matvec
```

Both use `Eigen::FullPivLU`. A singular `P` is an init failure (the legacy ALGLIB path returned
garbage of magnitude ~1e15 instead). The weight enters **squared** — `WᵀW` — which is one of the
correlated-oracle bugs found and fixed (PARITY_NOTES O1).

### 5.2 Per (contract, lean) tables

For every **ordered pair of traded contracts** `(c, j)`, `c != j`:

* **vector exposures**: `exp1[v] = exposure(c, v)`, `exp2[v] = exposure(j, v)`, where
  `exposure(x, v) = Σ_legs risk[v][leg] · leg_ratio · qmult[x]` (and `risk[v][x] · qmult[x]` for an
  outright).
* **candidate hedge ratios**: `possible[v] = exp1[v] / exp2[v]` — the size of `j` that neutralises
  factor `v` of one unit of `c`.
* **pick the best one**: for each candidate `v`, build the residual matrix
  `vector_risk[r][vc] = exp1[r] - exp2[r] · possible[vc]` and score each column with
  `sums[vc] = Σ_r fare_move[r] · |vector_risk[r][vc]|`, i.e. "how much unhedged factor risk is left if
  I hedge into column `vc`, weighted by how far that factor typically moves". The candidate with the
  smallest score wins; ties are broken by the *largest* `fare_move` (the most volatile factor),
  and ties within 1e-6 count as ties.
* **hedge ratio** `hedge_ratio[c][j] = Round<4>(possible[winner])`.
* **factor-hedged index** `factor_hedged_index[c][j] = winner` **only if that vector is flagged
  `Hedged`** in FactorConfigs, else `-1`. This is what later lets the engine compute a *residual*
  position in factor terms at all.
* **commission**: normalise the two commissions by the smallest non-zero pricing exposure of the pair:

  ```
  normalizer = min_{o} | ratio_toward(c,o) - ratio_toward(j,o) * hedge |   (skip zeros)
  commission = (comm[c] * pm[c] + |hedge| * comm[j] * pm[j]) / normalizer
  ```

  It is stored pre-divided by `pm[c]` (`commission_scaled`) so the cycle does not divide.
* **quote size**: for one lot of the hedge, the move it implies per pricing contract
  `fair_move[o] = Σ_v one_lot[v] · risk[v][o] / pm[o]` with `one_lot[v] = (exp1[v] - exp2[v]·hedge) · fare_move[v]`;
  then the smallest size over all traded contracts that keeps the implied move below the configured
  `%stfMove` of the width:

  ```
  quote_size[c][j] = min over traded s of  pct_stf_move[s] / (|move_s| / width_s)
  ```

### 5.3 Per-contract tables

| Field | Source | Note |
|---|---|---|
| `tick` (as a magic divider) | secdb `tick` | `fast_div`, see §12.6 |
| `lot` | secdb `order_increment_qty` | sizes are floored to this |
| `venue` | secdb `activate` | per-venue external-quoting gate |
| `width` / `width_extra` | `RVWidth` × `RVWidthMultiplier` | `width_extra = width + width·(mult−1)`; the cycle uses `width_extra` |
| `threshold` | `0.05 × tick` | change detection is `|old − new| > threshold`, so effectively any move |
| `est_level` | `establishedLevel` | clip for the weighted midpoint |
| `max_ticks_through_market` | `maxTicksThroughMarket` × tick | the quote sanity gate (§7.5) |
| `weight` | `PricingConfigs.weights` | ODM weight; 0 ⇒ the contract's mid is not computed |
| `weighted_mid` | `weightedMid` | simple vs size-weighted midpoint |
| `static_quote`, `static_quote_size` | `staticQuote`, `quoteSize` | external size override |
| `rhone_max_clip` | runtime (`SetRhoneLimits`) | **0 until the host supplies it**, and 0 disables RV on the first fill |

---

## 6. The cycle

`strategy::run_cycle(now_cycles)` runs on every timer tick (a "cycle", typically 10/s per configured
frequency) and is the only place that orders the work:

```
if (!enabled) return;                       // disabled strategies do nothing at all
valuation_.refresh_dirty_mids(state_);      // 1. midpoints, only contracts that moved
valuation_.recompute(state_, now);          // 2. stages 2-7 below (always full)
quotes_.build_all(state_);                  // 3. quotes, both sides, every traded contract
if (hedger_.has_request()) {                // 4. at most one hedge contract per cycle
    must_disable = hedger_.run(state_);
    if (must_disable) disable();
    valuation_.recompute(state_, now);      //    positions moved: re-price...
    quotes_.build_all(state_);              //    ...and re-quote (legacy :572)
}
state_.clear_dirty();
```

The pipeline is recomputed **in full** even if only one contract moved: one midpoint reaches every
contract through the ODM, so incremental updating is not possible. The dirty set exists only to skip
the midpoint recomputation.

### 6.1 Midpoint (stage 1)

From the **external** books (`valuation::mid`):

```
simple:    mid = (bid + ask) / 2                                  (in dollars)
weighted:  mid = (ask_px·bid_sz + bid_px·ask_sz) / (bid_sz + ask_sz)
           with sizes clipped to establishedLevel, and 0 if both clip to 0
```

Note the cross-weighting in the weighted variant (each price weighted by the *opposite* size) — that is
legacy's formula, reproduced. A missing side propagates `NULL_PRICE` and produces a garbage midpoint
(~9.2e10); that is legacy behaviour too, and it is why the differential test only compares cycles with
two-sided books (PARITY_NOTES O2).

Mids are computed only for contracts with a non-zero weight (`used_in_pricing`), **including spreads**
(they are ODM inputs).

### 6.2 Pricing prices (stage 2)

```
pricing[o] = Σ_c ODM[o][c] · mid[c]
```

One `n_pricing × n_all` matvec. With consistent books this round-trips exactly (the round-trip is a
property test: ODM → P → riskᵀ must reproduce the fed mids).

### 6.3 Factor prices (stage 3)

```
factor[v] = Σ_o P[v][o] · (pricing[o] · mult[o])
```

The pricing multiplier scales each contract's contribution; the quantity multiplier does *not*
appear here.

### 6.4 Long-term fair value (stage 4, the EMA)

Per vector, if `EMA` is on:

```
elapsed = now − last_ema_time
alpha   = elapsed / ema_period_cycles          (0 → ltf = factor directly)
mu      = exp(−alpha)
nu      = (1 − mu) / alpha
ltf    += mu·ltf + (nu − mu)·prior_factor + (1 − nu)·factor      (only if finite)
prior_factor = factor                                          (always)
```

This is an exponentially weighted average written in the "slope-aware" form so it is correct for
arbitrary (non-uniform) elapsed time. `EMALength(secs)` is converted to cycles by the host's
`cycles_per_second`; a 0-length EMA means "no smoothing, use the factor".

### 6.5 Per-vector short-term fair (stage 5)

```
stf_vec[v] = SetToMarket ? factor[v] : ltf[v] − pos_vec[v] · fare_move[v]
```

This is where **positions** enter pricing: holding `pos_vec[v]` units of factor `v` pushes the fair
value *against* you by `range/maxPos` per unit — the model's assumption that a large position requires
a better price to be worth holding. `SetToMarket` bypasses it (the fair value simply tracks the
market).

### 6.6 Pricing-contract fair value (stage 6)

```
stf[c] = trunc( Σ_v risk[c][v] · stf_vec[v] / mult[c] )        c ∈ [0, n_pricing)
```

Integer again: `to_rhone_price_saturated` truncates (legacy truncates, it does not round) and clamps
to ±9.2e18, because a config-scale `ltf` can overflow int64 at the 1e9 price scale.

### 6.7 Spread fair value (stage 7)

```
stf[c] = Σ_legs trunc( stf[leg] · ratio )        for spreads, in leg order
```

Each leg is truncated **before** the sum, inside a 128-bit accumulator (two saturated legs can exceed
int64, and a leg ratio > 1 can overflow on its own). This per-leg truncation is legacy behaviour and
changes results versus summing in floating point — do not "improve" it.

---

## 7. Quoting

For every traded contract and both sides (`quote_engine::build`).

### 7.1 Walk the lean row

`model::lean_begin/lean_index` is a CSR row built at init from LeanMatrix rows, keeping only pairs that
are both *leanable* (LeanMatrix = 1) and *quotable* (hedge ratio ≥ 0; a negative ratio is folded out).
For each lean `j`:

```
external = best_is_external(j, sd)          // which book of the lean we are pricing off
price    = lean_price(c, sd, j, external)   // NULL_PRICE ⇒ skip this lean
```

### 7.2 The lean price (the heart of the strategy)

```
cost      = sd == BID ? stf[j] − lean_market_price : lean_market_price − stf[j]
hedge     = cost · ratio · (pm[j]/pm[c]) · (qm[j]/qm[c])        ← legacy order, 4 divisions
            (fast path: cost · ratio when all four multipliers are exactly 1.0)
hedge     = clamp(hedge, ≥ −0.95·width_extra[c])               ← negative-cost clamp
price     = sd == BID ? stf[c] − width_extra[c] − hedge − commission_scaled[c][j]
                       : stf[c] + width_extra[c] + hedge + commission_scaled[c][j]
```

Read it as: *"what would this contract be worth if the market is right about `j`, minus the edge I
want, minus the cost of hedging, minus commissions"*. A bid is `fair` minus width; an ask is `fair`
plus width.

The **fast path** exists only where it is provably bit-identical (all four multipliers exactly 1.0).
The four-division form is kept otherwise because folding it into one precomputed factor changes the
last bit before truncation and produced a 0.5–0.7 % tick-mismatch rate (PARITY_NOTES F1).

### 7.3 Best lean, and the size that comes with it

`bid` wins on the highest price, `ask` on the lowest. Each candidate must pass the validation gate
(§7.5) — a rejected lean is skipped, and a *validation failure* disables the strategy (§10).

The quote's size is **not** the displayed size — it is the lean size (legacy `calculateSize`):

```
market_size = policy.adjusted_size(lean market)      // displayed minus reservations
size        = ratio > 0 ? min(quote_size[c][j], market_size / ratio) : quote_size[c][j]
q.size      = to_valid_qty(size + 0.0001, lot)       // floor to lot, +0.0001 kills FP dust
```

`q.price` is kept **unrounded** (legacy `_bestPrice`), because the hedge decision compares it against
market prices; only the external price is rounded.

### 7.4 External quote and the club

If the contract is flagged `quoteExternal`, the engine also produces the price it is willing to show
on the venue. Lean prices that land on the **same tick** are pooled into a *club*: their sizes are
summed and the average is offered alongside the internal size:

```
external_price = round_to_tick(price, tick[c], sd)          (of the club)
external_size  = staticQuote ? static_quote_size
                             : to_valid_qty(max(q.size, club_size / club_count + 0.0001), lot)
```

> **Known defect — see §13.1.** Legacy sorts the lean row by price *before* this loop
> (`_leans.sortBids()/_leans.sortAks()`), so its club is anchored on the **best** lean price and the
> `break` on a different price is meaningful. The engine walks the row in contract order and anchors
> on the **first** lean with a valid price. The differential test cannot see the difference because
> the oracle makes the same assumption.

### 7.5 The maxTicksThroughMarket gate

Before a lean can become a quote, `quote_engine::within_max_ticks_through_market` checks it:

```
other_side = best_market(c, other(sd)).price          // our own book, opposite side
if other_side == NULL_PRICE:  accept                 // nothing to measure against
quote_uncapped = lean price WITHOUT the negative-cost clamp
through = sd == BID ? quote_uncapped − other_side : other_side − quote_uncapped
reject if through >= max_ticks_through_market[c]      // >=, so 0 rejects everything
```

It is a **runaway detector**, not a filter: a quote that sits at or through the opposite side of the
traded contract's own book, by more than N ticks, means the model and the market have diverged (stale
data, a broken leg, a bad config). Legacy responds by disablingRV and raising a critical alert, and so
does this engine. Note the deliberate use of the *uncapped* price: the clamp in §7.2 would otherwise
hide exactly the runaway this looks for.

### 7.6 Change detection

```
has_change = |old_price − new_price| > threshold[c]  ||  old_size != new_size
           || (external) |old_ext − new_ext| > threshold[c] || old_ext_size != new_ext_size
q.has_change |= has_change                          // sticky until the host emits it
```

`threshold` is 5 % of a tick, so in practice any price change counts. The sticky flag is what makes the
whole quote lifecycle work: the host may be unable to send right now (`canSendQuotes == false` while an
internal fill is settling), and the pending change must survive to the next cycle.

---

## 8. Hedging

Quoting is the income; hedging is the risk control. RV never keeps a filled position: it converts every
fill into a hedge request against the lean instrument, and works those requests one contract per cycle.

### 8.1 Fill → request

```
on_fill(c, sd, qty):  request[c] += (sd == BID ? +qty : −qty)
```

Requests are a small fixed array with a contract → slot index (FIFO order, legacy's `unordered_map` order
was unspecified — D-V4). Repeated fills on the same contract accumulate into the same request.

### 8.2 Residual: how much is already hedged

The request is not the order size. The engine first asks "how much of this position is still
unhedged?" (`hedger::residual`, legacy `getResiduals`):

```
q = current quote for (c, side);        no quote ⇒ residual 0
v = factor_hedged_index[c][q.lean];     −1 ⇒ residual 0   (this pair is not factor-hedged)
vector_risk = Σ_legs risk[leg][v]·ratio           (or risk[c][v] for an outright), then Round<2>
residual    = pos_vec[v] / vector_risk / qmult[c]          (0 if vector_risk rounds to 0)
```

In words: the engine's total position in factor `v` (`pos_vec[v]`) expressed in units of this contract.
If it is already flat, the residual is 0 and the fill must be hedged in full; if a previous hedge
overshot, the residual has the opposite sign and reduces the new hedge.

### 8.3 Request → orders (`hedger::run`)

One request per cycle (S10):

```
side   = request_size > 0 ? BID : ASK
res    = residual(c, side)
size   = request_size + res                       // what actually needs hedging
if |size| > rhone_max_clip[c]:  disable (S12), drop request        ← safety: size sanity
else: left = send_hedges(c, |size|, hedge_side, remove_request, |res|)

if remove_request:  add_position(c, request_size); drop request
else:               hedged = |request_size| − remaining; add_position(c, ±hedged); keep remainder
```

### 8.4 The club walk

`send_hedges` mirrors the quote stage: it walks the same lean row, prices each lean the same way, and
only hedges into the **club** — leans whose rounded price equals the first one's:

```
for each lean j in row:
    price = lean_price(c, sd, j, external)                NULL ⇒ skip
    if round_to_tick(price, tick[j], sd) != club price:  leave the club → stop
    qty = order_size(j, ratio, adjusted_market_size(j), need, lean_size(j), |res|, quote_size[c][j])
    if qty > 0 and no guard fires:
        send { contract: j, side: other(sd), price: best_market(j, sd).price,
               size: qty, external, hedge_id: ++counter, market_index: (j, sd, external) }
        add_position(j, ±qty)
```

`order_size` (legacy `getOrderSize`) does the tiling:

```
max_hedge      = (lean_size + |residual|) · ratio        // what the displayed liquidity supports
available      = min(adjusted_market_size(j), max_hedge)
need_in_hedge  = need · ratio
qty            = to_valid_qty(round(min(need_in_hedge, available)), lot[j])
need           = (need_in_hedge − qty) / ratio           // remainder for the next lean/cycle
```

So one cycle can emit several orders across the club, but each is capped by *displayed* liquidity minus
reservations, and the leftover stays as the request for the next cycle.

### 8.5 Guards (why a hedge is *not* sent)

| Guard | Condition | Effect |
|---|---|---|
| self-cross (S14) | the hedge price is at or through our own quote on the lean | skip this lean; count a skip |
| no market | the lean's market price is `NULL_PRICE` | skip |
| ticks worse | the hedge is ≥ 2 ticks worse than our own quote on the lean, in hedge terms | skip this lean; count a skip |
| 50-skip fail-safe | 50 consecutive skips | disable the strategy |
| clip (S12) | request larger than `rhone_max_clip` | disable the strategy |

"Ticks worse" is measured in the **contract's** terms, not the lean's: the price delta is scaled by the
hedge ratio (and the multiplier ratios), which is why the code recomputes that expression rather than
comparing raw prices.

### 8.6 Position bookkeeping

`add_position` is the only writer of positions (`state.position`, `state.pos_vec`):

```
position[c] += size
if spread:      pos_vec[v] += risk[leg][v] · (size·ratio) · qmult[c]   for every leg, every v
else (priced):  pos_vec[v] += risk[c][v] · size · qmult[c]
```

This is what closes the loop: `pos_vec` feeds stage 5 of the *next* valuation (§6.5), which shifts the
fair value against the position, which widens the quotes on the side you already hold. The strategy
gets less aggressive exactly where it is getting long/short — that is the position-aware part of the
model, not an afterthought.

---

## 9. Liquidity reservations

Two quotes can both believe they own the same displayed size (our spread bid and its hedge bid live on
different markets but draw on the same liquidity pool). The ledger prevents that double-counting.

* Each book is wired once (`host::wire_market`) to a `market_key` and the `liquidity_source` currently
  supplying it.
* Quote sizing asks the ledger first: `quote_policy::adjusted_size` → `displayed − reservations` (source
  scoped), so `lean_size` (§7.3) already accounts for what is spoken for.
* When a hedge is emitted, the engine reserves the size it is about to consume **on the market that is
  displaying the liquidity** — which is `order.market_index`, the lean book on the quote side, *not*
  the market the order is sent to (PARITY_NOTES O4).
* The reservation is released when the venue acks (`on_hedge_ack`, possibly for less than requested) and
  completes (`on_hedge_done`).
* **A replaced price level flushes its reservations** (`quote_policy::price_changed` → `invalidate`),
  because a FAK adjustment against a price that no longer exists is meaningless. Internal books never
  flush (updating the internal source is not a price-level replacement).

If no ledger is supplied, the strategy runs on raw displayed sizes — that is the parity mode used by the
tests.

---

## 10. The host: events, emission, disabling

`host` is the composition root. It owns the ledger bridge, the ports, the venue gates and the statistics;
the strategy only computes.

| Input | What the host does |
|---|---|
| `on_market_data(c, sd, external, px, sz)` | bounds check → strategy (which flushes reservations on an external price change and marks the contract dirty) |
| `on_fill(c, sd, qty)` | pass through to the hedger's request book |
| `on_hedge_ack(id, ord_id, actual)` | ledger ack with the *actual* size |
| `on_hedge_done(id)` | ledger complete |
| `on_timer(now)` | run the cycle, then: check the gate, check self-disable, then emit quotes |

Quote emission is a **pull** step, not a push from the cycle: the engine records the desired state
(`quote` per contract and side) and the host decides what may actually be sent:

```
if !can_send_quotes:            keep the change pending, try next cycle (legacy behaviour)
for c in traded contracts, both sides:
    if q.has_change: clear it; send quote_request{c, side, price, size, external_price, external_size}
    notify once per contract that had a change
```

`quote_external` is gated three ways: the contract flag, the strategy-wide gate, and the per-venue gate
(`set_venue_enabled`) — because one venue going down must not silence the others.

**Disable paths** (all end with an alert, external quoting off, and removals emitted for every live quote):

1. `validation_failures` increased during the cycle → `RV MaxTicksThroughMarketCheckFailed`;
2. the engine disabled itself (hedge clip S12, 50-skip S14) → surfaced with its own message;
3. `enable(false)` from the operator.

After a disable the cycle is a no-op until re-enabled: no quotes, no hedges, no position changes.

---

## 11. One cycle, with numbers

A synthetic two-leg spread, unit multipliers, tick 0.25 → `tick_size = 25 000 000`, price units 1e9.

Setup:

```
contract c  = the quoted spread      stf[c]      = 100.000000000   (100 000 000 000)
                                     width_extra = 0.008           (     8 000 000)
lean contract j (a pricing outright) stf[j]      =  99.900000000
market bid of j                      =  99.875000000
hedge_ratio[c][j] = 1.0, all multipliers 1.0, commission_scaled[c][j] = 0.00034
```

**1. Lean price for the bid of `c`:**

```
cost   = stf[j] − market_bid[j] = 99.900 − 99.875 = 0.025          (one tick)
hedge  = cost · ratio           = 0.025                            (unity fast path)
price  = stf[c] − width − hedge − commission
       = 100.000 − 0.008 − 0.025 − 0.00034 = 99.96666
```

**2. Quote:** `q.price = 99.96666` (unrounded, int64), `q.lean = j`.

**3. External price:** `round_to_tick(99.96666, 0.25, BID)` → floor to the grid → **99.75**. So the book
shows a bid at 99.75 while the model's opinion is 99.9667; the 0.2167 difference is the edge you asked
for (width), plus the hedge cost, plus the tick rounding the exchange forces on you.

**4. A fill:** someone lifts 2 lots → `request[c] = +2`.

**5. Next cycle:**

```
residual(c, BID) = pos_vec[v] / vector_risk / qmult                (0 here: no position yet)
size   = 2 + 0 = 2                                                 |size| = 2 <= clip → hedge
```

**6. Hedge walk over the club:** `lean_size(j) = min(quote_size[c][j], adjusted_bid_size(j) / 1.0)`.
With 50 displayed and nothing reserved, say `quote_size` = 7 → `lean_size` = 7, so `available = min(50, (7 + 0)·1) = 7`
and `qty = to_valid_qty(round(min(2·1, 7)), lot) = 2`:

```
order = { contract: j, side: ASK, price: best_market(j, BID).price, size: 2,
          external: true, hedge_id: 1, market_index: (j, BID, external) }
```

Note the hedge is a **sell** of `j` on the *same* side of the book we leaned on — hedging a bought spread
means selling the lean. The reservation is taken against `(j, BID, external)`, the book that displayed
the liquidity.

**7. Position:** `add_position(j, −2)` → `position[j] = −2`, `pos_vec` updated through the risk row of
`j`. From the next cycle on, stage 5 (§6.5) nudges the fair value against that position, which widens the
`j`-side quotes — the feedback loop that keeps RV from stacking risk.

---

## 12. Invariants, quirks and traps

These are not style notes. Each one changes numbers or breaks trading if you "clean it up".

1. **Legacy is the spec.** Every formula above was transcribed from `src/RelativeValue.h` with the same
   operation order and the same truncation points. Re-associating arithmetic changes the last bit before
   an `int64` truncation and therefore changes ticks (PARITY_NOTES F1, F2).
2. **Truncation, not rounding.** `to_rhone_price` truncates toward zero; `round_to_tick` floors bids and
   ceils asks on the grid. Both are reproduced exactly, including for negative prices.
3. **`NULL_PRICE` propagates.** A missing side produces a garbage midpoint (~9.2e10) that flows into fair
   values. Legacy does this; the engine does too, and the differential test simply excludes such cycles.
4. **`maxTicksThroughMarket = 0` rejects every quote** and therefore disables RV. It is reproduced, not
   treated as "no limit".
5. **`rhone_max_clip = 0` (the default until `SetRhoneLimits` arrives) disables RV on the first fill.**
6. **Tick division uses a precomputed magic number** (`fast_div`), because `int64` division is ~1.8× slower
   than `double` and it used to run ~120 times per cycle. The fast path is exact, not approximate.
7. **The negative-cost clamp** caps a *negative* hedge cost at 95 % of the width. The uncapped value is
   used by the runaway gate, deliberately (§7.5).
8. **Per-leg truncation** when summing spread fair values (§6.7).
9. **Position sign conventions:** a bid fill is `+`, an ask fill is `−`; a hedge on `other(sd)`; the
   residual can have the opposite sign and shrink the hedge.
10. **One hedge request per cycle**, and a request can be partially filled across several cycles.
11. **`has_change` is sticky** until the host actually emits the quote.
12. **Prices in the emitted quote vs on the wire:** `quote_request.price` is the **unrounded** lean price;
    `external_price` is on the tick grid. Sending the unrounded one to the venue is the host's job to
    handle (legacy does the same).
13. **Two books, one truth:** midpoints always use the external book; hedge sizing uses whichever book is
    displaying, and reservations are scoped to that source.
14. **Recompute twice when a hedge is sent:** positions changed, so fair values and quotes are rebuilt in
    the same cycle (legacy `:572`). Skipping the second pass leaves quotes stale by one cycle.

### 12.1 Numeric boundary

Prices are `int64` at 1e9; the middle of the pipeline (mids, operators, EMA) is `double`. That is a
measured decision, not an oversight: on x86-64 a 64-bit fixed-point matvec needs `__int128` accumulation
and AVX2 has no 64-bit integer multiply, so double is 1.6–2.5× *faster* here, and the operator tables
contain non-terminating rationals anyway. Full reasoning and measurements: PLAN §4.3b.

---

## 13. Known defects and open items

### 13.1 The external club is anchored on row order, not on the best lean

*What legacy does* (`src/RelativeValue.h`, `Quote::createQuote`): sorts the lean container
(`_leans.sortBids()` / `sortAks()`) **before** building the club, then walks the sorted leans, seeding
`roundedPrice` from the first (i.e. best) lean and `break`ing at the first different rounded price. The
club is therefore "all leans whose rounded price equals the best rounded price".

*What the engine does* (`quote_engine::build_club`): walks `model::lean_index` in **contract order** and
seeds the club from the first lean with a valid price. When the best lean is not the first valid lean in
the row, `q.external_price` is the wrong tick — demonstrably:

```
ask: best price = 100.25 (lean 1)   external price = 100.50   ← first-in-row lean 0
```

*Why the gates are silent:* the differential oracle (`rv_tests.cpp`, `diff_quotes`) walks the same row in
the same order, so engine and oracle agree with each other and both disagree with legacy — the O1/O3/O5
pattern in PARITY_NOTES.

*Impact:* every contract with `quoteExternal = TRUE` and more than one usable lean, i.e. the external
quote price (and hence where the venue order rests), whenever the best lean is not first in the row.

*Fix:* anchor the club on the **best rounded price** (equivalently: collect every lean whose rounded
price equals the round of the winning price — no sort needed, O(n)), and on a validation failure set
`external_price` to that anchor with `external_size = 0` before returning, so the disable path looks like
legacy's. Then fix the oracle the same way (deriving it from the legacy text, not from the engine) and
re-run `rv_tests`, `rv_host_test` and the replay matrix.

### 13.2 Config gaps (not engine defects)

* `config/minimal_eurex` cannot build: its model trades Sep-21/Mar-22 `efFGBM*` contracts while its secdb
  only knows Dec-20…Jun-21 symbols (no tick, no `details`). Refresh the secdb.
* `config/backtest` runs and then saturates because its `ltf` column holds out-of-range values
  (PARITY_NOTES F6).

### 13.3 Not yet verified against the legacy binary

Parity is proved against a literal transcription of the legacy formulas, not against numbers produced by
compiling `src/RelativeValue.h` (PLAN §6.2 layer-2 traces). Two items are still open on that basis:
non-unit `pricingMultiplier` configs (PARITY_NOTES F2/F4) and the effect of `-ffp-contract=off` relative to
*the legacy binary* rather than to the oracle.

---

## 14. Code map

| File | Contains |
|---|---|
| `types.hpp` | `price_t`/`qty_t`, fixed point & saturation helpers, `fast_div` + `round_to_tick`, `side`, `market_index`, `quote`, `hedge_order`, `quote_request` |
| `limits.hpp` | compile-time caps (`MAX_CONTRACTS`, `MAX_PRICING`, `MAX_VECTORS`, `MAX_LEANS`) |
| `model.hpp/.cpp` | `model::build` (operators, per-pair tables, validations) + contract queries |
| `state.hpp/.cpp` | `cycle_state`: books, fairs, quotes, positions, dirty set, `best_market` |
| `valuation.hpp/.cpp` | stages 1–7 (mid → ODM → factor → EMA → stf_vec → stf → spreads) |
| `quotes.hpp/.cpp` | `lean_calculator` (lean price/size/cost) and `quote_engine` (best lean, club, gate, change detection) |
| `hedger.hpp/.cpp` | request book, residual, tiling/sizing, club walk, guards, position bookkeeping, `order_port` |
| `strategy.hpp/.cpp` | the façade: input surface, enabled flag, `run_cycle` |
| `host.hpp/.cpp` | composition root: events, quote lifecycle & emission, ledger wiring, disable paths, stats |
| `ledger_bridge.hpp` | binds `quote_policy`/`order_port` to `common::liquidity_ledger` |
| `config.hpp/.cpp` | `rv_model_cfg` (model CSV) + `load_secdb` (ticks, lots, venues, `details` legs) |
| `config_model.cpp` | `build_model_inputs`: config + secdb → `model_inputs` (hedge ratios, commissions, quote sizes) |
| `app/rv_replay.cpp` | runnable driver: real config → model → ledger → host → deterministic tape + invariant checks |
| `tests/` | `reference_oracle.hpp` (legacy transcription), `rv_tests.cpp` (differential + property tests), `rv_host_test.cpp` (wiring), `rv_cycle_bench.cpp` |

---

## 15. Running it

```bash
cmake -S new_impl -B new_impl/build -DCMAKE_BUILD_TYPE=Release \
      -DEIGEN3_INCLUDE_DIR=/path/to/eigen-3.4.0        # or a real Eigen3 package
cmake --build new_impl/build -j

cd new_impl/build && ctest                 # parity, host, bench smoke, replay smoke

cd ../..                                   # the drivers resolve config paths from the repo root
./new_impl/build/rv_tests 20000            # differential gate vs the legacy transcription
./new_impl/build/rv_host_test              # quote lifecycle / gate / reservation round trip
./new_impl/build/rv_cycle_bench 200000 5   # latency, allocations per cycle
./new_impl/build/rv_replay config/backtest 500 42     # end-to-end on a real config
```

`rv_replay` exits non-zero when a deployment invariant fails (quotes emitted, reservations drained,
adjusted size never exceeds displayed). Run it on every config directory before touching the engine:
currently 7 of 8 pass, see §13.2.

---

## 16. Glossary of legacy names

| This document / code | Legacy `src/RelativeValue.h` |
|---|---|
| `stf`, `stf_vec`, `ltf` | `_shortTermFairs`, `_shortTermFairsInVectors`, `_longTermFairs` |
| `vector` | factor |
| `lean` | `Lean` / `_leans` |
| `club` | the lean-price group in `Quote::createQuote` |
| `quote_engine::within_max_ticks_through_market` | `validateMaxTicksThroughMarket` |
| `hedger::residual` | `getResiduals` |
| `hedger::order_size` | `getOrderSize` |
| `hedger::send_hedges` | `sendHedges` |
| `hedger::add_position` | `addPosition` / `addPositionToVector` |
| `host::emit_quotes` | `sendQuoteOrder` |
| `ledger_bridge::on_hedge` | `hedgeMarket.addFAKAdjustment` |
| `strategy::run_cycle` | `runCycle` |
| `model::build` | `initializeFromRelativeValue` + `calculateHedgeRatiosAndWidthAndCommissionsAndQuoteSizes` |
