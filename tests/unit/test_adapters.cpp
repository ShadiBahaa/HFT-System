#include <gtest/gtest.h>
#include <cstring>
#include "gateway/adapters/nasdaq_ouch.h"
#include "gateway/adapters/cboe_boe.h"
#include "gateway/adapters/cme_ilink3.h"

using namespace hft::gateway::adapters;

// ===== NASDAQ OUCH =====

TEST(OUCHEncoderTest, EncodeEnterOrderSize) {
    OUCHEnterOrder o;
    std::memcpy(o.token, "TOK00000000001", 14);
    o.buy_sell_indicator = 'B';
    o.shares = 100;
    std::memcpy(o.stock, "MSFT    ", 8);
    o.price_usd_e4 = 4150000;
    o.time_in_force = 99999;
    std::memcpy(o.firm, "FIRM", 4);

    uint8_t buf[128];
    size_t n = OUCHEncoder::encode_enter_order(o, buf, sizeof(buf));
    EXPECT_EQ(n, ouch::ENTER_ORDER_SIZE);
    EXPECT_EQ(buf[0], static_cast<uint8_t>(ouch::MSG_ENTER_ORDER));
    EXPECT_EQ(0, std::memcmp(buf + 1, "TOK00000000001", 14));
    EXPECT_EQ(buf[15], static_cast<uint8_t>('B'));
    // Big-endian shares
    EXPECT_EQ(buf[16], 0);
    EXPECT_EQ(buf[17], 0);
    EXPECT_EQ(buf[18], 0);
    EXPECT_EQ(buf[19], 100);
}

TEST(OUCHEncoderTest, BufferTooSmall) {
    OUCHEnterOrder o;
    uint8_t buf[8];
    EXPECT_EQ(OUCHEncoder::encode_enter_order(o, buf, sizeof(buf)), 0U);
}

TEST(OUCHDecoderTest, AcceptedRoundtrip) {
    uint8_t buf[128]{};
    buf[0] = static_cast<uint8_t>(ouch::MSG_ORDER_ACCEPTED);
    // timestamp = 0x0102030405060708
    for (int i = 0; i < 8; ++i) buf[1 + i] = static_cast<uint8_t>(i + 1);
    std::memcpy(buf + 9, "TOK00000000001", 14);
    buf[23] = 'S';
    // shares = 500 (big-endian)
    buf[24] = 0; buf[25] = 0; buf[26] = 0x01; buf[27] = 0xF4;
    std::memcpy(buf + 28, "SPY     ", 8);
    // price = 4000 (big-endian)
    buf[36] = 0; buf[37] = 0; buf[38] = 0x0F; buf[39] = 0xA0;
    // tif
    for (int i = 0; i < 4; ++i) buf[40 + i] = 0;
    std::memcpy(buf + 44, "ABCD", 4);
    buf[48] = 'Y';
    // order_reference = 0x1122334455667788
    for (int i = 0; i < 8; ++i) buf[49 + i] = static_cast<uint8_t>(0x11 * (i + 1));
    buf[57] = 'P';
    buf[58] = 'N';
    // min_qty = 0
    for (int i = 0; i < 4; ++i) buf[59 + i] = 0;
    buf[63] = 'N';
    buf[64] = 'L';

    OUCHOrderAccepted out{};
    ASSERT_TRUE(OUCHDecoder::decode_accepted(buf, 66, out));
    EXPECT_EQ(out.timestamp_ns, 0x0102030405060708ULL);
    EXPECT_EQ(out.buy_sell_indicator, 'S');
    EXPECT_EQ(out.shares, 500U);
    EXPECT_EQ(out.price_usd_e4, 4000U);
    EXPECT_EQ(out.order_reference, 0x1122334455667788ULL);
}

TEST(OUCHDecoderTest, WrongTypeFails) {
    uint8_t buf[66]{};
    buf[0] = 'Z';
    OUCHOrderAccepted out{};
    EXPECT_FALSE(OUCHDecoder::decode_accepted(buf, 66, out));
}

// ===== CBOE BOE =====

TEST(BOEEncoderTest, NewOrderHeaderCorrect) {
    BOENewOrder o;
    std::memcpy(o.cl_ord_id, "CL00000000000000001 ", 20);
    o.side = '1';
    o.order_qty = 250;
    std::memcpy(o.symbol, "AAPL    ", 8);
    o.ord_type = 2;
    o.price_e4 = 1500000;
    std::memcpy(o.account, "ACCT000000000001", 16);

    uint8_t buf[128];
    size_t n = BOEEncoder::encode_new_order(o, 42, 1, buf, sizeof(buf));
    EXPECT_EQ(n, BOEEncoder::NEW_ORDER_TOTAL);

    BOEHeader h;
    ASSERT_TRUE(BOEDecoder::decode_header(buf, n, h));
    EXPECT_EQ(h.start_of_message[0], boe::START_OF_MSG_0);
    EXPECT_EQ(h.start_of_message[1], boe::START_OF_MSG_1);
    EXPECT_EQ(h.msg_length, BOEEncoder::NEW_ORDER_TOTAL);
    EXPECT_EQ(h.msg_type, boe::MSG_NEW_ORDER);
    EXPECT_EQ(h.sequence, 42U);
    EXPECT_EQ(h.matching_unit, 1U);
}

TEST(BOEDecoderTest, BadStartOfMessage) {
    uint8_t buf[64]{0x11, 0x22, 0x33};
    BOEHeader h;
    EXPECT_FALSE(BOEDecoder::decode_header(buf, sizeof(buf), h));
}

