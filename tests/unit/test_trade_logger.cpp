#include <gtest/gtest.h>
#include "telemetry/trade_logger.h"

using namespace hft::core;
using namespace hft::telemetry;

TEST(TradeLoggerTest, LogOrder) {
    TradeLogger tl(16);
    OrderRequest req{};
    req.client_order_id = 42;
    req.instrument_id = 7;
    req.side = Side::BUY;
    req.price = 10000;
    req.quantity = 100;
    req.timestamp = 1234567;
    tl.log_order(req);

    EXPECT_EQ(tl.size(), 1U);
    auto snap = tl.snapshot();
    ASSERT_EQ(snap.size(), 1U);
    EXPECT_EQ(snap[0].type, TradeEventType::ORDER_SENT);
    EXPECT_EQ(snap[0].cl_ord_id, 42U);
    EXPECT_EQ(snap[0].quantity, 100);
}

TEST(TradeLoggerTest, LogExecutionFill) {
    TradeLogger tl(16);
    ExecutionReport er{};
    er.order_id = 99;
    er.cl_ord_id = 42;
    er.exec_type = ExecType::FILL;
    er.side = Side::SELL;
    er.price = 15000;
    er.filled_qty = 50;
    er.leaves_qty = 0;
    tl.log_execution(er);

    auto snap = tl.snapshot();
    ASSERT_EQ(snap.size(), 1U);
    EXPECT_EQ(snap[0].type, TradeEventType::ORDER_FILLED);
    EXPECT_EQ(snap[0].filled_qty, 50);
    EXPECT_EQ(snap[0].leaves_qty, 0);
}

TEST(TradeLoggerTest, LogCancel) {
    TradeLogger tl(16);
    tl.log_cancel(77, 12345);
    auto snap = tl.snapshot();
    ASSERT_EQ(snap.size(), 1U);
    EXPECT_EQ(snap[0].type, TradeEventType::ORDER_CANCELLED);
    EXPECT_EQ(snap[0].cl_ord_id, 77U);
    EXPECT_EQ(snap[0].timestamp, 12345U);
}

TEST(TradeLoggerTest, RingWraps) {
    TradeLogger tl(4);
    for (uint64_t i = 0; i < 10; ++i) {
        tl.log_cancel(i, i);
    }
    EXPECT_EQ(tl.size(), 4U);
    EXPECT_EQ(tl.total_logged(), 10U);
    EXPECT_TRUE(tl.wrapped());

    auto snap = tl.snapshot();
    ASSERT_EQ(snap.size(), 4U);
    // Oldest retained should be #6
    EXPECT_EQ(snap[0].cl_ord_id, 6U);
    EXPECT_EQ(snap[3].cl_ord_id, 9U);
}

TEST(TradeLoggerTest, CsvExport) {
    TradeLogger tl(4);
    tl.log_cancel(1, 100);
    tl.log_cancel(2, 200);
    std::string csv = tl.to_csv();
    EXPECT_NE(csv.find("timestamp,type,side"), std::string::npos);
    EXPECT_NE(csv.find(",1,"), std::string::npos);
    EXPECT_NE(csv.find(",2,"), std::string::npos);
}

TEST(TradeLoggerTest, Clear) {
    TradeLogger tl(4);
    tl.log_cancel(1, 100);
    tl.log_cancel(2, 200);
    EXPECT_EQ(tl.size(), 2U);
    tl.clear();
    EXPECT_EQ(tl.size(), 0U);
    EXPECT_FALSE(tl.wrapped());
}

TEST(TradeLoggerTest, EventSize128) {
    EXPECT_EQ(sizeof(TradeEvent), 128U);
}
