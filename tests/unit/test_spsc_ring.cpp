#include <gtest/gtest.h>
#include "core/spsc_ring.h"
#include <thread>
#include <vector>
#include <atomic>

using namespace hft::core;

TEST(SPSCRingTest, SingleThreadPushPop) {
    SPSCRingBuffer<int, 4> ring;
    int val = 0;

    EXPECT_FALSE(ring.try_pop(val));
    EXPECT_TRUE(ring.try_push(42));
    EXPECT_TRUE(ring.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_FALSE(ring.try_pop(val));
}

TEST(SPSCRingTest, CapacityLimits) {
    SPSCRingBuffer<int, 4> ring;

    EXPECT_TRUE(ring.try_push(1));
    EXPECT_TRUE(ring.try_push(2));
    EXPECT_TRUE(ring.try_push(3));
    EXPECT_TRUE(ring.try_push(4));

    // Should fail when full
    EXPECT_FALSE(ring.try_push(5));

    int val;
    EXPECT_TRUE(ring.try_pop(val));
    EXPECT_EQ(val, 1);

    // Now it should have room for 1 more
    EXPECT_TRUE(ring.try_push(6));
    EXPECT_FALSE(ring.try_push(7)); // Full again
}

TEST(SPSCRingTest, MultiThreadConcurrency) {
    constexpr size_t NUM_ITEMS = 1'000'000;
    SPSCRingBuffer<size_t, 1024> ring;

    std::atomic<bool> start_flag{false};
    size_t sums[2] = {0, 0};

    std::thread producer([&]() {
        while (!start_flag.load()) {} // spin wait

        for (size_t i = 1; i <= NUM_ITEMS; ++i) {
            while (!ring.try_push(i)) {
                // busy wait while full
            }
            sums[0] += i;
        }
    });

    std::thread consumer([&]() {
        while (!start_flag.load()) {} // spin wait

        for (size_t i = 1; i <= NUM_ITEMS; ++i) {
            size_t val;
            while (!ring.try_pop(val)) {
                // busy wait while empty
            }
            sums[1] += val;
        }
    });

    // Unleash threads
    start_flag.store(true);

    producer.join();
    consumer.join();

    // Verify all items were transferred correctly
    EXPECT_EQ(sums[0], sums[1]);

    // Expected sum of 1 to NUM_ITEMS: n * (n + 1) / 2
    size_t expected_sum = NUM_ITEMS * (NUM_ITEMS + 1) / 2;
    EXPECT_EQ(sums[1], expected_sum);
}