TEST(BOEDecoderTest, AckDecode) {
    uint8_t buf[128]{};
    // Header
    buf[0] = boe::START_OF_MSG_0;
    buf[1] = boe::START_OF_MSG_1;
    buf[2] = 58;  // 10 header + 48 body
    buf[3] = 0;
    buf[4] = boe::MSG_ORDER_ACKNOWLEDGE;
    buf[5] = 0;
    // seq = 7
    buf[6] = 7; buf[7] = 0; buf[8] = 0; buf[9] = 0;
    // body: timestamp
    buf[10] = 0x78; buf[11] = 0x56; buf[12] = 0x34; buf[13] = 0x12;
    buf[14] = 0; buf[15] = 0; buf[16] = 0; buf[17] = 0;
    std::memcpy(buf + 18, "CL00000000000000001 ", 20);
    // order_id = 0xAABBCCDD
    buf[38] = 0xDD; buf[39] = 0xCC; buf[40] = 0xBB; buf[41] = 0xAA;
    buf[42] = 0; buf[43] = 0; buf[44] = 0; buf[45] = 0;
    // price_e4 = 0x3E8 (1000)
    buf[46] = 0xE8; buf[47] = 0x03;
    for (int i = 48; i < 54; ++i) buf[i] = 0;
    // leaves_qty = 500
    buf[54] = 0xF4; buf[55] = 0x01; buf[56] = 0; buf[57] = 0;

    BOEHeader h;
    ASSERT_TRUE(BOEDecoder::decode_header(buf, 58, h));
    EXPECT_EQ(h.msg_type, boe::MSG_ORDER_ACKNOWLEDGE);

    BOEOrderAcknowledgement ack{};
    ASSERT_TRUE(BOEDecoder::decode_ack(buf, 58, ack));
    EXPECT_EQ(ack.timestamp_ns, 0x12345678U);
    EXPECT_EQ(ack.order_id, 0xAABBCCDDU);
    EXPECT_EQ(ack.price_e4, 1000);
    EXPECT_EQ(ack.leaves_qty, 500U);
}

// ===== CME iLink3 =====

TEST(ILink3Test, FrameRoundtrip) {
    ILink3Session s;
    s.set_uuid(0xDEADBEEFCAFEBABEULL);
    s.set_next_out_seq(100);

    uint8_t body[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t out[128];
    size_t n = s.frame(ilink3::TMPL_NEW_ORDER_SINGLE, 7, 9, body, 8, 12345, out, sizeof(out));
    ASSERT_GT(n, 0U);

    ILink3Session r;
    ILink3SessionHeader sh{};
    uint16_t tid, sid, ver;
    const uint8_t* body_out = nullptr;
    size_t body_len = 0;
    ASSERT_TRUE(r.unframe(out, n, tid, sid, ver, sh, body_out, body_len));
    EXPECT_EQ(tid, ilink3::TMPL_NEW_ORDER_SINGLE);
    EXPECT_EQ(sid, 7);
    EXPECT_EQ(ver, 9);
    EXPECT_EQ(sh.uuid, 0xDEADBEEFCAFEBABEULL);
    EXPECT_EQ(sh.seq_num, 100U);
    EXPECT_EQ(sh.sending_time_ns, 12345U);
    ASSERT_EQ(body_len, 8U);
    EXPECT_EQ(0, std::memcmp(body_out, body, 8));
}

TEST(ILink3Test, SequenceIncrements) {
    ILink3Session s;
    s.set_next_out_seq(10);
    uint8_t body[1] = {0};
    uint8_t out[128];
    s.frame(ilink3::TMPL_SEQUENCE, 7, 9, body, 1, 0, out, sizeof(out));
    EXPECT_EQ(s.next_out_seq(), 11U);
    s.frame(ilink3::TMPL_SEQUENCE, 7, 9, body, 1, 0, out, sizeof(out));
    EXPECT_EQ(s.next_out_seq(), 12U);
}

TEST(ILink3Test, BuildNegotiate) {
    ILink3Session s;
    ILink3Negotiate n{};
    n.uuid = 0x1234;
    n.request_timestamp_ns = 5555;
    n.firm_id = 42;
    std::memcpy(n.credentials, "user", 4);

    uint8_t buf[128];
    size_t sz = s.build_negotiate(n, buf, sizeof(buf));
    ASSERT_GT(sz, 0U);

    ILink3SessionHeader sh{};
    uint16_t tid, sid, ver;
    const uint8_t* body = nullptr;
    size_t body_len = 0;
    ASSERT_TRUE(s.unframe(buf, sz, tid, sid, ver, sh, body, body_len));
    EXPECT_EQ(tid, ilink3::TMPL_NEGOTIATE);
    EXPECT_EQ(body_len, 42U);
}

TEST(ILink3Test, BadEncodingTypeRejected) {
    ILink3Session s;
    uint8_t buf[40]{};
    // Set length = 40 but wrong encoding type
    buf[0] = 40; buf[1] = 0; buf[2] = 0; buf[3] = 0;
    buf[4] = 0x99; buf[5] = 0x99;

    ILink3SessionHeader sh{};
    uint16_t tid, sid, ver;
    const uint8_t* body = nullptr;
    size_t body_len = 0;
    EXPECT_FALSE(s.unframe(buf, sizeof(buf), tid, sid, ver, sh, body, body_len));
}
