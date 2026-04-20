#include <gtest/gtest.h>
#include "resilience/failover_manager.h"

using namespace hft::resilience;

TEST(HeartbeatMonitorTest, IsAliveWithinTimeout) {
    HeartbeatMonitor hb;
    hb.beat(1000);
    EXPECT_TRUE(hb.is_alive(1500, 1000));
    EXPECT_FALSE(hb.is_alive(3000, 1000));
}

TEST(HeartbeatMonitorTest, NoBeatsIsDead) {
    HeartbeatMonitor hb;
    EXPECT_FALSE(hb.is_alive(1000, 1000));
}

TEST(FencingTokenTest, MonotonicAdvance) {
    FencingToken t;
    EXPECT_EQ(t.current(), 0U);
    EXPECT_TRUE(t.advance(5));
    EXPECT_EQ(t.current(), 5U);
    EXPECT_FALSE(t.advance(3));  // Can't go backward
    EXPECT_EQ(t.current(), 5U);
    EXPECT_TRUE(t.advance(10));
    EXPECT_EQ(t.current(), 10U);
}

TEST(FencingTokenTest, FencesOldWriter) {
    FencingToken t;
    t.advance(7);
    EXPECT_TRUE(t.is_fenced(3));
    EXPECT_FALSE(t.is_fenced(7));
    EXPECT_FALSE(t.is_fenced(9));
}

TEST(FailoverManagerTest, PrimaryStaysPrimary) {
    FailoverManager fm;
    fm.become_primary();
    fm.evaluate(1000);
    EXPECT_EQ(fm.state(), FailoverState::ACTIVE_PRIMARY);
}

TEST(FailoverManagerTest, SecondaryDetectsMissedHeartbeat) {
    FailoverManager fm;
    FailoverConfig cfg;
    cfg.heartbeat_timeout_ns = 1000;
    cfg.verify_timeout_ns = 2000;
    fm.set_config(cfg);
    fm.become_secondary();

    fm.heartbeat().beat(100);
    fm.evaluate(500);   // Within timeout
    EXPECT_EQ(fm.state(), FailoverState::ACTIVE_SECONDARY);

    fm.evaluate(2000);  // Missed
    EXPECT_EQ(fm.state(), FailoverState::DETECT);
}

TEST(FailoverManagerTest, FullTransitionToPrimary) {
    FailoverManager fm;
    FailoverConfig cfg;
    cfg.heartbeat_timeout_ns = 1000;
    cfg.verify_timeout_ns = 2000;
    cfg.min_reconcile_entries = 0;
    fm.set_config(cfg);
    fm.become_secondary();

    fm.heartbeat().beat(100);
    fm.evaluate(2000);   // DETECT
    EXPECT_EQ(fm.state(), FailoverState::DETECT);

    fm.evaluate(2100);   // VERIFY
    EXPECT_EQ(fm.state(), FailoverState::VERIFY);

    fm.evaluate(5000);   // QUARANTINE (verify timeout elapsed, still no heartbeat)
    EXPECT_EQ(fm.state(), FailoverState::QUARANTINE);
    EXPECT_EQ(fm.fencing_token().current(), 1U);

    fm.evaluate(5100);   // RECONCILE
    EXPECT_EQ(fm.state(), FailoverState::RECONCILE);

    fm.evaluate(5200);   // RESUME (min_reconcile_entries = 0)
    EXPECT_EQ(fm.state(), FailoverState::RESUME);

    fm.evaluate(5300);   // ACTIVE_PRIMARY
    EXPECT_EQ(fm.state(), FailoverState::ACTIVE_PRIMARY);
}

TEST(FailoverManagerTest, HeartbeatRecoveryAbortsFailover) {
    FailoverManager fm;
    FailoverConfig cfg;
    cfg.heartbeat_timeout_ns = 1000;
    cfg.verify_timeout_ns = 5000;
    fm.set_config(cfg);
    fm.become_secondary();

    fm.heartbeat().beat(100);
    fm.evaluate(2000);   // DETECT
    fm.evaluate(2001);   // VERIFY
    EXPECT_EQ(fm.state(), FailoverState::VERIFY);

    fm.heartbeat().beat(2500);  // Primary recovered
    fm.evaluate(2600);
    EXPECT_EQ(fm.state(), FailoverState::ACTIVE_SECONDARY);
}

TEST(FailoverManagerTest, ReconcileWaitsForEntries) {
    FailoverManager fm;
    FailoverConfig cfg;
    cfg.heartbeat_timeout_ns = 1000;
    cfg.verify_timeout_ns = 1000;
    cfg.min_reconcile_entries = 3;
    fm.set_config(cfg);
    fm.become_secondary();

    fm.heartbeat().beat(100);
    fm.evaluate(2000);   // DETECT
    fm.evaluate(2100);   // VERIFY
    fm.evaluate(4000);   // QUARANTINE
    fm.evaluate(4100);   // RECONCILE
    EXPECT_EQ(fm.state(), FailoverState::RECONCILE);

    // Only 1 entry reconciled — should stay in RECONCILE
    fm.mark_reconciled_entry();
    fm.evaluate(4200);
    EXPECT_EQ(fm.state(), FailoverState::RECONCILE);

    // After all entries → RESUME
    fm.mark_reconciled_entry();
    fm.mark_reconciled_entry();
    fm.evaluate(4300);
    EXPECT_EQ(fm.state(), FailoverState::RESUME);
}
