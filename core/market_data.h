#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include "core/types.h"

namespace hft::core {

    // =========================================================================
    // Actions and message types used across the pipeline
    // =========================================================================

    enum class Action : uint8_t {
        NONE        = 0,
        BUY         = 1,
        SELL        = 2,
        CANCEL      = 3,
        REPLACE     = 4
    };

    enum class OrderType : uint8_t {
        LIMIT       = 1,
        MARKET      = 2,
        IOC         = 3,  // Immediate or Cancel
        FOK         = 4   // Fill or Kill
    };

    enum class TimeInForce : uint8_t {
        DAY         = 0,
        GTC         = 1,  // Good Till Cancel
        IOC         = 2,
        FOK         = 3
    };

    enum class UpdateType : uint8_t {
        ADD         = 1,
        MODIFY      = 2,
        DELETE      = 3,
        EXECUTE     = 4,
        TRADE       = 5,
        SNAPSHOT    = 6
    };

    enum class RiskResult : uint8_t {
        PASS             = 0,
        POSITION_BREACH  = 1,
        SIZE_BREACH      = 2,
        PRICE_BREACH     = 3,
        NOTIONAL_BREACH  = 4,
        RATE_BREACH      = 5,
        KILL_SWITCH      = 6,
        THROTTLED        = 7
    };

    // =========================================================================
    // MarketUpdate — normalized output of the feed handler
    // Pushed into SPSC ring from feed handler to order book builder
    // Kept compact for cache-friendly transport
    // =========================================================================
    struct alignas(64) MarketUpdate {
        TimestampNs     timestamp;          // Exchange timestamp (ns)
        InstrumentId    instrument_id;      // Instrument index
        UpdateType      type;               // ADD/MODIFY/DELETE/EXECUTE/TRADE
        Side            side;               // BUY/SELL
        Price           price;              // Fixed-point price
        Quantity         quantity;           // Shares/contracts
        OrderId         order_ref;          // Exchange order reference
        uint64_t        sequence;           // Feed sequence number
    };
    static_assert(sizeof(MarketUpdate) <= 64, "MarketUpdate must fit in one cache line");

    // =========================================================================
    // Signal — output of a strategy's evaluation
    // =========================================================================
    struct Signal {
        Action          action{Action::NONE};
        Side            side{Side::UNKNOWN};
        Price           price{0};
        Quantity         quantity{0};
        InstrumentId    instrument_id{0};
        OrderId         target_order_id{0};  // For cancel/replace
        double          urgency{0.0};        // 0.0 = no signal, 1.0 = max urgency
    };

    // =========================================================================
    // OrderRequest — sent from strategy to OMS via SPSC ring
    // =========================================================================
    struct OrderRequest {
        Action          action{Action::NONE};
        Side            side{Side::UNKNOWN};
        OrderType       order_type{OrderType::LIMIT};
        TimeInForce     tif{TimeInForce::DAY};
        InstrumentId    instrument_id{0};
        Price           price{0};
        Quantity         quantity{0};
        OrderId         client_order_id{0};
        OrderId         orig_order_id{0};     // For cancel/replace
        TimestampNs     timestamp{0};
    };

    // =========================================================================
    // NewOrderSingle — fully formed order ready for wire encoding
    // =========================================================================
    struct NewOrderSingle {
        char            cl_ord_id[20]{};
        char            symbol[8]{};
        Side            side{Side::UNKNOWN};
        OrderType       ord_type{OrderType::LIMIT};
        TimeInForce     tif{TimeInForce::DAY};
        Price           price{0};
        Quantity         qty{0};
        VenueId         venue{VenueId::UNKNOWN};
        TimestampNs     timestamp{0};
    };

    // =========================================================================
    // ExecutionReport — received from exchange after order action
    // =========================================================================
    enum class ExecType : uint8_t {
        NEW         = 0,
        PARTIAL     = 1,
        FILL        = 2,
        CANCELLED   = 3,
        REPLACED    = 4,
        REJECTED    = 5
    };

    struct ExecutionReport {
        OrderId         order_id{0};
        OrderId         cl_ord_id{0};
        ExecType        exec_type{ExecType::NEW};
        Side            side{Side::UNKNOWN};
        InstrumentId    instrument_id{0};
        Price           price{0};
        Quantity         filled_qty{0};
        Quantity         leaves_qty{0};
        TimestampNs     timestamp{0};
        VenueId         venue{VenueId::UNKNOWN};
    };

    // =========================================================================
    // BookSignal — compact snapshot pushed from order book to strategy
    // Contains only what the strategy needs for signal generation
    // =========================================================================
    struct alignas(64) BookSignal {
        InstrumentId    instrument_id{0};
        Price           best_bid{0};
        Price           best_ask{0};
        Quantity         bid_size{0};
        Quantity         ask_size{0};
        uint32_t        bid_orders{0};
        uint32_t        ask_orders{0};
        Price           mid_price{0};
        Price           spread{0};
        TimestampNs     timestamp{0};
    };

} // namespace hft::core
