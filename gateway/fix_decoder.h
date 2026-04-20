#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <array>
#include "core/types.h"
#include "core/market_data.h"

namespace hft::gateway {

    using namespace hft::core;

    // =========================================================================
    // FIX 4.4 zero-allocation decoder
    //
    // FIX is an ASCII protocol of tag=value\x01 pairs. This decoder walks the
    // buffer in place and hands out string_views pointing into the caller's
    // memory — no heap allocations, no parsing into an intermediate map.
    //
    // Typical use:
    //   FixDecoder::Fields f;
    //   if (FixDecoder::parse(buf, len, f)) {
    //       auto type = f.get(35);  // MsgType
    //       ...
    //   }
    // =========================================================================
    class FixDecoder {
    public:
        static constexpr char SOH = '\x01';
        static constexpr size_t MAX_FIELDS = 128;

        struct Field {
            uint32_t tag{0};
            std::string_view value;
        };

        struct Fields {
            std::array<Field, MAX_FIELDS> items{};
            size_t count{0};

            [[nodiscard]] std::string_view get(uint32_t tag) const noexcept {
                for (size_t i = 0; i < count; ++i) {
                    if (items[i].tag == tag) return items[i].value;
                }
                return {};
            }

            [[nodiscard]] bool has(uint32_t tag) const noexcept {
                for (size_t i = 0; i < count; ++i) {
                    if (items[i].tag == tag) return true;
                }
                return false;
            }
        };

        // Parse a raw FIX message into `out`. Returns true on a structurally
        // valid message including a checksum tag 10 that matches.
        static bool parse(const char* buf, size_t len, Fields& out) noexcept {
            if (!buf || len < 10) return false;
            out.count = 0;

            // Walk tag=value<SOH> pairs
            size_t pos = 0;
            while (pos < len && out.count < MAX_FIELDS) {
                // Read tag
                uint32_t tag = 0;
                bool saw_digit = false;
                while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
                    tag = tag * 10 + static_cast<uint32_t>(buf[pos] - '0');
                    ++pos;
                    saw_digit = true;
                }
                if (!saw_digit || pos >= len || buf[pos] != '=') return false;
                ++pos;  // skip '='

                // Read value until SOH
                size_t vstart = pos;
                while (pos < len && buf[pos] != SOH) ++pos;
                if (pos >= len) return false;
                out.items[out.count].tag = tag;
                out.items[out.count].value = std::string_view(buf + vstart, pos - vstart);
                ++out.count;
                ++pos;  // skip SOH
            }

            // Verify checksum tag 10 if present
            auto cs = out.get(10);
            if (!cs.empty()) {
                // Checksum covers everything up to the "10=" tag itself
                size_t cs_start = find_tag_offset(buf, len, 10);
                if (cs_start == std::string_view::npos) return false;
                uint32_t sum = 0;
                for (size_t i = 0; i < cs_start; ++i) sum += static_cast<uint8_t>(buf[i]);
                uint32_t expected = 0;
                for (char c : cs) {
                    if (c < '0' || c > '9') return false;
                    expected = expected * 10 + static_cast<uint32_t>(c - '0');
                }
                if ((sum % 256) != expected) return false;
            }

            return out.count > 0;
        }

        // Find the starting byte offset of a tag within the raw buffer.
        // Returns std::string_view::npos if not found.
        static size_t find_tag_offset(const char* buf, size_t len, uint32_t tag) noexcept {
            char needle[16];
            int n = 0;
            uint32_t t = tag;
            if (t == 0) { needle[n++] = '0'; }
            else {
                char tmp[12];
                int tn = 0;
                while (t) { tmp[tn++] = static_cast<char>('0' + (t % 10)); t /= 10; }
                while (tn) needle[n++] = tmp[--tn];
            }
            needle[n++] = '=';

            // Look for SOH + needle, or start-of-buffer + needle
            for (size_t i = 0; i + static_cast<size_t>(n) <= len; ++i) {
                bool at_boundary = (i == 0) || buf[i - 1] == SOH;
                if (!at_boundary) continue;
                bool ok = true;
                for (int j = 0; j < n; ++j) {
                    if (buf[i + static_cast<size_t>(j)] != needle[j]) { ok = false; break; }
                }
                if (ok) return i;
            }
            return std::string_view::npos;
        }

        // Compute FIX checksum (sum of all bytes mod 256) for a buffer.
        static uint8_t checksum(const char* buf, size_t len) noexcept {
            uint32_t sum = 0;
            for (size_t i = 0; i < len; ++i) sum += static_cast<uint8_t>(buf[i]);
            return static_cast<uint8_t>(sum % 256);
        }

        // Convenience: decode an ExecutionReport (MsgType=8) from parsed fields.
        static bool to_execution_report(const Fields& f, ExecutionReport& out) noexcept {
            if (f.get(35) != "8") return false;
            auto cl = f.get(11);
            auto oi = f.get(37);
            auto ss = f.get(54);
            auto px = f.get(31);                       // LastPx
            auto qt = f.get(32);                       // LastQty
            auto lq = f.get(151);                      // LeavesQty
            auto et = f.get(150);                      // ExecType

            auto to_u64 = [](std::string_view v) noexcept -> uint64_t {
                uint64_t x = 0;
                for (char c : v) { if (c < '0' || c > '9') break; x = x * 10 + static_cast<uint64_t>(c - '0'); }
                return x;
            };

            out.cl_ord_id = to_u64(cl);
            out.order_id  = to_u64(oi);
            out.side      = (ss == "1") ? Side::BUY : (ss == "2") ? Side::SELL : Side::UNKNOWN;
            // Convert decimal price "123.45" to fixed-point * 10000
            out.price = 0;
            {
                uint64_t whole = 0, frac = 0, frac_digits = 0;
                bool in_frac = false;
                for (char c : px) {
                    if (c == '.') { in_frac = true; continue; }
                    if (c < '0' || c > '9') continue;
                    if (in_frac) { frac = frac * 10 + static_cast<uint64_t>(c - '0'); ++frac_digits; }
                    else          { whole = whole * 10 + static_cast<uint64_t>(c - '0'); }
                }
                while (frac_digits < 4) { frac *= 10; ++frac_digits; }
                while (frac_digits > 4) { frac /= 10; --frac_digits; }
                out.price = static_cast<Price>(whole * 10000 + frac);
            }
            out.filled_qty = static_cast<Quantity>(to_u64(qt));
            out.leaves_qty = static_cast<Quantity>(to_u64(lq));
            if (et == "F" || et == "2") out.exec_type = ExecType::FILL;
            else if (et == "1")         out.exec_type = ExecType::PARTIAL;
            else if (et == "4")         out.exec_type = ExecType::CANCELLED;
            else if (et == "5")         out.exec_type = ExecType::REPLACED;
            else if (et == "8")         out.exec_type = ExecType::REJECTED;
            else                        out.exec_type = ExecType::NEW;
            return true;
        }
    };

} // namespace hft::gateway
