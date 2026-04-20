#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "core/types.h"
#include "core/market_data.h"

namespace hft::gateway {

    using namespace hft::core;

    // =========================================================================
    // Fast integer-to-ASCII conversion (no snprintf, no locale overhead)
    // Used for FIX tag/value encoding on the hot path.
    // =========================================================================
    inline int itoa_fast(int64_t value, char* buf) noexcept {
        if (value == 0) {
            buf[0] = '0';
            return 1;
        }

        char tmp[20];
        int len = 0;
        bool negative = false;

        if (value < 0) {
            negative = true;
            // Handle INT64_MIN safely
            uint64_t uval = static_cast<uint64_t>(-(value + 1)) + 1;
            while (uval > 0) {
                tmp[len++] = '0' + static_cast<char>(uval % 10);
                uval /= 10;
            }
        } else {
            uint64_t uval = static_cast<uint64_t>(value);
            while (uval > 0) {
                tmp[len++] = '0' + static_cast<char>(uval % 10);
                uval /= 10;
            }
        }

        int pos = 0;
        if (negative) buf[pos++] = '-';
        for (int i = len - 1; i >= 0; --i)
            buf[pos++] = tmp[i];

        return pos;
    }

    // =========================================================================
    // FixEncoder — zero-allocation FIX 4.2 message encoder
    //
    // NOT using QuickFIX (adds 20-50us of overhead).
    // Pre-computed static fields, only variable fields written per-message.
    // =========================================================================
    class FixEncoder {
        static constexpr size_t BUF_SIZE = 2048;

        char buffer_[BUF_SIZE]{};
        int  pos_{0};

        char sender_comp_id_[16]{};
        int  sender_comp_id_len_{0};
        char target_comp_id_[16]{};
        int  target_comp_id_len_{0};
        uint32_t msg_seq_num_{0};

        static constexpr char SOH = '\x01';

        void append_tag_value(int tag, const char* value, int value_len) noexcept {
            pos_ += itoa_fast(tag, buffer_ + pos_);
            buffer_[pos_++] = '=';
            std::memcpy(buffer_ + pos_, value, value_len);
            pos_ += value_len;
            buffer_[pos_++] = SOH;
        }

        void append_tag_sv(int tag, std::string_view value) noexcept {
            append_tag_value(tag, value.data(), static_cast<int>(value.size()));
        }

        void append_tag_int(int tag, int64_t value) noexcept {
            pos_ += itoa_fast(tag, buffer_ + pos_);
            buffer_[pos_++] = '=';
            pos_ += itoa_fast(value, buffer_ + pos_);
            buffer_[pos_++] = SOH;
        }

        void append_tag_char(int tag, char c) noexcept {
            pos_ += itoa_fast(tag, buffer_ + pos_);
            buffer_[pos_++] = '=';
            buffer_[pos_++] = c;
            buffer_[pos_++] = SOH;
        }

        // Compute FIX body length and patch tag 9
        void fixup_body_length() noexcept {
            const char* body_start = nullptr;
            int soh_count = 0;
            for (int i = 0; i < pos_; ++i) {
                if (buffer_[i] == SOH) {
                    ++soh_count;
                    if (soh_count == 2) {
                        body_start = buffer_ + i + 1;
                        break;
                    }
                }
            }
            if (!body_start) return;

            int body_len = static_cast<int>((buffer_ + pos_) - body_start);

            char len_str[24];
            int len_len = itoa_fast(body_len, len_str);

            for (int i = 0; i < pos_ - 2; ++i) {
                if (buffer_[i] == '9' && buffer_[i + 1] == '=') {
                    int val_start = i + 2;
                    int pad = 3 - len_len;
                    for (int j = 0; j < pad; ++j)
                        buffer_[val_start + j] = '0';
                    std::memcpy(buffer_ + val_start + pad, len_str, len_len);
                    break;
                }
            }
        }

