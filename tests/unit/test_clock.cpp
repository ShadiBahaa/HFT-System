#include <gtest/gtest.h>
#include "core/clock.h"
#include <thread>
#include <chrono>

using namespace hft::core;

TEST(ClockTest, Monotonicity) {
    uint64_t t1 = rdtsc_start();
    // Simulate some work
    volatile int dummy = 0;
    for (int i = 0; i < 1000; ++i) { dummy = i; }
    (void)dummy;
    uint64_t t2 = rdtsc_end();

    EXPECT_GT(t2, t1);
}

TEST(ClockTest, FastExecution) {
    // Ensuring basic non-crashing execution
    uint64_t t1 = rdtsc();
    uint64_t t2 = rdtsc();
    EXPECT_GE(t2, t1);
}
