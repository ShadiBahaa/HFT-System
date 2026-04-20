#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "core/types.h"
#include "core/market_data.h"

namespace hft::gateway::adapters {

    using namespace hft::core;

    // =========================================================================
    // NASDAQ OUCH 4.2 — binary order entry protocol
    //
    // OUCH messages are big-endian on the wire; this implementation writes
    // values byte-by-byte so it stays correct on both little- and big-endian
    // hosts without needing htobe64 / _byteswap_uint64.
    // =========================================================================

    namespace ouch {
        constexpr char MSG_ENTER_ORDER     = 'O';
        constexpr char MSG_CANCEL_ORDER    = 'X';
        constexpr char MSG_REPLACE_ORDER   = 'U';
        constexpr char MSG_ORDER_ACCEPTED  = 'A';
        constexpr char MSG_ORDER_EXECUTED  = 'E';
        constexpr char MSG_ORDER_CANCELED  = 'C';
        constexpr char MSG_ORDER_REJECTED  = 'J';

        // Message sizes (header byte + payload)
        constexpr size_t ENTER_ORDER_SIZE     = 49;
        constexpr size_t CANCEL_ORDER_SIZE    = 19;
        constexpr size_t ORDER_ACCEPTED_SIZE  = 66;
        constexpr size_t ORDER_EXECUTED_SIZE  = 40;
        constexpr size_t ORDER_CANCELED_SIZE  = 27;
    }

    struct OUCHEnterOrder {
        char        token[14]{};         // Client order reference
        char        buy_sell_indicator{'B'};
        uint32_t    shares{0};
        char        stock[8]{};
        uint32_t    price_usd_e4{0};     // 0 = market
        uint32_t    time_in_force{0};
        char        firm[4]{};
        char        display{'Y'};
        char        capacity{'P'};       // Principal
        char        iso_eligible{'N'};
        char        min_qty_e4{0};       // Reserved byte
        uint32_t    min_qty{0};
        char        cross_type{'N'};
        char        customer_type{'R'};
    };

    struct OUCHCancelOrder {
        char        token[14]{};
        uint32_t    shares{0};
    };

    struct OUCHOrderAccepted {
        uint64_t    timestamp_ns{0};
        char        token[14]{};
        char        buy_sell_indicator{'B'};
        uint32_t    shares{0};
        char        stock[8]{};
        uint32_t    price_usd_e4{0};
        uint32_t    time_in_force{0};
        char        firm[4]{};
        char        display{'Y'};
        uint64_t    order_reference{0};
        char        capacity{'P'};
        char        iso_eligible{'N'};
        uint32_t    min_qty{0};
        char        cross_type{'N'};
        char        order_state{'L'};
    };

    struct OUCHOrderExecuted {
        uint64_t    timestamp_ns{0};
        char        token[14]{};
        uint32_t    executed_shares{0};
        uint32_t    execution_price_e4{0};
        char        liquidity_flag{'A'};
        uint64_t    match_number{0};
    };

