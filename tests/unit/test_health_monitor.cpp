#include <gtest/gtest.h>
#include "telemetry/health_monitor.h"

using namespace hft::telemetry;

TEST(HealthMonitorTest, NormalByDefault) {
    HealthMonitor hm;
    EXPECT_EQ(hm.level(), DegradationLevel::NORMAL);
}

TEST(HealthMonitorTest, DisconnectIsCritical) {
    HealthMonitor hm;
    HealthMetrics m{};
    m.exchange_disconnect = true;
    hm.update(m);
    EXPECT_EQ(hm.level(), DegradationLevel::CRITICAL);
    EXPECT_TRUE(hm.should_halt(m));
}

TEST(HealthMonitorTest, LatencyThresholds) {
    HealthMonitor hm;
    HealthMetrics m{};

    m.latency_p99_ns = 500'000;    // Below warn
    hm.update(m);
    EXPECT_EQ(hm.level(), DegradationLevel::NORMAL);

    m.latency_p99_ns = 2'000'000;  // Warn region
    hm.update(m);
    EXPECT_EQ(hm.level(), DegradationLevel::WARNING);

    m.latency_p99_ns = 20'000'000; // Crit region
    hm.update(m);
    EXPECT_EQ(hm.level(), DegradationLevel::CRITICAL);
}

TEST(HealthMonitorTest, TwoSoftBreachesDegrade) {
    HealthMonitor hm;
    HealthMetrics m{};
    m.latency_p99_ns = 2'000'000;   // warn
    m.packet_loss_rate = 0.005;     // warn
    hm.update(m);
    EXPECT_EQ(hm.level(), DegradationLevel::DEGRADED);
}

TEST(HealthMonitorTest, DrawdownCritical) {
    HealthMonitor hm;
    HealthMetrics m{};
    m.pnl_drawdown = 0.10;  // 10%, above 5% crit
    hm.update(m);
    EXPECT_EQ(hm.level(), DegradationLevel::CRITICAL);
}

TEST(HealthMonitorTest, CustomThresholds) {
    HealthMonitor hm;
    HealthThresholds t = hm.thresholds();
    t.latency_crit_ns = 100'000;  // 100us
    hm.set_thresholds(t);

    HealthMetrics m{};
    m.latency_p99_ns = 200'000;
    hm.update(m);
    EXPECT_EQ(hm.level(), DegradationLevel::CRITICAL);
}
