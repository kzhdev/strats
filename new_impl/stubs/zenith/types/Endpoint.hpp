#pragma once

// Standalone-build stub for zenith's Endpoint enum (PLAN.md §8, R7).
//
// The RV engine itself carries the venue as `contract_input::venue` / `model::venue`
// (0 = CME, 1 = ICE, 2 = EUREX, 3 = JPX, 4 = other -- see rv/config.hpp), so the host is
// the only place that maps to the framework type. Only the enumerators a host adapter can
// map onto are declared here.

#include <cstdint>

namespace zenith::types {

enum class Endpoint : uint8_t {
    CME = 0,
    ICE = 1,
    EUREX = 2,
    JPX = 3,
    OTHER = 4,
};

}  // namespace zenith::types