    // -------------------------------------------------------------------------
    // Byte-order helpers (wire is big-endian)
    // -------------------------------------------------------------------------
    namespace detail {
        inline void put_u16_be(uint8_t* p, uint16_t v) noexcept {
            p[0] = static_cast<uint8_t>(v >> 8);
            p[1] = static_cast<uint8_t>(v);
        }
        inline void put_u32_be(uint8_t* p, uint32_t v) noexcept {
            p[0] = static_cast<uint8_t>(v >> 24);
            p[1] = static_cast<uint8_t>(v >> 16);
            p[2] = static_cast<uint8_t>(v >> 8);
            p[3] = static_cast<uint8_t>(v);
        }
        inline void put_u64_be(uint8_t* p, uint64_t v) noexcept {
            for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> ((7 - i) * 8));
        }
        inline uint16_t get_u16_be(const uint8_t* p) noexcept {
            return (static_cast<uint16_t>(p[0]) << 8) | p[1];
        }
        inline uint32_t get_u32_be(const uint8_t* p) noexcept {
            return (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8)  |
                    static_cast<uint32_t>(p[3]);
        }
        inline uint64_t get_u64_be(const uint8_t* p) noexcept {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
            return v;
        }
    }

    // -------------------------------------------------------------------------
    // Encoder
    // -------------------------------------------------------------------------
    class OUCHEncoder {
    public:
        // Returns bytes written, or 0 on buffer overflow.
        static size_t encode_enter_order(const OUCHEnterOrder& o, uint8_t* buf, size_t buflen) noexcept {
            if (buflen < ouch::ENTER_ORDER_SIZE) return 0;
            uint8_t* p = buf;
            *p++ = static_cast<uint8_t>(ouch::MSG_ENTER_ORDER);
            std::memcpy(p, o.token, 14); p += 14;
            *p++ = static_cast<uint8_t>(o.buy_sell_indicator);
            detail::put_u32_be(p, o.shares); p += 4;
            std::memcpy(p, o.stock, 8); p += 8;
            detail::put_u32_be(p, o.price_usd_e4); p += 4;
            detail::put_u32_be(p, o.time_in_force); p += 4;
            std::memcpy(p, o.firm, 4); p += 4;
            *p++ = static_cast<uint8_t>(o.display);
            *p++ = static_cast<uint8_t>(o.capacity);
            *p++ = static_cast<uint8_t>(o.iso_eligible);
            detail::put_u32_be(p, o.min_qty); p += 4;
            *p++ = static_cast<uint8_t>(o.cross_type);
            *p++ = static_cast<uint8_t>(o.customer_type);
            return static_cast<size_t>(p - buf);
        }

        static size_t encode_cancel(const OUCHCancelOrder& c, uint8_t* buf, size_t buflen) noexcept {
            if (buflen < ouch::CANCEL_ORDER_SIZE) return 0;
            uint8_t* p = buf;
            *p++ = static_cast<uint8_t>(ouch::MSG_CANCEL_ORDER);
            std::memcpy(p, c.token, 14); p += 14;
            detail::put_u32_be(p, c.shares); p += 4;
            return static_cast<size_t>(p - buf);
        }
    };

    // -------------------------------------------------------------------------
    // Decoder
    // -------------------------------------------------------------------------
    class OUCHDecoder {
    public:
        static bool decode_accepted(const uint8_t* buf, size_t len, OUCHOrderAccepted& out) noexcept {
            if (len < ouch::ORDER_ACCEPTED_SIZE) return false;
            if (buf[0] != static_cast<uint8_t>(ouch::MSG_ORDER_ACCEPTED)) return false;
            const uint8_t* p = buf + 1;
            out.timestamp_ns = detail::get_u64_be(p); p += 8;
            std::memcpy(out.token, p, 14); p += 14;
            out.buy_sell_indicator = static_cast<char>(*p++);
            out.shares = detail::get_u32_be(p); p += 4;
            std::memcpy(out.stock, p, 8); p += 8;
            out.price_usd_e4 = detail::get_u32_be(p); p += 4;
            out.time_in_force = detail::get_u32_be(p); p += 4;
            std::memcpy(out.firm, p, 4); p += 4;
            out.display = static_cast<char>(*p++);
            out.order_reference = detail::get_u64_be(p); p += 8;
            out.capacity = static_cast<char>(*p++);
            out.iso_eligible = static_cast<char>(*p++);
            out.min_qty = detail::get_u32_be(p); p += 4;
            out.cross_type = static_cast<char>(*p++);
            out.order_state = static_cast<char>(*p++);
            return true;
        }

        static bool decode_executed(const uint8_t* buf, size_t len, OUCHOrderExecuted& out) noexcept {
            if (len < ouch::ORDER_EXECUTED_SIZE) return false;
            if (buf[0] != static_cast<uint8_t>(ouch::MSG_ORDER_EXECUTED)) return false;
            const uint8_t* p = buf + 1;
            out.timestamp_ns = detail::get_u64_be(p); p += 8;
            std::memcpy(out.token, p, 14); p += 14;
            out.executed_shares = detail::get_u32_be(p); p += 4;
            out.execution_price_e4 = detail::get_u32_be(p); p += 4;
            out.liquidity_flag = static_cast<char>(*p++);
            out.match_number = detail::get_u64_be(p); p += 8;
            return true;
        }

        // Peek message type without full decode
        static char msg_type(const uint8_t* buf, size_t len) noexcept {
            return (len == 0) ? 0 : static_cast<char>(buf[0]);
        }
    };

} // namespace hft::gateway::adapters
