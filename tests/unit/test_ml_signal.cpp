#include <gtest/gtest.h>
#include <cstdio>
#include "strategy/ml_signal.h"

#if defined(_WIN32)
  #include <windows.h>   // GetCurrentProcessId
#else
  #include <unistd.h>    // getpid
#endif

using namespace hft::strategy;

namespace {
    // Unique shm name per test run to avoid collision across parallel runs.
    std::string make_name() {
        char buf[64];
#if defined(_WIN32)
        const int pid = static_cast<int>(::GetCurrentProcessId());
#else
        const int pid = static_cast<int>(::getpid());
#endif
        std::snprintf(buf, sizeof(buf), "hft_ml_test_%d_%p", pid, (void*)&buf);
        return buf;
    }
}

TEST(MLSignalTest, WriterReaderRoundtrip) {
    auto name = make_name();
    MLSignalWriter w;
    ASSERT_TRUE(w.create(name.c_str()));

    MLSignalReader r;
    ASSERT_TRUE(r.connect(name.c_str()));

    MLSignalParams p{};
    p.spread_multiplier = 1.5;
    p.inventory_bias = -0.2;
    p.update_timestamp_ns = 12345;
    p.model_version = 7;
    w.publish(p);

    MLSignalParams out{};
    ASSERT_TRUE(r.read(out));
    EXPECT_DOUBLE_EQ(out.spread_multiplier, 1.5);
    EXPECT_DOUBLE_EQ(out.inventory_bias, -0.2);
    EXPECT_EQ(out.update_timestamp_ns, 12345U);
    EXPECT_EQ(out.model_version, 7U);

    ml_signal_unlink(name.c_str());
}

TEST(MLSignalTest, StaleDetection) {
    auto name = make_name();
    MLSignalWriter w;
    ASSERT_TRUE(w.create(name.c_str()));
    MLSignalReader r;
    ASSERT_TRUE(r.connect(name.c_str()));

    MLSignalParams p{};
    p.update_timestamp_ns = 1000;
    w.publish(p);

    EXPECT_FALSE(r.is_stale(1500, 1000));   // 500 < 1000
    EXPECT_TRUE(r.is_stale(5000, 1000));    // 4000 > 1000

    ml_signal_unlink(name.c_str());
}

TEST(MLSignalTest, MultipleUpdates) {
    auto name = make_name();
    MLSignalWriter w;
    ASSERT_TRUE(w.create(name.c_str()));
    MLSignalReader r;
    ASSERT_TRUE(r.connect(name.c_str()));

    for (uint32_t v = 1; v <= 5; ++v) {
        MLSignalParams p{};
        p.model_version = v;
        p.update_timestamp_ns = 100ULL * v;
        w.publish(p);

        MLSignalParams out{};
        ASSERT_TRUE(r.read(out));
        EXPECT_EQ(out.model_version, v);
        EXPECT_EQ(out.update_timestamp_ns, 100ULL * v);
    }

    ml_signal_unlink(name.c_str());
}

TEST(MLSignalTest, UnconnectedReaderFails) {
    MLSignalReader r;
    MLSignalParams out{};
    EXPECT_FALSE(r.read(out));
}
