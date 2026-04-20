#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "core/types.h"

namespace hft::gateway::adapters {

    using namespace hft::core;

    // =========================================================================
    // CBOE BOE 2.0 — Binary Order Entry (little-endian on the wire)
    //
    // BOE has a session-layer header and a message-body header. We model both
    // and provide a minimal NewOrder / OrderAcknowledgement pair sufficient
    // for session smoke testing and round-trip integration.
    // =========================================================================

    namespace boe {
        constexpr uint8_t  START_OF_MSG_0 = 0xBA;
        constexpr uint8_t  START_OF_MSG_1 = 0xBA;

        constexpr uint8_t  MSG_NEW_ORDER           = 0x38;
        constexpr uint8_t  MSG_CANCEL_ORDER        = 0x39;
        constexpr uint8_t  MSG_ORDER_ACKNOWLEDGE   = 0x25;
        constexpr uint8_t  MSG_ORDER_REJECTED      = 0x26;
        constexpr uint8_t  MSG_ORDER_EXECUTION     = 0x2C;
        constexpr uint8_t  MSG_LOGIN               = 0x01;
        constexpr uint8_t  MSG_LOGIN_RESPONSE      = 0x24;
    }

    // BOE session-layer header — 6 bytes, followed by message type + body
    struct BOEHeader {
        uint8_t     start_of_message[2]{boe::START_OF_MSG_0, boe::START_OF_MSG_1};
        uint16_t    msg_length{0};   // Length including this header
        uint8_t     msg_type{0};
        uint8_t     matching_unit{0};
        uint32_t    sequence{0};
    };

    struct BOENewOrder {
        char        cl_ord_id[20]{};      // Space-padded ASCII
        char        side{'1'};             // '1'=buy, '2'=sell
        uint32_t    order_qty{0};
        char        symbol[8]{};
        uint8_t     ord_type{2};           // 2=limit
        int64_t     price_e4{0};
        char        time_in_force{'0'};    // '0'=Day
        uint8_t     capacity{'P'};
        char        account[16]{};
    };

    struct BOEOrderAcknowledgement {
        uint64_t    timestamp_ns{0};
        char        cl_ord_id[20]{};
        uint64_t    order_id{0};
        int64_t     price_e4{0};
        uint32_t    leaves_qty{0};
    };

    namespace detail_le {
        inline void put_u16(uint8_t* p, uint16_t v) noexcept {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
        }
        inline void put_u32(uint8_t* p, uint32_t v) noexcept {
            for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
        }
        inline void put_u64(uint8_t* p, uint64_t v) noexcept {
            for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
        }
        inline void put_i64(uint8_t* p, int64_t v) noexcept {
            put_u64(p, static_cast<uint64_t>(v));
        }
        inline uint16_t get_u16(const uint8_t* p) noexcept {
            return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        }
        inline uint32_t get_u32(const uint8_t* p) noexcept {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (i * 8);
            return v;
        }
        inline uint64_t get_u64(const uint8_t* p) noexcept {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
            return v;
        }
        inline int64_t get_i64(const uint8_t* p) noexcept {
            return static_cast<int64_t>(get_u64(p));
        }
    }

    class BOEEncoder {
    public:
        // Header (10) + body (60) = 70 bytes
        static constexpr size_t NEW_ORDER_TOTAL = 10 + 20 + 1 + 4 + 8 + 1 + 8 + 1 + 1 + 16;

        static size_t encode_new_order(const BOENewOrder& o, uint32_t seq, uint8_t matching_unit,
                                       uint8_t* buf, size_t buflen) noexcept {
            if (buflen < NEW_ORDER_TOTAL) return 0;
            uint8_t* p = buf;
            // Header
            *p++ = boe::START_OF_MSG_0;
            *p++ = boe::START_OF_MSG_1;
            detail_le::put_u16(p, static_cast<uint16_t>(NEW_ORDER_TOTAL)); p += 2;
            *p++ = boe::MSG_NEW_ORDER;
            *p++ = matching_unit;
            detail_le::put_u32(p, seq); p += 4;
            // Body
            std::memcpy(p, o.cl_ord_id, 20); p += 20;
            *p++ = static_cast<uint8_t>(o.side);
            detail_le::put_u32(p, o.order_qty); p += 4;
            std::memcpy(p, o.symbol, 8); p += 8;
            *p++ = o.ord_type;
            detail_le::put_i64(p, o.price_e4); p += 8;
            *p++ = static_cast<uint8_t>(o.time_in_force);
            *p++ = o.capacity;
            std::memcpy(p, o.account, 16); p += 16;
            return static_cast<size_t>(p - buf);
        }
    };

    class BOEDecoder {
    public:
        // Parse the session header; returns false if start-of-message is wrong
        // or the buffer is too small.
        static bool decode_header(const uint8_t* buf, size_t len, BOEHeader& out) noexcept {
            if (len < 10) return false;
            if (buf[0] != boe::START_OF_MSG_0 || buf[1] != boe::START_OF_MSG_1) return false;
            out.start_of_message[0] = buf[0];
            out.start_of_message[1] = buf[1];
            out.msg_length    = detail_le::get_u16(buf + 2);
            out.msg_type      = buf[4];
            out.matching_unit = buf[5];
            out.sequence      = detail_le::get_u32(buf + 6);
            return len >= out.msg_length;
        }

        // Parse ORDER_ACKNOWLEDGEMENT body (following header). Caller is
        // responsible for having verified the header msg_type.
        static bool decode_ack(const uint8_t* buf, size_t len, BOEOrderAcknowledgement& out) noexcept {
            // Header is 10 bytes; ack body = 8 + 20 + 8 + 8 + 4 = 48
            constexpr size_t BODY = 48;
            if (len < 10 + BODY) return false;
            const uint8_t* p = buf + 10;
            out.timestamp_ns = detail_le::get_u64(p); p += 8;
            std::memcpy(out.cl_ord_id, p, 20); p += 20;
            out.order_id = detail_le::get_u64(p); p += 8;
            out.price_e4 = detail_le::get_i64(p); p += 8;
            out.leaves_qty = detail_le::get_u32(p); p += 4;
            return true;
        }
    };

} // namespace hft::gateway::adapters
