#include <gtest/gtest.h>
#include "telemetry/latency_profiler.h"

using namespace hft::telemetry;

// ---- LatencyTracker Tests ----

TEST(LatencyTrackerTest, EmptyHistogramReturnsZero) {
    LatencyTracker tracker;
    EXPECT_EQ(tracker.count(), 0U);
    EXPECT_EQ(tracker.percentile(50.0), 0U);
    EXPECT_EQ(tracker.max(), 0U);
    EXPECT_DOUBLE_EQ(tracker.mean(), 0.0);
}

TEST(LatencyTrackerTest, SingleRecord) {
    LatencyTracker tracker;
    tracker.record(1000);
    EXPECT_EQ(tracker.count(), 1U);
    EXPECT_EQ(tracker.min(), 1000U);
    EXPECT_EQ(tracker.max(), 1000U);
}

TEST(LatencyTrackerTest, PercentilesIncreaseMonotonically) {
    LatencyTracker tracker;
    for (int i = 1; i <= 1000; ++i) {
        tracker.record(static_cast<uint64_t>(i) * 100);
    }

    uint64_t p50 = tracker.percentile(50.0);
    uint64_t p90 = tracker.percentile(90.0);
    uint64_t p99 = tracker.percentile(99.0);

    EXPECT_LE(p50, p90);
    EXPECT_LE(p90, p99);
}

TEST(LatencyTrackerTest, P50ApproximatelyCorrect) {
    LatencyTracker tracker;
    // Record 1000 values from 1000 to 1000000
    for (int i = 1; i <= 1000; ++i) {
        tracker.record(static_cast<uint64_t>(i) * 1000);
    }

    uint64_t p50 = tracker.percentile(50.0);
    // p50 should be roughly around 500000 (within 10% given bucketing)
    EXPECT_GT(p50, 400'000U);
    EXPECT_LT(p50, 600'000U);
}

TEST(LatencyTrackerTest, MinMaxTracked) {
    LatencyTracker tracker;
    tracker.record(100);
    tracker.record(5000);
    tracker.record(200);

    EXPECT_EQ(tracker.min(), 100U);
    EXPECT_EQ(tracker.max(), 5000U);
    EXPECT_EQ(tracker.count(), 3U);
}

TEST(LatencyTrackerTest, Reset) {
    LatencyTracker tracker;
    tracker.record(100);
    tracker.record(200);
    tracker.reset();

    EXPECT_EQ(tracker.count(), 0U);
    EXPECT_EQ(tracker.max(), 0U);
    EXPECT_EQ(tracker.percentile(50.0), 0U);
}

TEST(LatencyTrackerTest, LargeValuesClamped) {
    LatencyTracker tracker;
    // Record very large value (10 seconds in ns)
    tracker.record(10'000'000'000ULL);
    EXPECT_EQ(tracker.count(), 1U);
}

// ---- TracePoint Tests ----

TEST(TracePointTest, SizeIs24Bytes) {
    static_assert(sizeof(TracePoint) == 24);
}

// ---- LatencyProfiler Tests ----

TEST(LatencyProfilerTest, AddComponents) {
    LatencyProfiler profiler;
    profiler.add_component("FeedHandler");
    profiler.add_component("OrderBook");
    profiler.add_component("Strategy");

    EXPECT_EQ(profiler.num_components(), 3);
    EXPECT_STREQ(profiler.component_name(0), "FeedHandler");
    EXPECT_STREQ(profiler.component_name(1), "OrderBook");
    EXPECT_STREQ(profiler.component_name(2), "Strategy");
}

TEST(LatencyProfilerTest, TimestampChainMarks) {
    LatencyProfiler::TimestampChain chain;
    chain.mark();
    chain.mark();
    chain.mark();

    EXPECT_EQ(chain.count, 3);
    EXPECT_GT(chain.stamps[0], 0U);
    EXPECT_GE(chain.stamps[1], chain.stamps[0]);
    EXPECT_GE(chain.stamps[2], chain.stamps[1]);
}

TEST(LatencyProfilerTest, ResetChain) {
    LatencyProfiler::TimestampChain chain;
    chain.mark();
    chain.mark();
    chain.reset();
    EXPECT_EQ(chain.count, 0);
}
