#pragma once

#include <cstdint>
#include "core/types.h"
#include "core/market_data.h"
#include "feed/itch_decoder.h"

namespace hft::feed {

    using namespace hft::core;

    // =========================================================================
    // Endian conversion helpers (ITCH uses network byte order / big-endian)
    // =========================================================================

    inline uint16_t read_be16(const void* p) noexcept {
        const auto* b = static_cast<const uint8_t*>(p);
        return static_cast<uint16_t>((b[0] << 8) | b[1]);
    }

    inline uint32_t read_be32(const void* p) noexcept {
        const auto* b = static_cast<const uint8_t*>(p);
        return (static_cast<uint32_t>(b[0]) << 24) |
               (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8)  |
               static_cast<uint32_t>(b[3]);
    }

    inline uint64_t read_be64(const void* p) noexcept {
        const auto* b = static_cast<const uint8_t*>(p);
        return (static_cast<uint64_t>(b[0]) << 56) |
               (static_cast<uint64_t>(b[1]) << 48) |
               (static_cast<uint64_t>(b[2]) << 40) |
               (static_cast<uint64_t>(b[3]) << 32) |
               (static_cast<uint64_t>(b[4]) << 24) |
               (static_cast<uint64_t>(b[5]) << 16) |
               (static_cast<uint64_t>(b[6]) << 8)  |
               static_cast<uint64_t>(b[7]);
    }

    // ITCH timestamps are 6 bytes big-endian nanoseconds since midnight
    inline uint64_t read_itch_timestamp(const uint8_t ts[6]) noexcept {
        return (static_cast<uint64_t>(ts[0]) << 40) |
               (static_cast<uint64_t>(ts[1]) << 32) |
               (static_cast<uint64_t>(ts[2]) << 24) |
               (static_cast<uint64_t>(ts[3]) << 16) |
               (static_cast<uint64_t>(ts[4]) << 8)  |
               static_cast<uint64_t>(ts[5]);
    }

    // =========================================================================
    // ITCH Handler implementations — inline definitions
    // =========================================================================

    inline bool ITCHHandler<'A'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHAddOrder*>(data);
        out.type = UpdateType::ADD;
        out.order_ref = read_be64(&msg->order_ref);
        out.side = (msg->side == 'B') ? Side::BUY : Side::SELL;
        out.quantity = static_cast<Quantity>(read_be32(&msg->shares));
        out.price = static_cast<Price>(read_be32(&msg->price));
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    inline bool ITCHHandler<'F'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHAddOrderMPID*>(data);
        out.type = UpdateType::ADD;
        out.order_ref = read_be64(&msg->order_ref);
        out.side = (msg->side == 'B') ? Side::BUY : Side::SELL;
        out.quantity = static_cast<Quantity>(read_be32(&msg->shares));
        out.price = static_cast<Price>(read_be32(&msg->price));
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    inline bool ITCHHandler<'E'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHOrderExecuted*>(data);
        out.type = UpdateType::EXECUTE;
        out.order_ref = read_be64(&msg->order_ref);
        out.quantity = static_cast<Quantity>(read_be32(&msg->executed_shares));
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    inline bool ITCHHandler<'C'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHOrderExecutedPrice*>(data);
        out.type = UpdateType::EXECUTE;
        out.order_ref = read_be64(&msg->order_ref);
        out.quantity = static_cast<Quantity>(read_be32(&msg->executed_shares));
        out.price = static_cast<Price>(read_be32(&msg->execution_price));
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    inline bool ITCHHandler<'X'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHOrderCancel*>(data);
        out.type = UpdateType::DELETE;
        out.order_ref = read_be64(&msg->order_ref);
        out.quantity = static_cast<Quantity>(read_be32(&msg->cancelled_shares));
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    inline bool ITCHHandler<'D'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHOrderDelete*>(data);
        out.type = UpdateType::DELETE;
        out.order_ref = read_be64(&msg->order_ref);
        out.quantity = 0;
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    inline bool ITCHHandler<'U'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHOrderReplace*>(data);
        out.type = UpdateType::MODIFY;
        out.order_ref = read_be64(&msg->orig_order_ref);
        out.quantity = static_cast<Quantity>(read_be32(&msg->shares));
        out.price = static_cast<Price>(read_be32(&msg->price));
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    inline bool ITCHHandler<'P'>::handle(const void* data, MarketUpdate& out) noexcept {
        const auto* msg = static_cast<const ITCHTrade*>(data);
        out.type = UpdateType::TRADE;
        out.order_ref = read_be64(&msg->order_ref);
        out.side = (msg->side == 'B') ? Side::BUY : Side::SELL;
        out.quantity = static_cast<Quantity>(read_be32(&msg->shares));
        out.price = static_cast<Price>(read_be32(&msg->price));
        out.instrument_id = read_be16(&msg->stock_locate);
        out.timestamp = read_itch_timestamp(msg->timestamp);
        return true;
    }

    // =========================================================================
    // Top-level ITCH dispatch — jump table, O(1) lookup
    // =========================================================================
    [[gnu::hot]]
    inline bool dispatch_itch(const uint8_t* buf, size_t len, MarketUpdate& out) noexcept {
        if (len < 1) return false;

        char msg_type = static_cast<char>(buf[0]);
        int expected_size = itch_message_size(msg_type);
        if (expected_size < 0 || static_cast<int>(len) < expected_size) return false;

        switch (msg_type) {
            case 'A': return ITCHHandler<'A'>::handle(buf, out);
            case 'F': return ITCHHandler<'F'>::handle(buf, out);
            case 'E': return ITCHHandler<'E'>::handle(buf, out);
            case 'C': return ITCHHandler<'C'>::handle(buf, out);
            case 'X': return ITCHHandler<'X'>::handle(buf, out);
            case 'D': return ITCHHandler<'D'>::handle(buf, out);
            case 'U': return ITCHHandler<'U'>::handle(buf, out);
            case 'P': return ITCHHandler<'P'>::handle(buf, out);
            default:  return false;
        }
    }

} // namespace hft::feed
