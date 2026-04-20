#include <gtest/gtest.h>
#include "risk/pre_trade_risk.h"
#include "risk/kill_switch.h"
#include "risk/throttle.h"

using namespace hft::core;
using namespace hft::risk;

// ---- KillSwitch Tests ----

TEST(KillSwitchTest, InitiallyInactive) {
    KillSwitch ks;
    EXPECT_FALSE(ks.is_active());
    EXPECT_FALSE(ks.is_strategy_killed(0));
}

TEST(KillSwitchTest, GlobalActivation) {
    KillSwitch ks;
    ks.activate_global();

    EXPECT_TRUE(ks.is_active());
    EXPECT_TRUE(ks.is_strategy_killed(0));
    EXPECT_TRUE(ks.is_strategy_killed(5));

    ks.deactivate_global();
    EXPECT_FALSE(ks.is_active());
}

TEST(KillSwitchTest, PerStrategyKill) {
    KillSwitch ks;
    ks.activate_strategy(2);

    EXPECT_FALSE(ks.is_active());           // Global not active
    EXPECT_FALSE(ks.is_strategy_killed(0)); // Strategy 0 fine
    EXPECT_TRUE(ks.is_strategy_killed(2));  // Strategy 2 killed

    ks.deactivate_strategy(2);
    EXPECT_FALSE(ks.is_strategy_killed(2));
}

TEST(KillSwitchTest, OutOfRangeStrategy) {
    KillSwitch ks;
    EXPECT_TRUE(ks.is_strategy_killed(-1));   // Invalid = killed
    EXPECT_TRUE(ks.is_strategy_killed(999));  // Out of range = killed
}

// ---- ExchangeThrottle Tests ----

TEST(ExchangeThrottleTest, AllowsWithinLimits) {
    ExchangeThrottle throttle;
    // 300 new/sec, 500 cancel/sec, 1000 total/sec, burst=50
    throttle.configure(300, 500, 1000, 50, 3.0e9);

    // Should allow up to burst limit immediately
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(throttle.allow_new_order());
    }
}

TEST(ExchangeThrottleTest, RejectsBeyondBurst) {
    ExchangeThrottle throttle;
    throttle.configure(300, 500, 1000, 10, 3.0e9);

    // Drain all tokens
    for (int i = 0; i < 10; ++i) {
        (void)throttle.allow_new_order();
    }

    // Next should fail (no time for refill)
    EXPECT_FALSE(throttle.allow_new_order());
}

TEST(ExchangeThrottleTest, CancelSeparateFromNew) {
    ExchangeThrottle throttle;
    throttle.configure(300, 500, 1000, 10, 3.0e9);

    // Drain new order tokens
    for (int i = 0; i < 10; ++i) {
        (void)throttle.allow_new_order();
    }

    // Cancel tokens should still be available
    EXPECT_TRUE(throttle.allow_cancel());
}

// ---- PreTradeRisk Tests ----

class PreTradeRiskTest : public ::testing::Test {
protected:
    std::array<GlobalPosition, MAX_INSTRUMENTS> global_positions_{};
    KillSwitch kill_switch_;
    PreTradeRisk* risk_{nullptr};

    void SetUp() override {
        risk_ = new PreTradeRisk(global_positions_, kill_switch_, 0);

        InstrumentLimits lim{};
        lim.max_position = 1000;
        lim.max_notional = 100'000'000;
        lim.max_orders_per_sec = 100;
        lim.max_order_size = 500;
        lim.fat_finger_price = 1000;
        risk_->set_limits(1, lim);
    }

    void TearDown() override {
        delete risk_;
    }
};

TEST_F(PreTradeRiskTest, PassesValidOrder) {
    RiskResult result = risk_->check(1, Side::BUY, 100, 10000, 10010);
    EXPECT_EQ(result, RiskResult::PASS);
}

TEST_F(PreTradeRiskTest, RejectsPositionBreach) {
    // Fill up to limit
    for (int i = 0; i < 10; ++i) {
        (void)risk_->check(1, Side::BUY, 100, 10000, 10010);
    }
    // This should breach (position = 1000 + 100 > max 1000)
    RiskResult result = risk_->check(1, Side::BUY, 100, 10000, 10010);
    EXPECT_EQ(result, RiskResult::POSITION_BREACH);
}

TEST_F(PreTradeRiskTest, RejectsSizeBreach) {
    // max_order_size = 500
    RiskResult result = risk_->check(1, Side::BUY, 600, 10000, 10010);
    EXPECT_EQ(result, RiskResult::SIZE_BREACH);
}

TEST_F(PreTradeRiskTest, RejectsPriceBreach) {
    // fat_finger_price = 1000, deviation = |15000 - 10000| = 5000
    RiskResult result = risk_->check(1, Side::BUY, 100, 15000, 10000);
    EXPECT_EQ(result, RiskResult::PRICE_BREACH);
}

TEST_F(PreTradeRiskTest, RejectsRateBreach) {
    // max_orders_per_sec = 100
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(risk_->check(1, Side::BUY, 1, 10000, 10010), RiskResult::PASS);
    }
    EXPECT_EQ(risk_->check(1, Side::BUY, 1, 10000, 10010), RiskResult::RATE_BREACH);
}

TEST_F(PreTradeRiskTest, RejectsWhenKillSwitchActive) {
    kill_switch_.activate_global();
    RiskResult result = risk_->check(1, Side::BUY, 100, 10000, 10010);
    EXPECT_EQ(result, RiskResult::KILL_SWITCH);
}

TEST_F(PreTradeRiskTest, ResetRateCounters) {
    for (int i = 0; i < 100; ++i) {
        (void)risk_->check(1, Side::BUY, 1, 10000, 10010);
    }
    EXPECT_EQ(risk_->check(1, Side::BUY, 1, 10000, 10010), RiskResult::RATE_BREACH);

    risk_->reset_rate_counters();
    EXPECT_EQ(risk_->check(1, Side::BUY, 1, 10000, 10010), RiskResult::PASS);
}

TEST_F(PreTradeRiskTest, UpdatesGlobalPosition) {
    (void)risk_->check(1, Side::BUY, 100, 10000, 10010);

    int64_t global_pos = global_positions_[1].net_position.load();
    EXPECT_EQ(global_pos, 100);

    (void)risk_->check(1, Side::SELL, 50, 10000, 10010);
    global_pos = global_positions_[1].net_position.load();
    EXPECT_EQ(global_pos, 50);
}

TEST_F(PreTradeRiskTest, SkipsFatFingerWhenNoMid) {
    // mid_price = 0 => skip fat finger check
    RiskResult result = risk_->check(1, Side::BUY, 100, 99999, 0);
    EXPECT_EQ(result, RiskResult::PASS);
}
