#pragma once

#include <cstdint>
#include <cstring>
#include <span>

#include "core/types.h"
#include "core/market_data.h"

namespace hft::gateway {

    using namespace hft::core;

    // =========================================================================
    // SBE (Simple Binary Encoding) — fixed-layout, zero parsing overhead
    // Used for CME iLink 3 and similar binary protocols.
    // Encoding: just fill the struct and send — ~50ns encode time.
    // =========================================================================

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

    struct
#ifndef _MSC_VER
    __attribute__((packed))
#endif
    SBEMessageHeader {
        uint16_t block_length;
        uint16_t template_id;
        uint16_t schema_id;
        uint16_t version;
    };

    struct
#ifndef _MSC_VER
    __attribute__((packed))
#endif
    SBENewOrderSingle {
        SBEMessageHeader header;
        int64_t  price;                // PRICENULL9 mantissa
        int32_t  order_qty;
        int32_t  security_id;
        uint8_t  side;                 // 1=Buy, 2=Sell
        uint64_t order_request_id;
        uint64_t sending_time_epoch;
        char     cl_ord_id[20];
    };

#ifdef _MSC_VER
#pragma pack(pop)
#endif

    class SBEEncoder {
        alignas(64) char buffer_[256]{};
        int pos_{0};

    public:
        // Encode a NewOrderSingle in SBE format (~50ns)
        [[gnu::hot]]
        std::span<const char> encode_new_order(
            const NewOrderSingle& order, int32_t security_id,
            uint64_t request_id) noexcept
        {
            auto* msg = reinterpret_cast<SBENewOrderSingle*>(buffer_);

            msg->header.block_length = sizeof(SBENewOrderSingle) - sizeof(SBEMessageHeader);
            msg->header.template_id = 514;  // NewOrderSingle
            msg->header.schema_id = 1;
            msg->header.version = 1;

            msg->price = order.price;
            msg->order_qty = order.qty;
            msg->security_id = security_id;
            msg->side = (order.side == Side::BUY) ? 1 : 2;
            msg->order_request_id = request_id;
            msg->sending_time_epoch = order.timestamp;
            std::memcpy(msg->cl_ord_id, order.cl_ord_id, 20);

            pos_ = sizeof(SBENewOrderSingle);
            return {buffer_, static_cast<size_t>(pos_)};
        }

        [[nodiscard]] const char* data() const noexcept { return buffer_; }
        [[nodiscard]] int size() const noexcept { return pos_; }
    };

} // namespace hft::gateway
