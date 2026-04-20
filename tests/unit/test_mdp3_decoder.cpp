#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include "feed/mdp3_decoder.h"

using namespace hft::core;
using namespace hft::feed;

TEST(MDP3DecoderTest, ParsePacketHeader) {
    // MDP3PacketHeader is #pragma pack(1), so sending_time sits at offset 4.
    // Writing the struct directly and then reading its uint64_t field via a
    // reference (as EXPECT_EQ does) is a misaligned load that UBSan flags.
    // Build the wire buffer by byte-copying scalars in, and compare to local
    // aligned scalars after parsing.
    const uint32_t seq      = 100U;
    const uint64_t sent_ns  = 1'700'000'000'000'000'000ULL;

    std::vector<uint8_t> buf(sizeof(MDP3PacketHeader));
    std::memcpy(buf.data() + 0, &seq,     sizeof(seq));
    std::memcpy(buf.data() + 4, &sent_ns, sizeof(sent_ns));

    MDP3PacketHeader out{};
    EXPECT_TRUE(parse_packet_header(buf.data(), buf.size(), out));

    uint32_t got_seq;
    uint64_t got_sent;
    std::memcpy(&got_seq,  &out.msg_seq_num,  sizeof(got_seq));
    std::memcpy(&got_sent, &out.sending_time, sizeof(got_sent));
    EXPECT_EQ(got_seq,  seq);
    EXPECT_EQ(got_sent, sent_ns);
}

TEST(MDP3DecoderTest, ParsePacketHeaderTooShort) {
    uint8_t buf[4] = {};
    MDP3PacketHeader out{};
    EXPECT_FALSE(parse_packet_header(buf, sizeof(buf), out));
}

TEST(MDP3DecoderTest, UpdateActionMapping) {
    EXPECT_EQ(to_update_type(MDP3_NEW),    UpdateType::ADD);
    EXPECT_EQ(to_update_type(MDP3_CHANGE), UpdateType::MODIFY);
    EXPECT_EQ(to_update_type(MDP3_DELETE), UpdateType::DELETE);
}

TEST(MDP3DecoderTest, EntryTypeToSide) {
    EXPECT_EQ(to_side(MDP3_BID),   Side::BUY);
    EXPECT_EQ(to_side(MDP3_OFFER), Side::SELL);
    EXPECT_EQ(to_side(MDP3_TRADE), Side::UNKNOWN);
}

TEST(MDP3DecoderTest, DispatchSingleIncRefresh) {
    MDP3PacketHeader pkt{};
    pkt.msg_seq_num = 1;
    pkt.sending_time = 12345;

    MDP3MessageHeader mh{};
    SBEMessageHeader sh{};
    sh.template_id = 46;
    sh.block_length = 0;

    MDP3IncRefreshEntry e{};
    e.md_entry_px = 150000000000LL;      // 1500.0 in 1e8 scaling -> 150000 (after /100000)
    e.md_entry_size = 200;
    e.security_id = 42;
    e.rpt_seq = 10;
    e.md_price_level = 1;
    e.md_update_action = MDP3_NEW;
    e.md_entry_type = MDP3_BID;

    mh.msg_size = static_cast<uint16_t>(sizeof(mh) + sizeof(sh) + sizeof(e));

    std::vector<uint8_t> buf;
    buf.resize(sizeof(pkt) + mh.msg_size);
    size_t off = 0;
    std::memcpy(buf.data() + off, &pkt, sizeof(pkt)); off += sizeof(pkt);
    std::memcpy(buf.data() + off, &mh,  sizeof(mh));  off += sizeof(mh);
    std::memcpy(buf.data() + off, &sh,  sizeof(sh));  off += sizeof(sh);
    std::memcpy(buf.data() + off, &e,   sizeof(e));

    size_t count = 0;
    dispatch_mdp3(buf.data(), buf.size(), [&](const MarketUpdate& u) {
        EXPECT_EQ(u.instrument_id, 42);
        EXPECT_EQ(u.type, UpdateType::ADD);
        EXPECT_EQ(u.side, Side::BUY);
        EXPECT_EQ(u.quantity, 200);
        EXPECT_EQ(u.sequence, 10U);
        ++count;
    });
    EXPECT_EQ(count, 1U);
}

TEST(MDP3DecoderTest, EmptyBuffer) {
    size_t count = dispatch_mdp3(nullptr, 0, [&](const MarketUpdate&) {});
    EXPECT_EQ(count, 0U);
}
