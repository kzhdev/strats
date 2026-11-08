#pragma once

#include <tachyon/core/common/types.hpp>
#include <tachyon/core/om/order_types.hpp>

#include <zenith/types/Endpoint.hpp>

#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace tachyon::trade {

using price_t     = tachyon::price_t;
using qty_t       = tachyon::qty_t;
using inst_id_t   = tachyon::inst_id_t;
using ord_id_t    = tachyon::ord_id_t;
using timestamp_t = tachyon::timestamp_t;

using order_side = tachyon::om::order_side;
using hedge_t = tachyon::om::order_type;
using endpoint_t = zenith::types::Endpoint;

constexpr uint32_t INVALID_INTERNAL_ID = std::numeric_limits<uint32_t>::max();

constexpr uint8_t BUY = static_cast<uint8_t>(order_side::BUY);
constexpr uint8_t SELL = static_cast<uint8_t>(order_side::SELL);

enum class strategy_id : uint8_t {
    MM,     // Market Maker
    RV,     // Relative Value
    AS,     // Arbie Spreader
    __COUNT__,
};

constexpr std::string_view to_string(strategy_id id) noexcept {
    switch(id) {
        case strategy_id::MM : return "MM";
        case strategy_id::RV : return "RV";
        case strategy_id::AS : return "AS";
        default:
            break;
    }
    return "UNKNOWN";
}

// MM hedge order is an LMT order, while RV and AS hedge is FAK.
// Only MM hedge order can rest internally and become part of the
// internal market, therefore only MM hedge orders have a `liquidity_source`
enum liquidity_source : uint8_t {
    LIQUIDITY_SOURCE_MM_HEDGE,
    LIQUIDITY_SOURCE_MM_QUOTE,
    LIQUIDITY_SOURCE_RV_QUOTE,
    LIQUIDITY_SOURCE_AS_QUOTE,
    LIQUIDITY_SOURCE_EXTERNAL,
    __LIQUIDITY_SOURCE_END__
};

constexpr std::string_view to_string(liquidity_source id) noexcept {
    switch(id) {
        case liquidity_source::LIQUIDITY_SOURCE_MM_HEDGE: return "LIQUIDITY_SOURCE_MM_HEDGE";
        case liquidity_source::LIQUIDITY_SOURCE_MM_QUOTE: return "LIQUIDITY_SOURCE_MM_QUOTE";
        case liquidity_source::LIQUIDITY_SOURCE_RV_QUOTE: return "LIQUIDITY_SOURCE_RV_QUOTE";
        case liquidity_source::LIQUIDITY_SOURCE_AS_QUOTE: return "LIQUIDITY_SOURCE_AS_QUOTE";
        case liquidity_source::LIQUIDITY_SOURCE_EXTERNAL: return "LIQUIDITY_SOURCE_EXTERNAL";
        default: return "INVALID";
    }
}

enum market_type : uint8_t {
    INTERNAL,
    EXTERNAL,
    __MARKET_TYPE_COUNT__
};

constexpr uint8_t MARKET_TYPE_COUNT = static_cast<uint8_t>(market_type::__MARKET_TYPE_COUNT__);

constexpr std::string_view to_string(market_type type) noexcept {
    switch(type) {
        case market_type::INTERNAL: return "INTERNAL";
        case market_type::EXTERNAL: return "EXTERNAL";
        default: return "INVALID";
    }
}

using market_key = int32_t;

inline constexpr market_key make_market_key(uint32_t internal_id,
                                            order_side side,
                                            market_type type) noexcept {
    return static_cast<market_key>((internal_id << 2U) |
                                   (static_cast<uint32_t>(side) << 1U) |
                                   static_cast<uint32_t>(type));
}

enum class shifts {ASIA, EUROPE, US, SAFE_MODE, __CURRENT_MARKET__};

template<typename T>
    requires std::is_enum_v<T>
inline constexpr std::underlying_type_t<T> to_int(T value) noexcept {
    return static_cast<std::underlying_type_t<T>>(value);
}

}   // end namespace tachyon::trade
