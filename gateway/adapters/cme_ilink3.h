#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "core/types.h"
#include "gateway/sbe_encoder.h"

namespace hft::gateway::adapters {

    using namespace hft::core;

    // =========================================================================
    // CME iLink 3 (SBE 1.0) — session layer on top of the existing SBE encoder
    //
    // iLink3 wraps SBE messages with:
    //   - SOFH   (Simple Open Framing Header): 4 bytes message_length + 2 bytes
    //            encoding_type (0xEB50)
    //   - Session header: uuid, seq_num, sending_time
    //
    // This module provides the session layer; strategy-specific SBE payloads
    // (NewOrder, ExecutionReport, etc.) are built with hft::gateway::SbeEncoder.
    // =========================================================================

    namespace ilink3 {
        constexpr uint16_t SBE_ENCODING_TYPE = 0xEB50;

        // Template IDs from CME iLink3 spec (subset)
        constexpr uint16_t TMPL_NEGOTIATE          = 500;
        constexpr uint16_t TMPL_NEGOTIATE_RESPONSE = 501;
        constexpr uint16_t TMPL_ESTABLISH          = 503;
        constexpr uint16_t TMPL_ESTABLISH_ACK      = 504;
        constexpr uint16_t TMPL_TERMINATE          = 507;
        constexpr uint16_t TMPL_SEQUENCE           = 506;
        constexpr uint16_t TMPL_NEW_ORDER_SINGLE   = 514;
        constexpr uint16_t TMPL_EXECUTION_REPORT   = 525;
    }

    // Simple Open Framing Header (FIX SOFH)
    struct SOFH {
        uint32_t    message_length{0};   // Includes SOFH itself
        uint16_t    encoding_type{ilink3::SBE_ENCODING_TYPE};
    };

    // iLink3 session envelope — what wraps each business message
    struct ILink3SessionHeader {
        uint64_t    uuid{0};              // Session identifier
        uint32_t    seq_num{0};            // Per-session monotonic
        uint64_t    sending_time_ns{0};
    };

    // Negotiate — initial session handshake
    struct ILink3Negotiate {
        uint64_t    uuid{0};
        uint64_t    request_timestamp_ns{0};
        uint16_t    firm_id{0};
        char        credentials[24]{};
    };

    // Establish — after Negotiate, enables business flow
    struct ILink3Establish {
        uint64_t    uuid{0};
        uint64_t    request_timestamp_ns{0};
        uint32_t    next_seq_num{1};
        uint16_t    keep_alive_interval_ms{0};
    };

    // Terminate — orderly session close
    struct ILink3Terminate {
        uint64_t    uuid{0};
        uint64_t    request_timestamp_ns{0};
        uint16_t    error_code{0};
    };

    namespace ilink3_le {
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
    }

    // -------------------------------------------------------------------------
    // ILink3Session — session-layer codec
    //
    // Wire layout per frame:
    //   [SOFH:6][SBE msg hdr:8][session hdr:20][business body:N]
    // -------------------------------------------------------------------------
    class ILink3Session {
    public:
        ILink3Session() = default;

        void set_uuid(uint64_t u) noexcept { uuid_ = u; }
        [[nodiscard]] uint64_t uuid() const noexcept { return uuid_; }

        [[nodiscard]] uint32_t next_out_seq() const noexcept { return next_out_seq_; }
        void set_next_out_seq(uint32_t s) noexcept { next_out_seq_ = s; }

        [[nodiscard]] uint32_t expected_in_seq() const noexcept { return expected_in_seq_; }
        void set_expected_in_seq(uint32_t s) noexcept { expected_in_seq_ = s; }

