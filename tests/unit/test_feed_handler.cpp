#include <gtest/gtest.h>
#include "feed/feed_handler.h"
#include "feed/itch_decoder.h"
#include "feed/normalizer.h"
#include "feed/gap_detector.h"
#include <cstring>

using namespace hft::core;
using namespace hft::feed;

// ---- Endian Helpers ----

// Write a big-endian 16
static void write_be16(void* p, uint16_t v) {
    auto* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(v >> 8);
    b[1] = static_cast<uint8_t>(v);
}

// Write a big-endian 32
static void write_be32(void* p, uint32_t v) {
    auto* b = static_cast<uint8_t*>(p);
    b[0] = static_cast<uint8_t>(v >> 24);
    b[1] = static_cast<uint8_t>(v >> 16);
    b[2] = static_cast<uint8_t>(v >> 8);
    b[3] = static_cast<uint8_t>(v);
}

// Write a big-endian 64
static void write_be64(void* p, uint64_t v) {
    auto* b = static_cast<uint8_t*>(p);
    for (int i = 7; i >= 0; --i) {
        b[7 - i] = static_cast<uint8_t>(v >> (i * 8));
    }
}

// Write a 6-byte ITCH timestamp
static void write_itch_ts(uint8_t ts[6], uint64_t ns) {
    for (int i = 5; i >= 0; --i) {
        ts[i] = static_cast<uint8_t>(ns & 0xFF);
        ns >>= 8;
    }
}

// ---- ITCH Decoder Tests ----

TEST(ITCHDecoderTest, DecodeAddOrder) {
    ITCHAddOrder msg{};
    msg.msg_type = 'A';
    write_be16(&msg.stock_locate, 42);
    write_itch_ts(msg.timestamp, 1000000);
    write_be64(&msg.order_ref, 12345);
    msg.side = 'B';
    write_be32(&msg.shares, 100);
    write_be32(&msg.price, 15000);

    MarketUpdate update{};
    bool ok = dispatch_itch(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), update);

    ASSERT_TRUE(ok);
    EXPECT_EQ(update.type, UpdateType::ADD);
    EXPECT_EQ(update.order_ref, 12345U);
    EXPECT_EQ(update.side, Side::BUY);
    EXPECT_EQ(update.quantity, 100);
    EXPECT_EQ(update.price, 15000);
    EXPECT_EQ(update.instrument_id, 42);
    EXPECT_EQ(update.timestamp, 1000000U);
}

TEST(ITCHDecoderTest, DecodeOrderExecuted) {
    ITCHOrderExecuted msg{};
    msg.msg_type = 'E';
    write_be16(&msg.stock_locate, 10);
    write_itch_ts(msg.timestamp, 2000000);
    write_be64(&msg.order_ref, 54321);
    write_be32(&msg.executed_shares, 50);

    MarketUpdate update{};
    bool ok = dispatch_itch(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), update);

    ASSERT_TRUE(ok);
    EXPECT_EQ(update.type, UpdateType::EXECUTE);
    EXPECT_EQ(update.order_ref, 54321U);
    EXPECT_EQ(update.quantity, 50);
}

TEST(ITCHDecoderTest, DecodeOrderDelete) {
    ITCHOrderDelete msg{};
    msg.msg_type = 'D';
    write_be16(&msg.stock_locate, 5);
    write_itch_ts(msg.timestamp, 3000000);
    write_be64(&msg.order_ref, 99999);

    MarketUpdate update{};
    bool ok = dispatch_itch(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), update);

    ASSERT_TRUE(ok);
    EXPECT_EQ(update.type, UpdateType::DELETE);
    EXPECT_EQ(update.order_ref, 99999U);
}

TEST(ITCHDecoderTest, DecodeOrderReplace) {
    ITCHOrderReplace msg{};
    msg.msg_type = 'U';
    write_be16(&msg.stock_locate, 7);
    write_itch_ts(msg.timestamp, 4000000);
    write_be64(&msg.orig_order_ref, 11111);
    write_be64(&msg.new_order_ref, 22222);
    write_be32(&msg.shares, 300);
    write_be32(&msg.price, 16000);

    MarketUpdate update{};
    bool ok = dispatch_itch(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), update);

    ASSERT_TRUE(ok);
    EXPECT_EQ(update.type, UpdateType::MODIFY);
    EXPECT_EQ(update.order_ref, 11111U);  // orig ref for replace
    EXPECT_EQ(update.quantity, 300);
    EXPECT_EQ(update.price, 16000);
}

TEST(ITCHDecoderTest, DecodeTrade) {
    ITCHTrade msg{};
    msg.msg_type = 'P';
    write_be16(&msg.stock_locate, 3);
    write_itch_ts(msg.timestamp, 5000000);
    write_be64(&msg.order_ref, 77777);
    msg.side = 'S';
    write_be32(&msg.shares, 500);
    write_be32(&msg.price, 20000);

    MarketUpdate update{};
    bool ok = dispatch_itch(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), update);

    ASSERT_TRUE(ok);
    EXPECT_EQ(update.type, UpdateType::TRADE);
    EXPECT_EQ(update.side, Side::SELL);
    EXPECT_EQ(update.quantity, 500);
    EXPECT_EQ(update.price, 20000);
}

