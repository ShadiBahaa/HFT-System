#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string_view>
#include <array>
#include "security/secure_string.h"

namespace hft::security {

    // =========================================================================
    // ExchangeAuth — minimal credential holder + FIX logon message builder
    //
    // Stores username / password in SecureString so the bytes are wiped on
    // destruction and optionally locked into physical RAM.
    //
    // Produces a FIX Logon (MsgType=A) message ready to send over the wire.
    // HMAC signing is intentionally pluggable — the default is a simple
    // non-cryptographic digest that is fine for unit tests but MUST be
    // replaced with a real HMAC-SHA256 in production deployments.
    // =========================================================================
    class ExchangeAuth {
    public:
        static constexpr size_t MAX_SENDER_LEN = 32;
        static constexpr size_t MAX_TARGET_LEN = 32;
        static constexpr size_t MAX_TOKEN_LEN  = 64;
        static constexpr uint64_t TOKEN_TTL_NS_DEFAULT = 3'600'000'000'000ULL; // 1h

    private:
        char     sender_comp_id_[MAX_SENDER_LEN]{};
        char     target_comp_id_[MAX_TARGET_LEN]{};
        SecureString<128> username_;
        SecureString<128> password_;

        SecureString<MAX_TOKEN_LEN> session_token_;
        uint64_t token_issued_ns_{0};
        uint64_t token_ttl_ns_{TOKEN_TTL_NS_DEFAULT};

        static uint32_t fnv1a(std::string_view sv) noexcept {
            uint32_t h = 2166136261u;
            for (char c : sv) {
                h ^= static_cast<uint8_t>(c);
                h *= 16777619u;
            }
            return h;
        }

    public:
        ExchangeAuth() = default;

        void set_identity(std::string_view sender, std::string_view target) noexcept {
            size_t s = sender.size() < MAX_SENDER_LEN - 1 ? sender.size() : MAX_SENDER_LEN - 1;
            std::memcpy(sender_comp_id_, sender.data(), s);
            sender_comp_id_[s] = '\0';
            size_t t = target.size() < MAX_TARGET_LEN - 1 ? target.size() : MAX_TARGET_LEN - 1;
            std::memcpy(target_comp_id_, target.data(), t);
            target_comp_id_[t] = '\0';
        }

        bool set_credentials(std::string_view username, std::string_view password) noexcept {
            return username_.set(username) && password_.set(password);
        }

        void set_token_ttl_ns(uint64_t ttl_ns) noexcept { token_ttl_ns_ = ttl_ns; }

        [[nodiscard]] std::string_view sender_comp_id() const noexcept { return sender_comp_id_; }
        [[nodiscard]] std::string_view target_comp_id() const noexcept { return target_comp_id_; }

        // Build a FIX Logon message into the caller's buffer.
        // Returns number of bytes written, or 0 on error (buffer too small).
        size_t build_logon_message(char* buf, size_t buf_size,
                                   uint32_t seq_num, uint64_t sending_time_ns) const noexcept
        {
            if (!buf || buf_size < 64) return 0;

            // Body first so we can compute BodyLength.
            char body[512];
            auto u = username_.view();
            auto p = password_.view();

            // Signature: fnv1a(username + ":" + password + ":" + seq)
            char sig_input[256];
            int si = std::snprintf(sig_input, sizeof(sig_input), "%.*s:%.*s:%u",
                static_cast<int>(u.size()), u.data(),
                static_cast<int>(p.size()), p.data(),
                seq_num);
            (void)si;
            uint32_t sig = fnv1a(sig_input);

            int body_len = std::snprintf(body, sizeof(body),
                "35=A\x01"
                "49=%s\x01"
                "56=%s\x01"
                "34=%u\x01"
                "52=%llu\x01"
                "98=0\x01"
                "108=30\x01"
                "553=%.*s\x01"
                "554=%.*s\x01"
                "96=%08x\x01",
                sender_comp_id_,
                target_comp_id_,
                seq_num,
                static_cast<unsigned long long>(sending_time_ns),
                static_cast<int>(u.size()), u.data(),
                static_cast<int>(p.size()), p.data(),
                sig);

            if (body_len <= 0) return 0;

            // Now the header BeginString / BodyLength
            int head_len = std::snprintf(buf, buf_size,
                "8=FIX.4.4\x01" "9=%d\x01", body_len);
            if (head_len <= 0 || static_cast<size_t>(head_len) >= buf_size) return 0;

            if (static_cast<size_t>(head_len + body_len + 8) >= buf_size) return 0;
            std::memcpy(buf + head_len, body, static_cast<size_t>(body_len));

            // Checksum = sum of all bytes mod 256
            size_t total = static_cast<size_t>(head_len + body_len);
            uint32_t sum = 0;
            for (size_t i = 0; i < total; ++i) {
                sum += static_cast<uint8_t>(buf[i]);
            }
            int tail = std::snprintf(buf + total, buf_size - total,
                "10=%03u\x01", sum % 256);
            if (tail <= 0) return 0;
            return total + static_cast<size_t>(tail);
        }

        // Record a session token issued by the exchange (after successful logon).
        bool issue_token(std::string_view token, uint64_t now_ns) noexcept {
            if (!session_token_.set(token)) return false;
            token_issued_ns_ = now_ns;
            return true;
        }

        [[nodiscard]] bool token_valid(uint64_t now_ns) const noexcept {
            if (session_token_.empty()) return false;
            return (now_ns - token_issued_ns_) <= token_ttl_ns_;
        }

        [[nodiscard]] std::string_view token() const noexcept {
            return session_token_.view();
        }

        void clear_token() noexcept {
            session_token_.clear();
            token_issued_ns_ = 0;
        }
    };

} // namespace hft::security