        // Frame a business SBE message with SOFH + session header.
        // `body` points to an already-encoded SBE message (built by SbeEncoder).
        // Returns total bytes written, or 0 on overflow.
        size_t frame(uint16_t template_id, uint16_t schema_id, uint16_t version,
                     const uint8_t* body, uint16_t body_len,
                     uint64_t sending_time_ns,
                     uint8_t* buf, size_t buflen) noexcept {
            constexpr size_t SOFH_SIZE      = 6;
            constexpr size_t SBE_HDR_SIZE   = 8;
            constexpr size_t SESSION_HDR_SZ = 20;
            size_t total = SOFH_SIZE + SBE_HDR_SIZE + SESSION_HDR_SZ + body_len;
            if (buflen < total) return 0;

            uint8_t* p = buf;
            // SOFH
            ilink3_le::put_u32(p, static_cast<uint32_t>(total)); p += 4;
            ilink3_le::put_u16(p, ilink3::SBE_ENCODING_TYPE); p += 2;
            // SBE message header
            ilink3_le::put_u16(p, static_cast<uint16_t>(body_len + SESSION_HDR_SZ)); p += 2; // block_length
            ilink3_le::put_u16(p, template_id); p += 2;
            ilink3_le::put_u16(p, schema_id);   p += 2;
            ilink3_le::put_u16(p, version);     p += 2;
            // Session header
            ilink3_le::put_u64(p, uuid_); p += 8;
            ilink3_le::put_u32(p, next_out_seq_++); p += 4;
            ilink3_le::put_u64(p, sending_time_ns); p += 8;
            // Body
            if (body_len > 0) std::memcpy(p, body, body_len);
            return total;
        }

        // Parse SOFH + SBE header + session header; returns payload pointer/len
        bool unframe(const uint8_t* buf, size_t len,
                     uint16_t& template_id, uint16_t& schema_id, uint16_t& version,
                     ILink3SessionHeader& sess,
                     const uint8_t*& body, size_t& body_len) noexcept {
            if (len < 34) return false;
            uint32_t total = ilink3_le::get_u32(buf);
            uint16_t enc   = ilink3_le::get_u16(buf + 4);
            if (enc != ilink3::SBE_ENCODING_TYPE) return false;
            if (len < total) return false;

            // uint16_t block_len = ilink3_le::get_u16(buf + 6);  // not used at this layer
            template_id = ilink3_le::get_u16(buf + 8);
            schema_id   = ilink3_le::get_u16(buf + 10);
            version     = ilink3_le::get_u16(buf + 12);

            sess.uuid            = ilink3_le::get_u64(buf + 14);
            sess.seq_num         = ilink3_le::get_u32(buf + 22);
            sess.sending_time_ns = ilink3_le::get_u64(buf + 26);

            body = buf + 34;
            body_len = total - 34;

            // Update expected inbound sequence
            if (sess.seq_num == expected_in_seq_) ++expected_in_seq_;
            return true;
        }

        // Build a Negotiate message (session-layer, not business flow)
        size_t build_negotiate(const ILink3Negotiate& n, uint8_t* buf, size_t buflen) noexcept {
            // Body: 8 + 8 + 2 + 24 = 42 bytes
            constexpr size_t BODY = 42;
            uint8_t body[BODY];
            uint8_t* p = body;
            ilink3_le::put_u64(p, n.uuid); p += 8;
            ilink3_le::put_u64(p, n.request_timestamp_ns); p += 8;
            ilink3_le::put_u16(p, n.firm_id); p += 2;
            std::memcpy(p, n.credentials, 24);
            return frame(ilink3::TMPL_NEGOTIATE, 7, 9, body, BODY, n.request_timestamp_ns, buf, buflen);
        }

        size_t build_establish(const ILink3Establish& e, uint8_t* buf, size_t buflen) noexcept {
            constexpr size_t BODY = 8 + 8 + 4 + 2;
            uint8_t body[BODY];
            uint8_t* p = body;
            ilink3_le::put_u64(p, e.uuid); p += 8;
            ilink3_le::put_u64(p, e.request_timestamp_ns); p += 8;
            ilink3_le::put_u32(p, e.next_seq_num); p += 4;
            ilink3_le::put_u16(p, e.keep_alive_interval_ms);
            return frame(ilink3::TMPL_ESTABLISH, 7, 9, body, BODY, e.request_timestamp_ns, buf, buflen);
        }

        size_t build_terminate(const ILink3Terminate& t, uint8_t* buf, size_t buflen) noexcept {
            constexpr size_t BODY = 8 + 8 + 2;
            uint8_t body[BODY];
            uint8_t* p = body;
            ilink3_le::put_u64(p, t.uuid); p += 8;
            ilink3_le::put_u64(p, t.request_timestamp_ns); p += 8;
            ilink3_le::put_u16(p, t.error_code);
            return frame(ilink3::TMPL_TERMINATE, 7, 9, body, BODY, t.request_timestamp_ns, buf, buflen);
        }

    private:
        uint64_t    uuid_{0};
        uint32_t    next_out_seq_{1};
        uint32_t    expected_in_seq_{1};
    };

} // namespace hft::gateway::adapters
