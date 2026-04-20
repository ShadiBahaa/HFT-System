#include <gtest/gtest.h>
#include "telemetry/metrics_publisher.h"

using namespace hft::telemetry;

TEST(MetricsPublisherTest, RegisterAndIncrement) {
    MetricsPublisher p;
    int h = p.register_metric("orders_sent_total", "Total orders sent", MetricType::COUNTER);
    ASSERT_GE(h, 0);
    p.increment(h);
    p.increment(h, 3.0);
    EXPECT_DOUBLE_EQ(p.get(h), 4.0);
}

TEST(MetricsPublisherTest, SetGauge) {
    MetricsPublisher p;
    int h = p.register_metric("position_size", "Current position", MetricType::GAUGE);
    ASSERT_GE(h, 0);
    p.set(h, 42.5);
    EXPECT_DOUBLE_EQ(p.get(h), 42.5);
    p.set(h, -10.0);
    EXPECT_DOUBLE_EQ(p.get(h), -10.0);
}

TEST(MetricsPublisherTest, DuplicateRegistrationReturnsSameHandle) {
    MetricsPublisher p;
    int h1 = p.register_metric("foo", "bar", MetricType::COUNTER);
    int h2 = p.register_metric("foo", "bar", MetricType::COUNTER);
    EXPECT_EQ(h1, h2);
}

TEST(MetricsPublisherTest, TypeMismatchRejected) {
    MetricsPublisher p;
    int h1 = p.register_metric("x", "", MetricType::COUNTER);
    int h2 = p.register_metric("x", "", MetricType::GAUGE);
    EXPECT_GE(h1, 0);
    EXPECT_LT(h2, 0);
}

TEST(MetricsPublisherTest, Snapshot) {
    MetricsPublisher p;
    int a = p.register_metric("a", "help a", MetricType::COUNTER);
    int b = p.register_metric("b", "help b", MetricType::GAUGE);
    p.increment(a, 7);
    p.set(b, 3.14);

    auto snap = p.snapshot();
    ASSERT_EQ(snap.size(), 2U);
    EXPECT_EQ(snap[0].name, "a");
    EXPECT_DOUBLE_EQ(snap[0].value, 7.0);
    EXPECT_EQ(snap[1].name, "b");
    EXPECT_DOUBLE_EQ(snap[1].value, 3.14);
}

TEST(MetricsPublisherTest, PrometheusFormat) {
    MetricsPublisher p;
    int h = p.register_metric("requests_total", "Total requests", MetricType::COUNTER);
    p.increment(h, 123);

    std::string out = PrometheusExporter::format(p);
    EXPECT_NE(out.find("# HELP requests_total Total requests"), std::string::npos);
    EXPECT_NE(out.find("# TYPE requests_total counter"), std::string::npos);
    EXPECT_NE(out.find("requests_total 123"), std::string::npos);
}

TEST(MetricsPublisherTest, BadHandleNoCrash) {
    MetricsPublisher p;
    p.increment(-1);
    p.set(42, 1.0);
    EXPECT_DOUBLE_EQ(p.get(999), 0.0);
}
