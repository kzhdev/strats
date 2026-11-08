#pragma once

// Standalone-build stub for the tachyon OM order types (PLAN.md §8, R7).
//
// `new_impl/common/types.hpp` includes the real framework header for `order_side` and
// `order_type`; that header lives in the tachyon tree, which is not part of this
// repository. The stub only has to provide the enumerators the new implementation
// actually names, with the same underlying type and the same enumerator values as the
// framework (side: BUY = 0, SELL = 1 -- the order transport encodes the side as the low
// bit of the order flags). Add to it when new code starts using more of the header; the
// real header always wins once the framework is on the include path
// (VIPER_RV_STUBS_DIR, see new_impl/CMakeLists.txt).

#include <cstdint>

namespace tachyon::om {

enum class order_side : uint8_t {
    BUY = 0,
    SELL = 1,
};

enum class order_type : uint8_t {
    LIMIT = 0,
    MARKET = 1,
    FAK = 2,  // fill-and-kill: what RV hedge orders are sent as
    FOK = 3,
};

}  // namespace tachyon::om
