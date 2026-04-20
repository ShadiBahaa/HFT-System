#pragma once

#include <cstdint>

#include "core/types.h"
#include "core/market_data.h"

namespace hft::oms {

    using namespace hft::core;

    // =========================================================================
    // Order — internal representation tracked by the OMS
    // =========================================================================
    enum class OrderStatus : uint8_t {
        PENDING_NEW     = 0,
        OPEN            = 1,
        PARTIALLY_FILLED = 2,
        FILLED          = 3,
        PENDING_CANCEL  = 4,
        CANCELLED       = 5,
        REJECTED        = 6
    };

    struct Order {
        OrderId     client_order_id{0};
        OrderId     exchange_order_id{0};
        InstrumentId instrument_id{0};
        Side        side{Side::UNKNOWN};
        OrderType   order_type{OrderType::LIMIT};
        Price       price{0};
        Quantity    quantity{0};
        Quantity    filled_qty{0};
        Quantity    leaves_qty{0};
        OrderStatus status{OrderStatus::PENDING_NEW};
        VenueId     venue{VenueId::UNKNOWN};
        TimestampNs create_time{0};
        TimestampNs last_update_time{0};
    };

    // =========================================================================
    // VenueMetrics — per-venue state for smart routing decisions
    // =========================================================================
    struct VenueMetrics {
        Price    best_bid{0};
        Price    best_ask{0};
        Quantity bid_size{0};
        Quantity ask_size{0};
        uint32_t latency_p50_ns{0};
        uint32_t latency_p99_ns{0};
        int32_t  maker_fee_bps{0};      // Negative = rebate
        int32_t  taker_fee_bps{0};
        bool     active{false};
    };

} // namespace hft::oms
