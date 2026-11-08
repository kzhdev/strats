#pragma once

#include <common/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace tachyon::trade {

enum class reservation_type : uint8_t {
    HEDGE             = 1 << 0,
    AGGRESSIVE_QUOTE  = 1 << 1,
    ANTICIPATED_HEDGE = 1 << 2,
};

using reservation_type_mask = uint8_t;

constexpr reservation_type_mask to_mask(reservation_type type) noexcept {
    return static_cast<reservation_type_mask>(type);
}

constexpr std::string_view to_string(reservation_type type) noexcept {
    switch (type) {
        case reservation_type::HEDGE: return "HEDGE";
        case reservation_type::AGGRESSIVE_QUOTE: return "AGGRESSIVE_QUOTE";
        case reservation_type::ANTICIPATED_HEDGE: return "ANTICIPATED_HEDGE";
    }
    return "INVALID";
}

struct reservation_id {
    static constexpr uint32_t INVALID = 0;

    uint32_t value = INVALID;

    explicit constexpr operator bool() const noexcept {
        return value != INVALID;
    }

    friend constexpr bool operator==(reservation_id lhs,
                                     reservation_id rhs) noexcept {
        return lhs.value == rhs.value;
    }
};

struct reservation_filter {
    // Used for HEDGE reservations only. END means every source is counted.
    liquidity_source source = liquidity_source::__LIQUIDITY_SOURCE_END__;
    reservation_type_mask exclude_type = 0;
    uint32_t ignore_group_id = 0;
    reservation_id ignore_reservation = {};
};

enum class reservation_state : uint8_t {
    RESERVED,
    EXTERNAL_WORKING,
    COMPLETED,
    __RESERVATION_STATE_COUNT__
};

constexpr std::string_view to_string(reservation_state state) noexcept {
    switch (state) {
        case reservation_state::RESERVED: return "RESERVED";
        case reservation_state::EXTERNAL_WORKING: return "EXTERNAL_WORKING";
        case reservation_state::COMPLETED: return "COMPLETED";
        default: return "INVALID";
    }
}

struct reservation {
    reservation_id id;
    market_key target = 0;
    liquidity_source source = liquidity_source::__LIQUIDITY_SOURCE_END__;
    uint32_t hedge_id = 0;
    ord_id_t ord_id = INVALID_ORDER_ID;
    uint32_t group_id = 0;
    qty_t quantity = 0;
    reservation_type type = reservation_type::HEDGE;
    reservation_state state = reservation_state::RESERVED;
};

struct reservation_group {
    static constexpr uint32_t MAX_HEDGE_COUNT = 32;

    uint32_t id = 0;
    qty_t quantity = 0;
    liquidity_source source = liquidity_source::__LIQUIDITY_SOURCE_END__;
    market_key opposite_market = 0;
    std::array<market_key, MAX_HEDGE_COUNT> hedge_markets{};
    uint8_t hedge_market_count = 0;
    bool active = false;
};

// Tracks temporary claims against displayed liquidity. A market's executable
// quantity is its displayed size minus the reservations selected by the query.
// HEDGE reservations are source-scoped; quote-group reservations are not.
class liquidity_ledger {
public:
    qty_t available(
        market_key market,
        qty_t display_size,
        reservation_filter filter = {}) const noexcept;

    qty_t reserved(
        market_key market,
        reservation_filter filter = {}) const noexcept;

    reservation_id reserve_hedge(
        market_key market,
        liquidity_source source,
        uint32_t hedge_id,
        qty_t quantity);

    reservation_id reserve(
        reservation_type type,
        market_key market,
        liquidity_source source,
        qty_t quantity,
        uint32_t hedge_id = 0,
        uint32_t group_id = 0);

    bool acknowledge_external(
        reservation_id id,
        ord_id_t ord_id,
        qty_t actual_external_quantity);

    // Migration-friendly overload for the existing Executor acknowledgement,
    // which identifies a hedge by hedge ID and the external market key.
    bool acknowledge_external(
        uint32_t hedge_id,
        market_key target,
        ord_id_t ord_id,
        qty_t actual_external_quantity);

    bool complete(reservation_id id);
    bool complete_by_hedge_id(uint32_t hedge_id, market_key target);
    bool complete_by_order_id(ord_id_t ord_id);

    // Quote-group reservations are released/resized atomically because one
    // aggressive quote reserves both opposite-market liquidity and prospective
    // hedge liquidity on every target market.
    uint32_t create_quote_group(
        market_key opposite_market,
        liquidity_source source,
        qty_t quantity,
        std::span<const market_key> hedge_markets);

    bool resize_quote_group(uint32_t group_id, qty_t quantity);
    bool release_group(
        uint32_t group_id,
        std::vector<market_key>* affected_markets = nullptr);

    bool invalidate_market(
        market_key market,
        std::vector<market_key>* affected_markets = nullptr);

    const reservation_group* group(uint32_t group_id) const noexcept;
    std::size_t active_reservation_count() const noexcept;
private:
    bool counts_toward_availability(
        const reservation& reservation,
        market_key market,
        const reservation_filter& filter) const noexcept;

    reservation* find_reservation(reservation_id id) noexcept;
    reservation_group* find_group(uint32_t group_id) noexcept;
    uint32_t next_reservation_id() noexcept;
    uint32_t next_group_id() noexcept;

    static void add_affected_market(
        std::vector<market_key>* affected_markets,
        market_key market);

    std::vector<reservation> reservations_;
    std::vector<reservation_group> groups_;
    uint32_t next_reservation_id_ = reservation_id::INVALID + 1;
    uint32_t next_group_id_ = 1;
};

}   // end of namesapce tachyon::trade