TEST(ITCHDecoderTest, UnknownMessageType) {
    uint8_t buf[64]{};
    buf[0] = 'Z';  // Unknown type

    MarketUpdate update{};
    EXPECT_FALSE(dispatch_itch(buf, sizeof(buf), update));
}

TEST(ITCHDecoderTest, TruncatedMessage) {
    ITCHAddOrder msg{};
    msg.msg_type = 'A';

    MarketUpdate update{};
    // Pass less data than required
    EXPECT_FALSE(dispatch_itch(reinterpret_cast<const uint8_t*>(&msg), 5, update));
}

// ---- Gap Detector Tests ----

TEST(GapDetectorTest, InOrderSequence) {
    GapDetector gd;

    EXPECT_TRUE(gd.check(1));
    EXPECT_TRUE(gd.check(2));
    EXPECT_TRUE(gd.check(3));
    EXPECT_EQ(gd.gap_count(), 0U);
    EXPECT_EQ(gd.expected_seq(), 4U);
}

TEST(GapDetectorTest, DetectsGap) {
    GapDetector gd;

    EXPECT_TRUE(gd.check(1));
    EXPECT_TRUE(gd.check(2));
    EXPECT_TRUE(gd.check(5));  // Gap: 3-4 missing

    EXPECT_EQ(gd.gap_count(), 1U);
    EXPECT_EQ(gd.gap_at(0).begin, 3U);
    EXPECT_EQ(gd.gap_at(0).end, 4U);
}

TEST(GapDetectorTest, DiscardsDuplicates) {
    GapDetector gd;

    EXPECT_TRUE(gd.check(1));
    EXPECT_TRUE(gd.check(2));
    EXPECT_FALSE(gd.check(1));  // Old/duplicate
    EXPECT_FALSE(gd.check(2));  // Duplicate
}

TEST(GapDetectorTest, MultipleGaps) {
    GapDetector gd;

    gd.check(1);
    gd.check(5);   // Gap 2-4
    gd.check(10);  // Gap 6-9

    EXPECT_EQ(gd.gap_count(), 2U);
    EXPECT_EQ(gd.gap_at(0).begin, 2U);
    EXPECT_EQ(gd.gap_at(0).end, 4U);
    EXPECT_EQ(gd.gap_at(1).begin, 6U);
    EXPECT_EQ(gd.gap_at(1).end, 9U);
}

TEST(GapDetectorTest, Reset) {
    GapDetector gd;
    gd.check(1);
    gd.check(5);  // Creates a gap

    gd.reset(100);
    EXPECT_EQ(gd.expected_seq(), 100U);
    EXPECT_EQ(gd.gap_count(), 0U);
}

// ---- FeedHandler Integration ----

TEST(FeedHandlerTest, ProcessPacketEndToEnd) {
    SPSCRingBuffer<MarketUpdate, 1024> ring;
    FeedHandler<1024> handler(ring);

    // Build an ITCH AddOrder
    ITCHAddOrder msg{};
    msg.msg_type = 'A';
    write_be16(&msg.stock_locate, 1);
    write_itch_ts(msg.timestamp, 9999);
    write_be64(&msg.order_ref, 42);
    msg.side = 'B';
    write_be32(&msg.shares, 200);
    write_be32(&msg.price, 10050);

    EXPECT_TRUE(handler.process_packet(
        reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 1));

    EXPECT_EQ(handler.packets_processed(), 1U);
    EXPECT_EQ(handler.packets_dropped(), 0U);

    // Read from ring
    MarketUpdate update{};
    ASSERT_TRUE(ring.try_pop(update));
    EXPECT_EQ(update.type, UpdateType::ADD);
    EXPECT_EQ(update.order_ref, 42U);
    EXPECT_EQ(update.side, Side::BUY);
    EXPECT_EQ(update.quantity, 200);
    EXPECT_EQ(update.price, 10050);
    EXPECT_EQ(update.sequence, 1U);
}

TEST(FeedHandlerTest, DropsDuplicateSequence) {
    SPSCRingBuffer<MarketUpdate, 1024> ring;
    FeedHandler<1024> handler(ring);

    ITCHAddOrder msg{};
    msg.msg_type = 'A';
    write_be16(&msg.stock_locate, 1);
    write_itch_ts(msg.timestamp, 100);
    write_be64(&msg.order_ref, 1);
    msg.side = 'B';
    write_be32(&msg.shares, 100);
    write_be32(&msg.price, 10000);

    EXPECT_TRUE(handler.process_packet(
        reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 1));
    EXPECT_FALSE(handler.process_packet(
        reinterpret_cast<const uint8_t*>(&msg), sizeof(msg), 1));  // Duplicate

    EXPECT_EQ(handler.packets_processed(), 1U);
    EXPECT_EQ(handler.packets_dropped(), 1U);
}
