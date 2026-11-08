#pragma once

#include <cstdint>

#include "model.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::rv {

// Hooks into the legacy host. Everything is optional (nullptr => legacy-neutral
// behaviour) so the engine is testable standalone.
struct quote_policy {
    // MaxTicksThroughMarket validation (legacy createQuote, PLAN.md §1.2).
    // Returns true when the computed lean price is acceptable.
    bool (*validate)(void* ctx, uint32_t contract, side sd, price_t lean_price,
                     uint32_t lean) = nullptr;
    void* validate_ctx = nullptr;
    // Displayed-size adjustment from the liquidity ledger (reservations).
    qty_t (*adjusted_size)(void* ctx, uint32_t contract, side sd, bool external,
                           qty_t raw) = nullptr;
    void* adjusted_ctx = nullptr;
    // A displayed price level was replaced: legacy Market::update flushes the FAK
    // adjustments for that market (Common.h:636-644). Internal markets never flush
    // (updateInternalMarket, :673-679), so this only fires for external books.
    void (*price_changed)(void* ctx, uint32_t contract, side sd, bool external) = nullptr;
    void* price_changed_ctx = nullptr;
};

inline qty_t to_valid_qty(qty_t qty, qty_t lot) noexcept {
    return lot > 1 ? (qty / lot) * lot : qty;
}

// Lean maths of a (traded contract, lean contract) pair: what it costs to quote contract
// `c` off the book of contract `lean`, and how much size that lean can support.
//
// Shared by quoting (a quote's price *is* the best lean price) and hedging (the club walk
// prices each hedge with the same maths), so the two cannot drift apart. Deliberately
// *stateless* (hence the static methods): the maths is a pure function of (model, state), and
// holding a `const model&` member would add a second indirection to every table access in the
// hottest loop of the engine -- measured at ~5% of the cycle (PLAN §7.3).
class lean_calculator {
public:

    // Unrounded lean price (legacy calculateQuote<Side, true>); NULL_PRICE when the lean must
    // be skipped (no market on that side, or a zero ratio outside the book).
    static price_t price(const model& m, const cycle_state& s, uint32_t contract, side sd,
                         uint32_t lean, bool lean_external) noexcept {
        const size_t k = m.pair(contract, lean);
        const double ratio = m.hedge_ratio[k];
        const market_state& lm = s.market(lean, sd, lean_external);
        const bool has_price = lm.price != tachyon::NULL_PRICE;
        // legacy: ratio != 0 requires a lean price; ratio == 0 only quotes externally
        if (!((has_price && ratio != 0.0) || (ratio == 0.0 && lean_external))) {
            return tachyon::NULL_PRICE;
        }
        const price_t cost = sd == side::bid ? s.stf[lean] - lm.price : lm.price - s.stf[lean];
        const price_t hedge = lean_calculator::cost(m, contract, lean, cost);
        const price_t fair = s.stf[contract];
        const price_t width = m.width_extra[contract];
        const price_t commission = m.commission_scaled[k];
        return sd == side::bid ? fair - width - hedge - commission
                               : fair + width + hedge + commission;
    }

    // Same, without the negative-cost clamp: legacy calculateQuote<Side, false> /
    // calculateHedgingCost<false>, used only by validateMaxTicksThroughMarket (:2809-2818).
    // The clamp only fires when the hedge cost is negative AND exceeds 95% of the width, so
    // this differs from price() only on deep-in-the-money leans.
    static price_t price_without_cost_cap(const model& m, const cycle_state& s, uint32_t contract,
                                          side sd, uint32_t lean, bool lean_external) noexcept {
        const size_t k = m.pair(contract, lean);
        const double ratio = m.hedge_ratio[k];
        const market_state& lm = s.market(lean, sd, lean_external);
        const bool has_price = lm.price != tachyon::NULL_PRICE;
        if (!((has_price && ratio != 0.0) || (ratio == 0.0 && lean_external))) {
            return tachyon::NULL_PRICE;
        }
        const price_t cost = sd == side::bid ? s.stf[lean] - lm.price : lm.price - s.stf[lean];
        const price_t hedge =
            m.hedge_unity[k] ? static_cast<price_t>(static_cast<double>(cost) * ratio)
                              : static_cast<price_t>(static_cast<double>(cost) * ratio *
                                                     m.pmult[lean] / m.pmult[contract] *
                                                     m.qmult[lean] / m.qmult[contract]);
        const price_t fair = s.stf[contract];
        const price_t width = m.width_extra[contract];
        const price_t commission = m.commission_scaled[k];
        return sd == side::bid ? fair - width - hedge - commission
                               : fair + width + hedge + commission;
    }

