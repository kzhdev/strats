# External order management (`new_impl/om`) — scope and first phase

The implied engine deliberately stops at *"this is the residual, send it"*. Everything that decides
**whether an order may go out, at what price and with what lifetime** is legacy external order
management, and it lives here. `exec/PLAN.md` D-EX6 lists it as the deferred surface; this document is
that surface, scoped.

## Why a decorator

The seam already exists: the engine emits `exchange_order` intents to an `exec::exchange_gateway`.
So the OM does **not** touch the engine -- it wraps the gateway:

```
  implied_engine  --intent-->  om::bannered_gateway  --allowed intent-->  <OM / venue>
                                     ^      ^
                    external book top |      | clock (cycles)
```

That is a drop-in change at the composition root, it is testable without a venue, and the engine keeps
its single responsibility.

## Phases

| Phase | Content | Needs inbound events? | State |
|---|---|---|---|
| 1 | quote ban, hedge ban, `maxTicksOffTopLevel`, "not better -> suppress" | no | **this phase** |
| 2 | fill banner, reject banner, throttling (`THROTTLING_MODE`), `goThroughAllLevels` | **yes**: reject/ack/cancel/fill from the venue | deferred |
| 3 | GTC reconciliation, pay-up matching, economic numbers, IOP tracking | no (but large) | deferred |

Phase 2 is where the legacy `OrderManager` weight sits (`src/OrderManager.h`, ~4 000 lines). It is
sequenced after phase 1 because it is the piece that needs the venue's inbound path to exist first.

## Phase-1 decisions

| # | Decision | Value |
|---|---|---|
| D-OM1 | Placement | decorator over `exec::exchange_gateway`; the engine is unchanged |
| D-OM2 | Clock | injected `now` provider (cycles), so a test drives time exactly; production passes `clockUtil::GetCycles` |
| D-OM3 | External book | `set_market_top(contract, side, price, size)`, fed by the MD path; `maxTicksOffTopLevel` reads it |
| D-OM4 | "Resting" state | the last quote this object forwarded (price + size) -- the equivalent of legacy `_om[Side].getPrice()/getRestingSize()` |
| D-OM5 | Parity | the gate logic is transcribed statement by statement from `synchronizeUnderBanners` (`InternalMatchingEngine.h:647-700`), `quoteWithMaxTicksOffTopLevelCheck` (:619-645) and `sendHedge` (:569-607), and pinned by a test per branch |

## What phase 1 must not lose

The legacy gate is **not** "suppress everything while banned", and the direction is easy to get
backwards: while a ban is open, an *improving* quote is the one that is dropped (`:659-678`, "not
updating") and a non-improving quote is the one that goes out. The ban rate-limits improvements, which
are what cause flicker; a pull (size 0) or a same-price quote must still reach the venue, or the
strategy could not get out of the market at all. Every branch is a named case in
`tests/om_banner_tests.cpp`, transcribed with the legacy line numbers, precisely because guessing this
from the outside gets it wrong.
