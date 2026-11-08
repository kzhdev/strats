#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace tachyon {

using price_t     = int64_t;
using qty_t       = uint64_t;
using inst_id_t   = int32_t;
using ord_id_t    = int64_t;
using timestamp_t = int64_t;

constexpr int64_t   DEFAULT_FIXED_PRICE_MULTIPLIER = 1'000'000'000;  // 1e9, matches CME's exponent of -9
constexpr price_t   NULL_PRICE = std::numeric_limits<price_t>::max();
constexpr inst_id_t INVALID_INSTRUMENT_ID = std::numeric_limits<inst_id_t>::min();
constexpr ord_id_t  INVALID_ORDER_ID = std::numeric_limits<ord_id_t>::max();
constexpr uint16_t  INVALID_SID = std::numeric_limits<uint16_t>::max();     // sid is tachyon internal symbol id
constexpr uint8_t   INVALID_STRAT_ID = std::numeric_limits<uint8_t>::max();
constexpr uint8_t   INVALID_APP_ID = std::numeric_limits<uint8_t>::max();
constexpr uint32_t  INVALID_PRODUCER_ID = std::numeric_limits<uint32_t>::max();

enum class component_type : uint8_t {
    MD,
    OM,
    __COUNT__,    // for internal use only
    UNKNOWN,
};

inline constexpr std::string_view to_string(component_type ct) noexcept {
    switch (ct) {
        case component_type::MD:
            return "MD";
        case component_type::OM:
            return "OM";
        default:
            return "UNKNOWN";
    }
}

constexpr auto COMPNONET_COUNT = static_cast<uint8_t>(component_type::__COUNT__);


}  // namespace tachyon
