#include <gtest/gtest.h>
#include <cstdio>
#include <vector>
#include "persistence/market_data_recorder.h"

using namespace hft::core;
using namespace hft::persistence;

namespace {
    std::string tmp_path(const char* tag) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "mdr_test_%s_%p.bin", tag, static_cast<void*>(&tag));
        return buf;
    }

    MarketUpdate make_update(uint64_t seq, Price px, Quantity qty) {
        MarketUpdate u{};
        u.timestamp = 1000 + seq;
        u.instrument_id = 7;
        u.type = UpdateType::ADD;
        u.side = Side::BUY;
        u.price = px;
        u.quantity = qty;
        u.order_ref = 100 + seq;
        u.sequence = seq;
        return u;
    }
}

TEST(MarketDataRecorderTest, RoundTrip) {
    auto path = tmp_path("roundtrip");

    {
        MarketDataRecorder r;
        ASSERT_TRUE(r.open(path.c_str()));
        for (uint64_t i = 0; i < 10; ++i) {
            ASSERT_TRUE(r.record(make_update(i, 10000 + i, 100)));
        }
        EXPECT_EQ(r.record_count(), 10U);
    }

    std::vector<MarketUpdate> out;
    {
        MarketDataPlayer p;
        ASSERT_TRUE(p.open(path.c_str()));
        MarketUpdate u;
        while (p.next(u)) out.push_back(u);
    }

    ASSERT_EQ(out.size(), 10U);
    for (uint64_t i = 0; i < 10; ++i) {
        EXPECT_EQ(out[i].sequence, i);
        EXPECT_EQ(out[i].price, static_cast<Price>(10000 + i));
    }

    std::remove(path.c_str());
}

TEST(MarketDataRecorderTest, RecordWhileClosedFails) {
    MarketDataRecorder r;
    EXPECT_FALSE(r.record(make_update(1, 100, 1)));
}

TEST(MarketDataRecorderTest, ReplayLambda) {
    auto path = tmp_path("replay");
    {
        MarketDataRecorder r;
        ASSERT_TRUE(r.open(path.c_str()));
        for (uint64_t i = 0; i < 5; ++i) r.record(make_update(i, 500, 10));
    }

    MarketDataPlayer p;
    ASSERT_TRUE(p.open(path.c_str()));
    uint64_t sum = 0;
    uint64_t count = p.replay([&](const MarketUpdate& u) { sum += u.sequence; });
    EXPECT_EQ(count, 5U);
    EXPECT_EQ(sum, 0U + 1 + 2 + 3 + 4);

    std::remove(path.c_str());
}
