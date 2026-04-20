#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include "feed/pitch_decoder.h"

using namespace hft::core;
using namespace hft::feed;

TEST(PitchDecoderTest, DispatchAddOrder) {
    PITCHAddOrder add{};
    add.length = sizeof(PITCHAddOrder);
    add.msg_type = PITCH_ADD_ORDER_LONG;
    add.time_offset = 1234;
    add.order_id = 42;
    add.side = 'B';
    add.quantity = 500;
    std::memcpy(add.symbol, "AAPL  ", 6);
    add.price = 1500000;
    add.flags = 0;

    std::vector<uint8_t> buf(sizeof(add));
    std::memcpy(buf.data(), &add, sizeof(add));

    std::vector<MarketUpdate> out;
    size_t count = dispatch_pitch(buf.data(), buf.size(),
        [&](const MarketUpdate& u) { out.push_back(u); });

    EXPECT_EQ(count, 1U);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, UpdateType::ADD);
    EXPECT_EQ(out[0].side, Side::BUY);
    EXPECT_EQ(out[0].price, 1500000);
    EXPECT_EQ(out[0].quantity, 500);
    EXPECT_EQ(out[0].order_ref, 42U);
}

TEST(PitchDecoderTest, DispatchExecuted) {
    PITCHOrderExecuted ex{};
    ex.length = sizeof(ex);
    ex.msg_type = PITCH_EXECUTED;
    ex.order_id = 17;
    ex.executed_qty = 100;
    ex.execution_id = 99;

    std::vector<uint8_t> buf(sizeof(ex));
    std::memcpy(buf.data(), &ex, sizeof(ex));

    size_t count = 0;
    dispatch_pitch(buf.data(), buf.size(), [&](const MarketUpdate& u) {
        EXPECT_EQ(u.type, UpdateType::EXECUTE);
        EXPECT_EQ(u.order_ref, 17U);
        EXPECT_EQ(u.quantity, 100);
        ++count;
    });
    EXPECT_EQ(count, 1U);
}

TEST(PitchDecoderTest, DispatchDelete) {
    PITCHOrderDelete d{};
    d.length = sizeof(d);
    d.msg_type = PITCH_DELETE;
    d.order_id = 55;

    std::vector<uint8_t> buf(sizeof(d));
    std::memcpy(buf.data(), &d, sizeof(d));

    size_t count = 0;
    dispatch_pitch(buf.data(), buf.size(), [&](const MarketUpdate& u) {
        EXPECT_EQ(u.type, UpdateType::DELETE);
        EXPECT_EQ(u.order_ref, 55U);
        ++count;
    });
    EXPECT_EQ(count, 1U);
}

TEST(PitchDecoderTest, UnknownMessageSkipped) {
    uint8_t buf[8] = {8, 0xFF, 0, 0, 0, 0, 0, 0};  // unknown type
    size_t count = dispatch_pitch(buf, sizeof(buf), [&](const MarketUpdate&) {});
    EXPECT_EQ(count, 0U);
}

TEST(PitchDecoderTest, EmptyBuffer) {
    size_t count = dispatch_pitch(nullptr, 0, [&](const MarketUpdate&) {});
    EXPECT_EQ(count, 0U);
}

TEST(PitchDecoderTest, MultipleMessages) {
    PITCHAddOrder a1{};
    a1.length = sizeof(a1);
    a1.msg_type = PITCH_ADD_ORDER_LONG;
    a1.order_id = 1;
    a1.side = 'B';
    a1.price = 100;
    a1.quantity = 10;

    PITCHAddOrder a2{};
    a2.length = sizeof(a2);
    a2.msg_type = PITCH_ADD_ORDER_LONG;
    a2.order_id = 2;
    a2.side = 'S';
    a2.price = 110;
    a2.quantity = 20;

    std::vector<uint8_t> buf(2 * sizeof(PITCHAddOrder));
    std::memcpy(buf.data(), &a1, sizeof(a1));
    std::memcpy(buf.data() + sizeof(a1), &a2, sizeof(a2));

    size_t count = dispatch_pitch(buf.data(), buf.size(),
        [&](const MarketUpdate&) {});
    EXPECT_EQ(count, 2U);
}
