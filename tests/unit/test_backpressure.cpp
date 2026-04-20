#include <gtest/gtest.h>
#include <thread>
#include "core/backpressure.h"

using namespace hft::core;

// ---- ConflatedUpdate Tests ----

TEST(ConflatedUpdateTest, PublishAndConsume) {
    ConflatedUpdate cu;
    MarketUpdate update{};
    update.instrument_id = 1;
    update.price = 10000;

    cu.publish(update);

    MarketUpdate out{};
    uint64_t last_seq = 0;
    EXPECT_TRUE(cu.consume(out, last_seq));
    EXPECT_EQ(out.instrument_id, 1);
    EXPECT_EQ(out.price, 10000);
    EXPECT_EQ(last_seq, 1U);
}

TEST(ConflatedUpdateTest, NoNewDataReturnsFalse) {
    ConflatedUpdate cu;
    MarketUpdate update{};
    update.price = 100;
    cu.publish(update);

    MarketUpdate out{};
    uint64_t last_seq = 0;
    EXPECT_TRUE(cu.consume(out, last_seq));
    EXPECT_FALSE(cu.consume(out, last_seq));  // no new data
}

TEST(ConflatedUpdateTest, ConflatesMultipleUpdates) {
    ConflatedUpdate cu;
    MarketUpdate update{};

    // Publish 3 updates rapidly
    update.price = 100;
    cu.publish(update);
    update.price = 200;
    cu.publish(update);
    update.price = 300;
    cu.publish(update);

    // Consumer sees only the latest
    MarketUpdate out{};
    uint64_t last_seq = 0;
    EXPECT_TRUE(cu.consume(out, last_seq));
    EXPECT_EQ(out.price, 300);
    EXPECT_EQ(last_seq, 3U);
}

// ---- BackpressurePolicy Tests ----

TEST(BackpressurePolicyTest, PublishAndConsume) {
    // Use unique_ptr to avoid stack overflow (BackpressurePolicy is large)
    auto bp = std::make_unique<BackpressurePolicy>();

    MarketUpdate update{};
    update.instrument_id = 5;
    update.price = 5000;
    bp->publish(5, update);

    MarketUpdate out{};
    uint64_t last_seq = 0;
    EXPECT_TRUE(bp->consume(5, out, last_seq));
    EXPECT_EQ(out.price, 5000);
}

TEST(BackpressurePolicyTest, DifferentInstrumentsIndependent) {
    auto bp = std::make_unique<BackpressurePolicy>();

    MarketUpdate u1{}, u2{};
    u1.instrument_id = 1;
    u1.price = 1000;
    u2.instrument_id = 2;
    u2.price = 2000;

    bp->publish(1, u1);
    bp->publish(2, u2);

    MarketUpdate out{};
    uint64_t seq1 = 0, seq2 = 0;
    EXPECT_TRUE(bp->consume(1, out, seq1));
    EXPECT_EQ(out.price, 1000);

    EXPECT_TRUE(bp->consume(2, out, seq2));
    EXPECT_EQ(out.price, 2000);
}

TEST(BackpressurePolicyTest, DropRateTracking) {
    auto bp = std::make_unique<BackpressurePolicy>();

    MarketUpdate update{};
    update.instrument_id = 1;

    // Publish 5 updates, consume only once
    for (int i = 0; i < 5; ++i) {
        update.price = static_cast<Price>(i * 100);
        bp->publish(1, update);
    }

    MarketUpdate out{};
    uint64_t last_seq = 0;
    EXPECT_TRUE(bp->consume(1, out, last_seq));

    // 4 updates were skipped
    EXPECT_EQ(bp->total_drops(), 4U);
    EXPECT_EQ(bp->total_processed(), 1U);
    EXPECT_GT(bp->drop_rate(), 0.0);
}

TEST(BackpressurePolicyTest, ResetCounters) {
    auto bp = std::make_unique<BackpressurePolicy>();

    MarketUpdate update{};
    update.instrument_id = 1;
    for (int i = 0; i < 3; ++i) {
        update.price = static_cast<Price>(i);
        bp->publish(1, update);
    }

    MarketUpdate out{};
    uint64_t last_seq = 0;
    bp->consume(1, out, last_seq);

    bp->reset_counters();
    EXPECT_EQ(bp->total_drops(), 0U);
    EXPECT_EQ(bp->total_processed(), 0U);
    EXPECT_DOUBLE_EQ(bp->drop_rate(), 0.0);
}
