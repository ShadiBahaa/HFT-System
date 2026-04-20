#pragma once

#include <cstdint>
#include <cstring>
#include "core/types.h"
#include "core/market_data.h"

namespace hft::feed {

    using namespace hft::core;

    // =========================================================================
    // CBOE PITCH 2.x Wire Protocol Structures
    // Binary, little-endian, length-prefixed. Parsed directly from DMA buffer.
    // =========================================================================

#pragma pack(push, 1)

    struct PITCHAddOrder {                 // Type 0x21 / 0x22 (short / long)
        uint8_t  length;
        uint8_t  msg_type;                 // 0x21 = short, 0x22 = long
        uint32_t time_offset;              // ns since time header
        uint64_t order_id;
        uint8_t  side;                     // 'B' / 'S'
        uint32_t quantity;
        char     symbol[6];
        uint64_t price;                    // fixed-point * 10000
        uint8_t  flags;
    };

    struct PITCHOrderExecuted {            // Type 0x23
        uint8_t  length;
        uint8_t  msg_type;
        uint32_t time_offset;
        uint64_t order_id;
        uint32_t executed_qty;
        uint64_t execution_id;
    };

    struct PITCHOrderCancel {              // Type 0x24
        uint8_t  length;
        uint8_t  msg_type;
        uint32_t time_offset;
        uint64_t order_id;
        uint32_t cancelled_qty;
    };

    struct PITCHOrderDelete {              // Type 0x25
        uint8_t  length;
        uint8_t  msg_type;
        uint32_t time_offset;
        uint64_t order_id;
    };

    struct PITCHTrade {                    // Type 0x2A
        uint8_t  length;
        uint8_t  msg_type;
        uint32_t time_offset;
        uint64_t order_id;
        uint8_t  side;
        uint32_t quantity;
        char     symbol[6];
        uint64_t price;
        uint64_t execution_id;
    };

#pragma pack(pop)

    enum PITCHType : uint8_t {
        PITCH_ADD_ORDER_SHORT  = 0x21,
        PITCH_ADD_ORDER_LONG   = 0x22,
        PITCH_EXECUTED         = 0x23,
        PITCH_CANCEL           = 0x24,
        PITCH_DELETE           = 0x25,
        PITCH_TRADE            = 0x2A
    };

    // =========================================================================
    // PitchDecoder — zero-copy dispatch for PITCH messages
    // Handler is a callable receiving a normalized MarketUpdate.
    // =========================================================================
    template <typename Handler>
    [[gnu::hot]] inline size_t dispatch_pitch(
        const uint8_t* buf, size_t len, Handler&& handler) noexcept
    {
        size_t off = 0;
        size_t count = 0;
        while (off + 2 <= len) {
            const uint8_t msg_len = buf[off];
            if (msg_len == 0 || off + msg_len > len) break;

            const uint8_t msg_type = buf[off + 1];
            MarketUpdate update{};
            update.timestamp = 0;

            switch (msg_type) {
            case PITCH_ADD_ORDER_SHORT:
            case PITCH_ADD_ORDER_LONG: {
                if (msg_len < sizeof(PITCHAddOrder)) break;
                const auto* m = reinterpret_cast<const PITCHAddOrder*>(buf + off);
                update.type = UpdateType::ADD;
                update.side = (m->side == 'B') ? Side::BUY : Side::SELL;
                update.price = static_cast<Price>(m->price);
                update.quantity = static_cast<Quantity>(m->quantity);
                update.order_ref = m->order_id;
                handler(update);
                ++count;
                break;
            }
            case PITCH_EXECUTED: {
                if (msg_len < sizeof(PITCHOrderExecuted)) break;
                const auto* m = reinterpret_cast<const PITCHOrderExecuted*>(buf + off);
                update.type = UpdateType::EXECUTE;
                update.quantity = static_cast<Quantity>(m->executed_qty);
                update.order_ref = m->order_id;
                handler(update);
                ++count;
                break;
            }
            case PITCH_CANCEL: {
                if (msg_len < sizeof(PITCHOrderCancel)) break;
                const auto* m = reinterpret_cast<const PITCHOrderCancel*>(buf + off);
                update.type = UpdateType::MODIFY;
                update.quantity = static_cast<Quantity>(m->cancelled_qty);
                update.order_ref = m->order_id;
                handler(update);
                ++count;
                break;
            }
            case PITCH_DELETE: {
                if (msg_len < sizeof(PITCHOrderDelete)) break;
                const auto* m = reinterpret_cast<const PITCHOrderDelete*>(buf + off);
                update.type = UpdateType::DELETE;
                update.order_ref = m->order_id;
                handler(update);
                ++count;
                break;
            }
            case PITCH_TRADE: {
                if (msg_len < sizeof(PITCHTrade)) break;
                const auto* m = reinterpret_cast<const PITCHTrade*>(buf + off);
                update.type = UpdateType::TRADE;
                update.side = (m->side == 'B') ? Side::BUY : Side::SELL;
                update.price = static_cast<Price>(m->price);
                update.quantity = static_cast<Quantity>(m->quantity);
                update.order_ref = m->order_id;
                handler(update);
                ++count;
                break;
            }
            default:
                break;
            }
            off += msg_len;
        }
        return count;
    }

} // namespace hft::feed
