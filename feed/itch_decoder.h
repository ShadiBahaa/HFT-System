#pragma once

#include <cstdint>
#include "core/types.h"
#include "core/market_data.h"

// Suppress MSVC padding warnings for packed wire structs
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4200 4103)
#endif

namespace hft::feed {

    using namespace hft::core;

    // =========================================================================
    // NASDAQ ITCH 5.0 Wire Protocol Structures
    // Binary, no field delimiters — parsed directly from DMA buffer (zero-copy)
    // =========================================================================

#pragma pack(push, 1)

    struct ITCHHeader {
        char     msg_type;
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];       // 6-byte nanosecond timestamp
    };

    struct ITCHAddOrder {          // Message type 'A'
        char     msg_type;           // 'A'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t order_ref;
        char     side;               // 'B' or 'S'
        uint32_t shares;
        char     stock[8];
        uint32_t price;              // price * 10000
    };

    struct ITCHAddOrderMPID {      // Message type 'F'
        char     msg_type;           // 'F'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t order_ref;
        char     side;
        uint32_t shares;
        char     stock[8];
        uint32_t price;
        char     attribution[4];
    };

    struct ITCHOrderExecuted {     // Message type 'E'
        char     msg_type;           // 'E'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t order_ref;
        uint32_t executed_shares;
        uint64_t match_number;
    };

    struct ITCHOrderExecutedPrice { // Message type 'C'
        char     msg_type;           // 'C'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t order_ref;
        uint32_t executed_shares;
        uint64_t match_number;
        char     printable;
        uint32_t execution_price;
    };

    struct ITCHOrderCancel {       // Message type 'X'
        char     msg_type;           // 'X'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t order_ref;
        uint32_t cancelled_shares;
    };

    struct ITCHOrderDelete {       // Message type 'D'
        char     msg_type;           // 'D'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t order_ref;
    };

    struct ITCHOrderReplace {      // Message type 'U'
        char     msg_type;           // 'U'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t orig_order_ref;
        uint64_t new_order_ref;
        uint32_t shares;
        uint32_t price;
    };

    struct ITCHTrade {             // Message type 'P' (non-cross)
        char     msg_type;           // 'P'
        uint16_t stock_locate;
        uint16_t tracking_number;
        uint8_t  timestamp[6];
        uint64_t order_ref;
        char     side;
        uint32_t shares;
        char     stock[8];
        uint32_t price;
        uint64_t match_number;
    };

#pragma pack(pop)

    // =========================================================================
    // ITCH Message Sizes — compile-time lookup
    // =========================================================================
    inline constexpr int itch_message_size(char msg_type) noexcept {
        switch (msg_type) {
            case 'A': return sizeof(ITCHAddOrder);
            case 'F': return sizeof(ITCHAddOrderMPID);
            case 'E': return sizeof(ITCHOrderExecuted);
            case 'C': return sizeof(ITCHOrderExecutedPrice);
            case 'X': return sizeof(ITCHOrderCancel);
            case 'D': return sizeof(ITCHOrderDelete);
            case 'U': return sizeof(ITCHOrderReplace);
            case 'P': return sizeof(ITCHTrade);
            default:  return -1;  // Unknown/unsupported
        }
    }

    // =========================================================================
    // ITCH Decoder — compile-time dispatch via template specialization
    // No virtual calls, no branches beyond the initial switch
    // =========================================================================

    template <char MsgType>
    struct ITCHHandler {
        static bool handle(const void*, MarketUpdate&) noexcept { return false; }
    };

    template <> struct ITCHHandler<'A'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };
    template <> struct ITCHHandler<'F'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };
    template <> struct ITCHHandler<'E'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };
    template <> struct ITCHHandler<'C'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };
    template <> struct ITCHHandler<'X'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };
    template <> struct ITCHHandler<'D'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };
    template <> struct ITCHHandler<'U'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };
    template <> struct ITCHHandler<'P'> {
        [[gnu::hot]] static bool handle(const void* data, MarketUpdate& out) noexcept;
    };

} // namespace hft::feed

#ifdef _MSC_VER
#pragma warning(pop)
#endif
