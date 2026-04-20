#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include "core/clock.h"

using namespace hft::core;

TEST(TSCCalibrationTest, CalibrateProducesNonZeroRatio) {
    TSCCalibration cal;
    cal.calibrate();
    EXPECT_GT(cal.tsc_to_ns_ratio, 0.0);
    EXPECT_GT(cal.base_tsc, 0U);
    EXPECT_GT(cal.base_ns, 0U);
}

TEST(TSCCalibrationTest, TscFrequencyReasonable) {
    TSCCalibration cal;
    cal.calibrate();
    double freq = cal.tsc_freq_hz();
    // TSC frequency should be between 500MHz and 10GHz for any modern CPU
    EXPECT_GT(freq, 500'000'000.0);
    EXPECT_LT(freq, 10'000'000'000.0);
}

TEST(TSCCalibrationTest, TscToNsMonotonic) {
    TSCCalibration cal;
    cal.calibrate();

    uint64_t tsc1 = rdtsc_end();
    uint64_t tsc2 = rdtsc_end();

    uint64_t ns1 = cal.tsc_to_ns(tsc1);
    uint64_t ns2 = cal.tsc_to_ns(tsc2);

    EXPECT_GE(ns2, ns1);
}

TEST(TSCCalibrationTest, TscToNsReasonableDelay) {
    TSCCalibration cal;
    cal.calibrate();

    uint64_t tsc_before = rdtsc_end();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uint64_t tsc_after = rdtsc_end();

    uint64_t ns_before = cal.tsc_to_ns(tsc_before);
    uint64_t ns_after = cal.tsc_to_ns(tsc_after);
    uint64_t delta = ns_after - ns_before;

    // Should be at least 500us (sleep(1ms) might be shorter) and less than 50ms
    EXPECT_GT(delta, 500'000U);
    EXPECT_LT(delta, 50'000'000U);
}

TEST(TSCCalibrationTest, RecalibrateAdjustsCorrectionFactor) {
    TSCCalibration cal;
    cal.calibrate();

    double initial = cal.correction_factor.load();
    EXPECT_DOUBLE_EQ(initial, 1.0);

    cal.recalibrate();
    double updated = cal.correction_factor.load();
    // Should be close to 1.0 (within 1% for a short interval)
    EXPECT_NEAR(updated, 1.0, 0.01);
}

// ---- WallClock tests ----

TEST(WallClockTest, NowNsReturnsNonZero) {
    WallClock clock;
    uint64_t ns = clock.now_ns();
    EXPECT_GT(ns, 0U);
}

TEST(WallClockTest, NowNsMonotonic) {
    WallClock clock;
    uint64_t ns1 = clock.now_ns();
    uint64_t ns2 = clock.now_ns();
    EXPECT_GE(ns2, ns1);
}

// ---- SimulatedClock tests ----

TEST(SimulatedClockTest, StartsAtZero) {
    SimulatedClock clock;
    EXPECT_EQ(clock.now_ns(), 0U);
}

TEST(SimulatedClockTest, SetAndRead) {
    SimulatedClock clock;
    clock.set(1'000'000'000);
    EXPECT_EQ(clock.now_ns(), 1'000'000'000U);
}

TEST(SimulatedClockTest, Advance) {
    SimulatedClock clock;
    clock.set(100);
    clock.advance(50);
    EXPECT_EQ(clock.now_ns(), 150U);
}
