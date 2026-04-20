#include <gtest/gtest.h>
#include "gateway/connection_manager.h"

using namespace hft::gateway;

TEST(ExchangeConnectionTest, InitialState) {
    ExchangeConnection conn;
    EXPECT_EQ(conn.state(), ConnState::DISCONNECTED);
    EXPECT_EQ(conn.retry_count(), 0U);
    EXPECT_EQ(conn.expected_seq_in(), 1U);
    EXPECT_EQ(conn.next_seq_out(), 1U);
    EXPECT_FALSE(conn.is_active());
}

TEST(ExchangeConnectionTest, NormalConnectFlow) {
    ExchangeConnection conn;

    EXPECT_TRUE(conn.connect());
    EXPECT_EQ(conn.state(), ConnState::CONNECTING);

    EXPECT_TRUE(conn.on_connected());
    EXPECT_EQ(conn.state(), ConnState::LOGGING_ON);

    EXPECT_TRUE(conn.on_logon_accepted());
    EXPECT_EQ(conn.state(), ConnState::ACTIVE);
    EXPECT_TRUE(conn.is_active());
}

TEST(ExchangeConnectionTest, DisconnectAndRetry) {
    ExchangeConnection conn;
    conn.connect();
    conn.on_connected();
    conn.on_logon_accepted();

    conn.on_disconnect();
    EXPECT_EQ(conn.state(), ConnState::DISCONNECTED);
    EXPECT_EQ(conn.retry_count(), 1U);
    EXPECT_TRUE(conn.can_retry());
    EXPECT_EQ(conn.next_retry_delay_ms(), 100U);

    // Second disconnect
    conn.connect();
    conn.on_connected();
    conn.on_logon_accepted();
    conn.on_disconnect();
    EXPECT_EQ(conn.retry_count(), 2U);
    EXPECT_EQ(conn.next_retry_delay_ms(), 500U);

    // Third disconnect
    conn.connect();
    conn.on_connected();
    conn.on_logon_accepted();
    conn.on_disconnect();
    EXPECT_EQ(conn.retry_count(), 3U);
    EXPECT_EQ(conn.next_retry_delay_ms(), 2000U);
    EXPECT_FALSE(conn.can_retry());
}

TEST(ExchangeConnectionTest, MaxRetriesTriggersCallback) {
    ExchangeConnection conn;
    bool kill_triggered = false;
    bool alert_triggered = false;

    conn.set_disconnect_handler([&] { kill_triggered = true; });
    conn.set_alert_handler([&](const char*) { alert_triggered = true; });

    // Exhaust retries
    for (uint32_t i = 0; i < ExchangeConnection::MAX_RETRIES; ++i) {
        conn.connect();
        conn.on_connected();
        conn.on_logon_accepted();
        conn.on_disconnect();
    }

    EXPECT_FALSE(kill_triggered);  // Not yet at max

    // One more triggers escalation
    conn.on_disconnect();
    EXPECT_TRUE(kill_triggered);
    EXPECT_TRUE(alert_triggered);
}

TEST(ExchangeConnectionTest, SequenceGapTransition) {
    ExchangeConnection conn;
    conn.connect();
    conn.on_connected();
    conn.on_logon_accepted();

    EXPECT_TRUE(conn.on_sequence_gap(10, 5));
    EXPECT_EQ(conn.state(), ConnState::RESENDING);

    EXPECT_TRUE(conn.on_resend_complete());
    EXPECT_EQ(conn.state(), ConnState::ACTIVE);
}

TEST(ExchangeConnectionTest, SequenceTracking) {
    ExchangeConnection conn;
    conn.connect();
    conn.on_connected();
    conn.on_logon_accepted();

    // Normal sequence
    EXPECT_TRUE(conn.check_seq_in(1));
    EXPECT_TRUE(conn.check_seq_in(2));
    EXPECT_TRUE(conn.check_seq_in(3));
    EXPECT_EQ(conn.expected_seq_in(), 4U);

    // Gap — jumps from 3 to 7
    EXPECT_TRUE(conn.check_seq_in(7));
    EXPECT_EQ(conn.state(), ConnState::RESENDING);

    // Duplicate
    EXPECT_FALSE(conn.check_seq_in(2));  // old
}

TEST(ExchangeConnectionTest, AllocateSeqOut) {
    ExchangeConnection conn;
    EXPECT_EQ(conn.allocate_seq_out(), 1U);
    EXPECT_EQ(conn.allocate_seq_out(), 2U);
    EXPECT_EQ(conn.allocate_seq_out(), 3U);
}

TEST(ExchangeConnectionTest, DrainAndDisconnect) {
    ExchangeConnection conn;
    conn.connect();
    conn.on_connected();
    conn.on_logon_accepted();

    EXPECT_TRUE(conn.drain());
    EXPECT_EQ(conn.state(), ConnState::DRAINING);

    EXPECT_TRUE(conn.disconnect());
    EXPECT_EQ(conn.state(), ConnState::DISCONNECTING);

    conn.on_disconnected();
    EXPECT_EQ(conn.state(), ConnState::DISCONNECTED);
}

TEST(ExchangeConnectionTest, Reset) {
    ExchangeConnection conn;
    conn.connect();
    conn.on_connected();
    conn.on_logon_accepted();
    conn.allocate_seq_out();
    conn.check_seq_in(1);

    conn.reset();
    EXPECT_EQ(conn.state(), ConnState::DISCONNECTED);
    EXPECT_EQ(conn.retry_count(), 0U);
    EXPECT_EQ(conn.expected_seq_in(), 1U);
    EXPECT_EQ(conn.next_seq_out(), 1U);
}

TEST(ConnStateTest, ToString) {
    EXPECT_STREQ(to_string(ConnState::DISCONNECTED), "DISCONNECTED");
    EXPECT_STREQ(to_string(ConnState::ACTIVE), "ACTIVE");
    EXPECT_STREQ(to_string(ConnState::RESENDING), "RESENDING");
}