        // Compute FIX checksum (sum of all bytes mod 256, as 3-digit string)
        void append_checksum() noexcept {
            int sum = 0;
            for (int i = 0; i < pos_; ++i)
                sum += static_cast<unsigned char>(buffer_[i]);
            int checksum = sum % 256;

            buffer_[pos_++] = '1';
            buffer_[pos_++] = '0';
            buffer_[pos_++] = '=';
            buffer_[pos_++] = '0' + static_cast<char>((checksum / 100) % 10);
            buffer_[pos_++] = '0' + static_cast<char>((checksum / 10) % 10);
            buffer_[pos_++] = '0' + static_cast<char>(checksum % 10);
            buffer_[pos_++] = SOH;
        }

    public:
        void set_sender(std::string_view sender) noexcept {
            sender_comp_id_len_ = static_cast<int>(
                std::min(sender.size(), sizeof(sender_comp_id_) - 1));
            std::memcpy(sender_comp_id_, sender.data(), sender_comp_id_len_);
        }

        void set_target(std::string_view target) noexcept {
            target_comp_id_len_ = static_cast<int>(
                std::min(target.size(), sizeof(target_comp_id_) - 1));
            std::memcpy(target_comp_id_, target.data(), target_comp_id_len_);
        }

        // Encode a NewOrderSingle (FIX MsgType=D)
        [[gnu::hot]]
        std::span<const char> encode_new_order(const NewOrderSingle& order) noexcept {
            pos_ = 0;

            static constexpr char PREFIX[] = "8=FIX.4.2\x01" "9=000\x01" "35=D\x01";
            std::memcpy(buffer_, PREFIX, sizeof(PREFIX) - 1);
            pos_ = sizeof(PREFIX) - 1;

            append_tag_value(49, sender_comp_id_, sender_comp_id_len_);
            append_tag_value(56, target_comp_id_, target_comp_id_len_);
            append_tag_int(34, ++msg_seq_num_);

            int cl_len = 0;
            while (cl_len < 20 && order.cl_ord_id[cl_len] != '\0') ++cl_len;
            append_tag_value(11, order.cl_ord_id, cl_len);

            int sym_len = 0;
            while (sym_len < 8 && order.symbol[sym_len] != '\0') ++sym_len;
            append_tag_value(55, order.symbol, sym_len);

            append_tag_char(54, (order.side == Side::BUY) ? '1' : '2');
            append_tag_int(38, order.qty);
            append_tag_int(44, order.price);

            char ord_type = '2';
            if (order.ord_type == OrderType::MARKET) ord_type = '1';
            append_tag_char(40, ord_type);

            char tif = '0';
            switch (order.tif) {
                case TimeInForce::GTC: tif = '1'; break;
                case TimeInForce::IOC: tif = '3'; break;
                case TimeInForce::FOK: tif = '4'; break;
                default: tif = '0'; break;
            }
            append_tag_char(59, tif);

            fixup_body_length();
            append_checksum();

            return {buffer_, static_cast<size_t>(pos_)};
        }

        // Encode a cancel request (FIX MsgType=F)
        [[gnu::hot]]
        std::span<const char> encode_cancel(OrderId cl_ord_id, OrderId orig_cl_ord_id,
                                             std::string_view symbol, Side side) noexcept {
            pos_ = 0;

            static constexpr char PREFIX[] = "8=FIX.4.2\x01" "9=000\x01" "35=F\x01";
            std::memcpy(buffer_, PREFIX, sizeof(PREFIX) - 1);
            pos_ = sizeof(PREFIX) - 1;

            append_tag_value(49, sender_comp_id_, sender_comp_id_len_);
            append_tag_value(56, target_comp_id_, target_comp_id_len_);
            append_tag_int(34, ++msg_seq_num_);

            char id_buf[20];
            int id_len = itoa_fast(static_cast<int64_t>(cl_ord_id), id_buf);
            append_tag_value(11, id_buf, id_len);

            int orig_len = itoa_fast(static_cast<int64_t>(orig_cl_ord_id), id_buf);
            append_tag_value(41, id_buf, orig_len);

            append_tag_sv(55, symbol);
            append_tag_char(54, (side == Side::BUY) ? '1' : '2');

            fixup_body_length();
            append_checksum();

            return {buffer_, static_cast<size_t>(pos_)};
        }

        [[nodiscard]] const char* data() const noexcept { return buffer_; }
        [[nodiscard]] int size() const noexcept { return pos_; }
        [[nodiscard]] uint32_t seq_num() const noexcept { return msg_seq_num_; }
    };

} // namespace hft::gateway
