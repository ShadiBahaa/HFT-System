#pragma once

#include <cstdint>
#include <cstring>
#include "core/types.h"
#include "core/market_data.h"

namespace hft::feed {

    using namespace hft::core;

    // =========================================================================
    // CME MDP 3.0 / SBE (Simple Binary Encoding) Wire Protocol
    // https://www.cmegroup.com/confluence/display/EPICSANDBOX/MDP+3.0
    // =========================================================================

#pragma pack(push, 1)

    // Packet Header (12 bytes) — one per UDP datagram
    struct MDP3PacketHeader {
        uint32_t msg_seq_num;              // Sequence of first message in packet
        uint64_t sending_time;             // ns since epoch
    };

    // Message Header (4 bytes) — one per logical message within a packet
    struct MDP3MessageHeader {
        uint16_t msg_size;                 // Total message size including this header
        uint16_t block_length;             // Length of root block
    };

    // SBE Message Header (8 bytes)
    struct SBEMessageHeader {
        uint16_t block_length;
        uint16_t template_id;              // 32 = MDIncRefresh, 38 = SnapshotFullRefresh, ...
        uint16_t schema_id;
        uint16_t version;
    };

    // MDIncrementalRefreshBook (template id 46 in current schema)
    // Simplified incremental update entry
    struct MDP3IncRefreshEntry {
        int64_t  md_entry_px;              // Price scaled by 1e9
        uint32_t md_entry_size;
        uint32_t security_id;
        uint32_t rpt_seq;
        uint32_t number_of_orders;
        uint8_t  md_price_level;
        uint8_t  md_update_action;         // 0 = New, 1 = Change, 2 = Delete
        uint8_t  md_entry_type;            // 0 = Bid, 1 = Ask, 2 = Trade
    };

#pragma pack(pop)

    enum MDP3UpdateAction : uint8_t {
        MDP3_NEW     = 0,
        MDP3_CHANGE  = 1,
        MDP3_DELETE  = 2,
        MDP3_OVERLAY = 5
    };

    enum MDP3EntryType : uint8_t {
        MDP3_BID   = 0,
        MDP3_OFFER = 1,
        MDP3_TRADE = 2
    };

    // Parse packet header
    [[nodiscard]] inline bool parse_packet_header(
        const uint8_t* buf, size_t len, MDP3PacketHeader& out) noexcept
    {
        if (len < sizeof(MDP3PacketHeader)) return false;
        std::memcpy(&out, buf, sizeof(out));
        return true;
    }

    // Convert MDP3 update action to generic UpdateType
    [[nodiscard]] inline UpdateType to_update_type(uint8_t action) noexcept {
        switch (action) {
            case MDP3_NEW:     return UpdateType::ADD;
            case MDP3_CHANGE:  return UpdateType::MODIFY;
            case MDP3_DELETE:  return UpdateType::DELETE;
            default:           return UpdateType::MODIFY;
        }
    }

    [[nodiscard]] inline Side to_side(uint8_t entry_type) noexcept {
        switch (entry_type) {
            case MDP3_BID:   return Side::BUY;
            case MDP3_OFFER: return Side::SELL;
            default:         return Side::UNKNOWN;
        }
    }

    // =========================================================================
    // dispatch_mdp3 — iterate messages in a UDP datagram and call handler with
    // a normalized MarketUpdate for each MDIncrementalRefresh entry.
    // =========================================================================
    template <typename Handler>
    [[gnu::hot]] inline size_t dispatch_mdp3(
        const uint8_t* buf, size_t len, Handler&& handler) noexcept
    {
        MDP3PacketHeader pkt{};
        if (!parse_packet_header(buf, len, pkt)) return 0;
        size_t off = sizeof(pkt);
        size_t count = 0;

        while (off + sizeof(MDP3MessageHeader) <= len) {
            MDP3MessageHeader mh{};
            std::memcpy(&mh, buf + off, sizeof(mh));
            if (mh.msg_size == 0 || off + mh.msg_size > len) break;

            size_t payload_off = off + sizeof(mh);
            if (payload_off + sizeof(SBEMessageHeader) > off + mh.msg_size) {
                off += mh.msg_size;
                continue;
            }

            SBEMessageHeader sh{};
            std::memcpy(&sh, buf + payload_off, sizeof(sh));
            payload_off += sizeof(sh);

            // We only decode MDIncrementalRefreshBook (template 46)
            if (sh.template_id == 46) {
                size_t entries_end = off + mh.msg_size;
                while (payload_off + sizeof(MDP3IncRefreshEntry) <= entries_end) {
                    MDP3IncRefreshEntry e{};
                    std::memcpy(&e, buf + payload_off, sizeof(e));
                    payload_off += sizeof(e);

                    MarketUpdate update{};
                    update.timestamp = pkt.sending_time;
                    update.instrument_id = static_cast<InstrumentId>(e.security_id);
                    update.type = to_update_type(e.md_update_action);
                    update.side = to_side(e.md_entry_type);
                    // MDP3 prices are scaled 1e9; our fixed-point is 1e4
                    update.price = static_cast<Price>(e.md_entry_px / 100000);
                    update.quantity = static_cast<Quantity>(e.md_entry_size);
                    update.sequence = e.rpt_seq;
                    handler(update);
                    ++count;
                }
            }

            off += mh.msg_size;
        }
        return count;
    }

} // namespace hft::feed
