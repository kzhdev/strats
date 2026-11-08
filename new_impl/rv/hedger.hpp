#pragma once

// Hedge decisions: the fill -> request -> residual -> tiled order path, and the position /
// vector-exposure bookkeeping that closes it.
//
// Split out of `strategy` because it is the one part of the cycle that *changes* the book:
// quotes only read positions (`state.position`, `state.pos_vec`), this object writes them.
// Keeping it separate makes that ownership explicit and lets the residual/club arithmetic be
// tested without a full strategy.
//
// `run()` reports back whether the engine must disable itself (legacy S12/S14 fail-safes):
// the hedger does not own the enabled flag, the strategy does.

#include <array>
#include <cstdint>

#include "model.hpp"
#include "quotes.hpp"
#include "state.hpp"
#include "types.hpp"

namespace tachyon::trade::rv {

// Single seam to the legacy order path (PLAN.md §4.7).
struct order_port {
    void (*send_hedge)(void* ctx, const hedge_order& order) = nullptr;
    void (*send_quote)(void* ctx, uint32_t contract) = nullptr;
    void* ctx = nullptr;
};

class hedger {
public:
    hedger(const model& m, const quote_policy& policy, const order_port& port) noexcept;

    // ---- inputs --------------------------------------------------------------
    // Legacy handleFill: accumulate the signed fill into the contract's hedge request.
    void on_fill(uint32_t contract, side sd, qty_t qty) noexcept;
    // Rhone clip limit (legacy SetRhoneLimits): a request larger than this disables RV (S12).
    void set_max_clip(uint32_t contract, price_t clip) noexcept { max_clip_[contract] = clip; }

    // ---- cycle ---------------------------------------------------------------
    bool has_request() const noexcept { return request_count_ > 0; }
    uint32_t request_count() const noexcept { return request_count_; }
    // Monotonic hedge id counter (compared structurally by the tests, not by value).
    uint32_t hedge_id() const noexcept { return hedge_id_; }

    // Process the oldest pending request: at most one hedge contract per cycle (S10).
    // Returns true when a fail-safe fired and the strategy must disable itself.
    bool run(cycle_state& s) noexcept;

private:
    struct pending_request {
        uint32_t contract = 0;
        double size = 0.0;
    };

    // legacy getResiduals (:578-607): how much of the quoted size is still unhedged.
    double residual(const cycle_state& s, uint32_t contract, side sd) const noexcept;
    // Legacy hedge tiling over the club of leans at one rounded price.
    double send_hedges(cycle_state& s, uint32_t contract, double size, side sd,
                       bool& remove_request, double residual_abs) noexcept;
    qty_t order_size(uint32_t lean, double hedge_ratio, qty_t min_available, double& need,
                     double size, double residuals, double quote_size) const noexcept;
    qty_t adjusted_market_size(const cycle_state& s, uint32_t contract, side sd,
                               bool external) const noexcept;
    // legacy addPosition + addPositionToVector (:2602-2637)
    void add_position(cycle_state& s, uint32_t contract, double size) noexcept;

    void append_request(uint32_t contract, double size) noexcept;
    void erase_request(uint32_t index) noexcept;

    const model& m_;
    quote_policy policy_;
    order_port port_;

    std::array<pending_request, MAX_CONTRACTS> requests_{};
    std::array<uint32_t, MAX_CONTRACTS> request_index_{};  // contract -> request index + 1
    uint32_t request_count_ = 0;
    std::array<price_t, MAX_CONTRACTS> max_clip_{};
    uint32_t hedge_id_ = 0;
    uint32_t skip_club_count_ = 0;
    // Set when a fail-safe fires (S12 clip exceeded, S14 50-skip escape); `run()` returns it
    // so the strategy can disable itself without the hedger owning the enabled flag.
    bool disable_requested_ = false;
};

}  // namespace tachyon::trade::rv
