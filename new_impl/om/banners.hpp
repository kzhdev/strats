#pragma once

#include <cstdint>

#include "exec/types.hpp"

// The external-quote gates: "may this intent actually go out, and at what price and size".
//
// Transcribed from the legacy external order management, branch by branch:
//   * banner            InternalMatchingEngine.h:32-54  (struct Banner)
//   * quote gate        InternalMatchingEngine.h:647-700 (synchronizeUnderBanners), :619-645
//                        (quoteWithMaxTicksOffTopLevelCheck), :703-711 (startQuoteBanner)
//   * hedge gate        InternalMatchingEngine.h:569-607 (sendHedge), :609-617 (startHedgingBanner)
//
// Everything here is a pure function of (state, config, intent, now), so every branch is pinnable by a
// unit test without a venue, a clock or an engine. That is the point of the split: the *decisions*
// are testable in isolation from the transport that carries out the decisions.

namespace tachyon::trade::om {

using exec::NULL_PRICE;
using exec::price_t;
using exec::qty_t;
using exec::side;

// Legacy Banner (:32-54). An open-ended "do not improve past this" window: `expiry > now` means
// banned. `expiry == 0` means the banner has never run.
struct banner {
    price_t price = NULL_PRICE;
    qty_t qty = 0;
    uint64_t expiry = 0;
    uint64_t ban_period = 0;

    bool is_banned(uint64_t now) const noexcept { return expiry > now; }
    bool was_running() const noexcept { return expiry != 0; }
    void expire() noexcept { expiry = 0; }

    // Legacy startQuoteBanner (:703-711) / startHedgingBanner (:609-617): record the quote and open a
    // fresh window. Note that expiry is `now + ban_period`, so a zero period leaves the banner
    // permanently expired -- that is the "off" configuration, and it keeps the fast path branch-free.
    void start(uint64_t now, price_t p, qty_t q) noexcept {
        price = p;
        qty = q;
        expiry = now + ban_period;
    }