    // Size the lean supports (legacy calculateSize): capped by market size / hedge ratio and
    // by the precomputed per-pair quote size. `raw_market_size` is the displayed size, which
    // the policy may reduce to account for outstanding reservations.
    static qty_t size(const model& m, const cycle_state& s, uint32_t contract, side sd,
                      uint32_t lean, bool lean_external, qty_t raw_market_size,
                      const quote_policy& policy) noexcept {
        const size_t k = m.pair(contract, lean);
        const double ratio = m.hedge_ratio[k];
        const double max_quote_size = m.quote_size[k];
        qty_t market_size = raw_market_size;
        if (policy.adjusted_size != nullptr) {
            market_size = policy.adjusted_size(policy.adjusted_ctx, lean, sd, lean_external,
                                               raw_market_size);
        }
        double size = max_quote_size;
        if (ratio > 0) {
            const double possible = static_cast<double>(market_size) / ratio;
            size = possible > max_quote_size ? max_quote_size : possible;
        }
        return static_cast<qty_t>(size);
    }

    // ---- hedge cost scaling (legacy :2963-3004) -----------------------------
    // S5: negative hedge cost clamped to 95% of the width.
    static price_t clamp_negative_cost(price_t hedging, price_t width) noexcept {
        if (hedging < 0) {
            const price_t max_cost = static_cast<price_t>(0.95 * static_cast<double>(width));
            if (-hedging > max_cost) {
                return -max_cost;
            }
        }
        return hedging;
    }

    // Legacy order, transcribed from :2978 (four divisions).
    static price_t cost_exact(price_t cost, double ratio, double lean_pm, double contract_pm,
                              double lean_qm, double contract_qm, price_t width) noexcept {
        return clamp_negative_cost(static_cast<price_t>(static_cast<double>(cost) * ratio * lean_pm /
                                                        contract_pm * lean_qm / contract_qm),
                                   width);
    }

    // Fast path, used only when all four multipliers are exactly 1.0 — then it is
    // bit-identical to the legacy expression, which the oracle test proves.
    static price_t cost_unity(price_t cost, double ratio, price_t width) noexcept {
        return clamp_negative_cost(static_cast<price_t>(static_cast<double>(cost) * ratio), width);
    }

    // Cost of leaning on `lean` for `contract`: one predictable branch per pair (the model
    // precomputed whether the four multipliers are all 1.0).
    static price_t cost(const model& m, uint32_t contract, uint32_t lean, price_t cost) noexcept {
        const size_t pair = m.pair(contract, lean);
        const price_t width = m.width_extra[contract];
        if (m.hedge_unity[pair]) {
            return cost_unity(cost, m.hedge_ratio[pair], width);
        }
        return cost_exact(cost, m.hedge_ratio[pair], m.pmult[lean], m.pmult[contract],
                          m.qmult[lean], m.qmult[contract], width);
    }
};

// Quote construction for the traded contracts: the best lean across a contract's lean row,
// the external "club" aggregation, and the maxTicksThroughMarket gate that guards both.
//
// The gate lives here rather than in the host because it is arithmetic on the lean price
// (legacy validateMaxTicksThroughMarket is a RelativeValue method); the host's policy callback
// only decides what a violation *means* (disable the strategy, raise an alert).
class quote_engine {
public:
    quote_engine(const model& m, const quote_policy& policy) noexcept
        : m_(m), policy_(policy) {}

    // Build the quote for one contract and side (legacy Quote::createQuote).
    void build(cycle_state& s, uint32_t contract, side sd) const noexcept;

    // Build quotes for every traded contract, both sides.
    void build_all(cycle_state& s) const noexcept;

    // Legacy validateMaxTicksThroughMarket (:2795-2824): true when the quote this lean would
    // produce sits closer to the traded contract's own opposite side than the configured
    // maxTicksThroughMarket. A missing opposite side accepts, as legacy does.
    bool within_max_ticks_through_market(const cycle_state& s, uint32_t contract, side sd,
                                         uint32_t lean) const noexcept;

    // Legacy calculateTicksThroughMarket (Common.h:445-453): distance in price units from the
    // quote to the opposite side of the traded contract's own book. Positive means the quote
    // sits at or through that price when it is >= 0.
    static price_t ticks_through_market(price_t quote_price, price_t other_side_best,
                                        side sd) noexcept {
        return sd == side::bid ? quote_price - other_side_best : other_side_best - quote_price;
    }

private:
    const model& m_;
    quote_policy policy_;
};

}  // namespace tachyon::trade::rv
