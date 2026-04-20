#include <gtest/gtest.h>
#include "risk/post_trade_risk.h"

using namespace hft::risk;

class PostTradeRiskTest : public ::testing::Test {
protected:
    PostTradeRisk ptr;
    KillSwitch ks;

    void SetUp() override {
        PostTradeRisk::Config cfg;
        cfg.max_drawdown = 1'000'000;       // $100 in fixed-point
        cfg.var_limit = 10'000'000.0;
        cfg.delta_limit = 50'000;
        cfg.max_gross_notional = 500'000'000;
        ptr.set_config(cfg);
        ptr.set_kill_switch(&ks);
    }
};

TEST_F(PostTradeRiskTest, EmptyPortfolioPassesAllLimits) {
    EXPECT_TRUE(ptr.evaluate());
    auto m = ptr.latest_metrics();
    EXPECT_EQ(m.total_pnl, 0);
    EXPECT_EQ(m.gross_notional, 0);
}

TEST_F(PostTradeRiskTest, UpdateAndCalculateMetrics) {
    PositionSnapshot snap;
    snap.instrument_id = 1;
    snap.net_position = 100;
    snap.mark_price = 15000;
    snap.realized_pnl = 5000;
    snap.unrealized_pnl = 2000;

    ptr.update_position(snap);
    auto m = ptr.calculate_metrics();

    EXPECT_EQ(m.total_realized_pnl, 5000);
    EXPECT_EQ(m.total_unrealized_pnl, 2000);
    EXPECT_EQ(m.total_pnl, 7000);
    EXPECT_EQ(m.net_delta, 100);
    EXPECT_GT(m.gross_notional, 0);
}

TEST_F(PostTradeRiskTest, DrawdownTriggersKillSwitch) {
    // First, establish a high-water mark
    PositionSnapshot snap;
    snap.instrument_id = 1;
    snap.net_position = 100;
    snap.mark_price = 15000;
    snap.realized_pnl = 2'000'000;  // High P&L
    snap.unrealized_pnl = 0;

    ptr.update_position(snap);
    EXPECT_TRUE(ptr.evaluate());

    // Now drop P&L below drawdown threshold
    snap.realized_pnl = 0;
    snap.unrealized_pnl = 0;
    ptr.update_position(snap);
    EXPECT_FALSE(ptr.evaluate());

    // Kill switch should be activated
    EXPECT_TRUE(ks.is_active());
}

TEST_F(PostTradeRiskTest, DeltaLimitBreach) {
    PostTradeRisk::Config cfg;
    cfg.max_drawdown = 0;
    cfg.delta_limit = 1000;
    ptr.set_config(cfg);

    PositionSnapshot snap;
    snap.instrument_id = 1;
    snap.net_position = 1500;  // exceeds 1000 limit
    snap.mark_price = 15000;
    ptr.update_position(snap);

    EXPECT_FALSE(ptr.evaluate());
}

TEST_F(PostTradeRiskTest, MultipleInstruments) {
    PositionSnapshot snap1;
    snap1.instrument_id = 1;
    snap1.net_position = 100;
    snap1.mark_price = 15000;
    snap1.realized_pnl = 1000;

    PositionSnapshot snap2;
    snap2.instrument_id = 2;
    snap2.net_position = -50;
    snap2.mark_price = 20000;
    snap2.realized_pnl = 2000;

    ptr.update_position(snap1);
    ptr.update_position(snap2);
    EXPECT_EQ(ptr.num_instruments(), 2);

    auto m = ptr.calculate_metrics();
    EXPECT_EQ(m.total_realized_pnl, 3000);
    EXPECT_EQ(m.net_delta, 50);  // 100 + (-50)
}

TEST_F(PostTradeRiskTest, UpdateExistingPosition) {
    PositionSnapshot snap;
    snap.instrument_id = 1;
    snap.net_position = 100;
    snap.mark_price = 15000;
    snap.realized_pnl = 1000;

    ptr.update_position(snap);
    EXPECT_EQ(ptr.num_instruments(), 1);

    snap.net_position = 200;
    ptr.update_position(snap);
    EXPECT_EQ(ptr.num_instruments(), 1);  // same instrument, not added again

    auto m = ptr.calculate_metrics();
    EXPECT_EQ(m.net_delta, 200);
}

TEST_F(PostTradeRiskTest, Reset) {
    PositionSnapshot snap;
    snap.instrument_id = 1;
    snap.net_position = 100;
    ptr.update_position(snap);

    ptr.reset();
    EXPECT_EQ(ptr.num_instruments(), 0);
    EXPECT_EQ(ptr.current_drawdown(), 0);
}
