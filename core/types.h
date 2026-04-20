#pragma once

#include <cstdint>
#include <string_view>

namespace hft::core {

    // Strongly-typed system identifiers
    using InstrumentId = uint16_t;
    using OrderId      = uint64_t;
    
    // Fixed-point pricing (e.g., actual_price * 10,000)
    // Double is avoided to prevent floating point inaccuracies and FPU penalties
    using Price = int64_t;
    
    // Quantity tracking
    using Quantity = int32_t;

    // Time representation
    using TimestampNs = uint64_t;

    // Order side
    enum class Side : int8_t {
        BUY  = 1,
        SELL = 2,
        UNKNOWN = 0
    };

    // Execution venue mapping
    enum class VenueId : uint8_t {
        NASDAQ = 1,
        CME    = 2,
        CBOE   = 3,
        UNKNOWN = 0
    };

    // Standard helper to format Side (non-allocating)
    constexpr std::string_view to_string(Side side) noexcept {
        switch (side) {
            case Side::BUY: return "BUY";
            case Side::SELL: return "SELL";
            default: return "UNKNOWN";
        }
    }

} // namespace hft::core