    // Legacy in-place update while banned (:664-666, :584-585): the recorded quote moves but the
    // window does not, so a stream of non-improving quotes cannot extend the ban indefinitely.
    void refresh(price_t p, qty_t q) noexcept {
        price = p;
        qty = q;
    }
};

// The external book top, for the max-ticks-off-top-level check. `NULL_PRICE` means "no top",
// which the legacy code treats as "skip the check" (:624, :629).
struct market_top {
    price_t price = NULL_PRICE;
    qty_t size = 0;
};

struct gate_config {
    // Legacy _maxOffTopLevel: `< 0` disables the check (:623).
    price_t max_ticks_off_top_level = -1;
    // Legacy _quoteBanner._banPeriod / _hedgeBanner._banPeriod.
    uint64_t quote_ban_period = 0;
    uint64_t hedge_ban_period = 0;
};

// All the mutable per-contract state of the gates. One instance per contract; the two sides are
// independent, exactly as the legacy `_quoteBanner[Side]` / `_hedgeBanner[Side]` arrays are.
struct gate_state {
    banner quote_ban[2];
    banner hedge_ban[2];
    // What this object last forwarded, i.e. what it believes rests at the venue: the equivalent of
    // legacy `_om[Side].getPrice()` / `getRestingSize()` / `_om[Side]._hedge->_price`.
    price_t resting_quote_price[2] = {NULL_PRICE, NULL_PRICE};
    qty_t resting_quote_size[2] = {0, 0};
    price_t resting_hedge_price[2] = {NULL_PRICE, NULL_PRICE};
};

// What to do with one quote intent.
struct gate_result {
    // The intent to forward. When `send` is false these keep the incoming values so a caller that
    // ignores the flag cannot accidentally send something else.
    price_t price = NULL_PRICE;
    qty_t size = 0;
    bool send = true;
    // The size was forced to zero by the max-ticks-off-top-level check (:624-633). The intent still
    // goes out -- legacy quotes size 0 to keep the price visible while taking no risk.
    bool degraded = false;
    // Banned and the intent is an improvement, so it is dropped entirely (:659-678).
    bool suppressed = false;
    // The banner was (re)started or refreshed by this decision, for callers that need to log it.
    bool banner_updated = false;
};

namespace detail {

constexpr uint32_t idx(side s) noexcept { return static_cast<uint32_t>(s); }

// Legacy quoteWithMaxTicksOffTopLevelCheck (:619-645): outside the permitted distance from the top,
// the *size* is zeroed, the price is kept, and no banner is refreshed (`_outsideMaxOffTopLevel`).
constexpr bool degrade_off_top(const gate_config& cfg, side sd, price_t price,
                              const market_top& top) noexcept {
    if (cfg.max_ticks_off_top_level < 0 || top.price == NULL_PRICE) return false;
    if (sd == side::bid) return price + cfg.max_ticks_off_top_level < top.price;
    return price - cfg.max_ticks_off_top_level > top.price;
}

// The ban predicate, shared by the quote and hedge gates.
constexpr bool improves_on(const banner& b, side sd, price_t price, qty_t size) noexcept {
    return exec::is_better_price(price, b.price, sd) || (price == b.price && size >= b.qty);
}

}  // namespace detail

// The quote gate. Mirrors `synchronizeUnderBanners<Side>(price, size, now)` with the ban sources that
// phase 1 owns (the quote banner); the fill/reject banners and throttling join the same predicate in
// phase 2 (om/PLAN.md), which is why the ban test is a named helper rather than an inline expression.
//
// The one thing worth spelling out, because it reads backwards at first: while banned, a *better*
// quote is the case that gets suppressed, and a non-improving quote is the case that goes out. The
// ban rate-limits improvements (which is what causes flicker); a pull (size 0) or a same-price quote
// must still be able to reach the venue, otherwise the strategy could not get out of the market.
inline gate_result gate_quote(gate_state& st, const gate_config& cfg, side sd, price_t price, qty_t size,
                             const market_top& top, uint64_t now) noexcept {
    const uint32_t i = detail::idx(sd);
    banner& quote_ban = st.quote_ban[i];
    quote_ban.ban_period = cfg.quote_ban_period;

    gate_result r{price, size, true, false, false, false};

    if (quote_ban.is_banned(now)) {
        // :656-678 -- under a ban, an improving quote is dropped and everything else is forwarded.
        if (detail::improves_on(quote_ban, sd, price, size)) {
            r.send = false;
            r.suppressed = true;
            return r;
        }
        if (detail::degrade_off_top(cfg, sd, price, top)) {
            r.size = 0;
            r.degraded = true;
            return r;  // no banner motion for a degraded quote (:662 -> !outsetMaxOffTopLevel)
        }
        // :663-670: normally the recorded quote moves inside the open window; the start branch is
        // only reachable in phase 2, when a ban can come from the fill/reject sources instead.
        if (quote_ban.is_banned(now)) {
            quote_ban.refresh(price, size);
        } else {
            quote_ban.start(now, price, size);
        }
        r.banner_updated = true;
    } else {
        quote_ban.expire();  // :685 -- runs before the max-tick check, exactly as legacy orders it
        if (detail::degrade_off_top(cfg, sd, price, top)) {
            r.size = 0;
            r.degraded = true;
            return r;  // :686-698 skipped: the quote is sent, but no banner is opened
        }
        const price_t current = st.resting_quote_price[i];
        const qty_t current_size = st.resting_quote_size[i];
        // :687-697 -- there is a quote in the market and this one improves it, or there is none.
        if (current == NULL_PRICE || exec::is_better_price(price, current, sd) ||
            (price == current && size > current_size)) {
            quote_ban.start(now, price, size);
            r.banner_updated = true;
        }
    }
    return r;
}

// The hedge gate. Mirrors `sendHedge<Side>` (:569-607): a pay-up bypasses the banner (and records the
// price), an open ban only lets through a non-worsening hedge or a pull, and a fresh hedge that is
// worse than the resting one opens a ban.
inline gate_result gate_hedge(gate_state& st, const gate_config& cfg, side sd, price_t price, qty_t size,
                             bool pay_up, uint64_t now) noexcept {
    const uint32_t i = detail::idx(sd);
    banner& hedge_ban = st.hedge_ban[i];
    hedge_ban.ban_period = cfg.hedge_ban_period;

    gate_result r{price, size, true, false, false, false};

    if (pay_up) {  // :572-576
        hedge_ban.refresh(price, size);
        r.banner_updated = true;
        return r;
    }
    if (hedge_ban.is_banned(now)) {  // :579-588
        // Legacy names this `isQuoteWorse`: banner price better than the new price means the new
        // hedge is the worse one, which is the case that is allowed through.
        const bool is_worse = exec::is_better_price(hedge_ban.price, price, sd);
        if (!is_worse || (size == 0 && hedge_ban.qty != 0)) {
            hedge_ban.refresh(price, size);
            r.banner_updated = true;
            return r;
        }
        r.send = false;
        r.suppressed = true;
        return r;
    }

    // :589-604 -- not banned: forward, then open a ban iff this hedge is worse than the resting one.
    const price_t resting = st.resting_hedge_price[i];
    if (resting == NULL_PRICE || exec::is_better_price(resting, price, sd)) {
        hedge_ban.start(now, price, size);
        r.banner_updated = true;
    }
    return r;
}

}  // namespace tachyon::trade::om
