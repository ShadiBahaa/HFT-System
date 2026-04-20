#include <gtest/gtest.h>
#include "oms/smart_router.h"
#include "oms/order_manager.h"
#include "risk/throttle.h"

using namespace hft::core;
using namespace hft::oms;
using hft::risk::ExchangeThrottle;

// ---- SmartRouter Tests ----

TEST(SmartRouterTest, SelectsBestVenue) {
    SmartRouter router;
    router.set_venue_count(3);

    VenueMetrics v0{};
    v0.best_ask = 10020;
    v0.ask_size = 500;
    v0.latency_p50_ns = 5000;
    v0.taker_fee_bps = 30;
    v0.active = true;

    VenueMetrics v1{};
    v1.best_ask = 10010;   // Better price
    v1.ask_size = 500;
    v1.latency_p50_ns = 3000;
    v1.taker_fee_bps = 25;
    v1.active = true;

    VenueMetrics v2{};
    v2.best_ask = 10005;   // Best price
    v2.ask_size = 500;
    v2.latency_p50_ns = 2000;
    v2.taker_fee_bps = 20;
    v2.active = true;

    router.update_venue(0, v0);
    router.update_venue(1, v1);
    router.update_venue(2, v2);

    VenueId selected = router.select_venue(Side::BUY, 10050, 100);
    EXPECT_EQ(selected, static_cast<VenueId>(3));  // Venue index 2 => VenueId 3
}

TEST(SmartRouterTest, SkipsInactiveVenue) {
    SmartRouter router;
    router.set_venue_count(2);

    VenueMetrics v0{};
    v0.best_ask = 10000;
    v0.ask_size = 500;
    v0.active = false;  // Inactive

    VenueMetrics v1{};
    v1.best_ask = 10020;
    v1.ask_size = 500;
    v1.latency_p50_ns = 1000;
    v1.taker_fee_bps = 10;
    v1.active = true;

    router.update_venue(0, v0);
    router.update_venue(1, v1);

    VenueId selected = router.select_venue(Side::BUY, 10050, 100);
    EXPECT_EQ(selected, static_cast<VenueId>(2));  // Only venue 1 available
}

TEST(SmartRouterTest, SkipsInsufficientLiquidity) {
    SmartRouter router;
    router.set_venue_count(2);

    VenueMetrics v0{};
    v0.best_ask = 10000;
    v0.ask_size = 10;    // Not enough
    v0.active = true;

    VenueMetrics v1{};
    v1.best_ask = 10020;
    v1.ask_size = 500;
    v1.latency_p50_ns = 1000;
    v1.taker_fee_bps = 10;
    v1.active = true;

    router.update_venue(0, v0);
    router.update_venue(1, v1);

    VenueId selected = router.select_venue(Side::BUY, 10050, 100);
    EXPECT_EQ(selected, static_cast<VenueId>(2));  // v0 has insufficient size
}

TEST(SmartRouterTest, ReturnsUnknownWhenNoVenue) {
    SmartRouter router;
    router.set_venue_count(0);

    VenueId selected = router.select_venue(Side::BUY, 10000, 100);
    EXPECT_EQ(selected, VenueId::UNKNOWN);
}

// ---- OrderManagementSystem Tests ----

TEST(OMSTest, CreateOrder) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.action = Action::BUY;
    req.side = Side::BUY;
    req.order_type = OrderType::LIMIT;
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order order = oms.create_order(req);

    EXPECT_EQ(order.client_order_id, 1U);
    EXPECT_EQ(order.instrument_id, 1);
    EXPECT_EQ(order.side, Side::BUY);
    EXPECT_EQ(order.price, 10000);
    EXPECT_EQ(order.quantity, 100);
    EXPECT_EQ(order.leaves_qty, 100);
    EXPECT_EQ(order.filled_qty, 0);
    EXPECT_EQ(order.status, OrderStatus::PENDING_NEW);

    EXPECT_EQ(oms.orders_sent(), 1U);
}

TEST(OMSTest, AssignsIncrementingIds) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order o1 = oms.create_order(req);
    Order o2 = oms.create_order(req);
    Order o3 = oms.create_order(req);

    EXPECT_EQ(o1.client_order_id, 1U);
    EXPECT_EQ(o2.client_order_id, 2U);
    EXPECT_EQ(o3.client_order_id, 3U);
}

TEST(OMSTest, FindOrder) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order order = oms.create_order(req);
    const Order* found = oms.find_order(order.client_order_id);

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->instrument_id, 1);
    EXPECT_EQ(found->price, 10000);
}

TEST(OMSTest, HandleNewAck) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order order = oms.create_order(req);

    ExecutionReport report{};
    report.cl_ord_id = order.client_order_id;
    report.order_id = 55555;
    report.exec_type = ExecType::NEW;

    oms.on_execution_report(report);

    const Order* found = oms.find_order(order.client_order_id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->status, OrderStatus::OPEN);
    EXPECT_EQ(found->exchange_order_id, 55555U);
}

TEST(OMSTest, HandleFill) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order order = oms.create_order(req);

    ExecutionReport report{};
    report.cl_ord_id = order.client_order_id;
    report.exec_type = ExecType::FILL;
    report.filled_qty = 100;
    report.leaves_qty = 0;

    oms.on_execution_report(report);

    const Order* found = oms.find_order(order.client_order_id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->status, OrderStatus::FILLED);
    EXPECT_EQ(found->filled_qty, 100);
    EXPECT_EQ(found->leaves_qty, 0);
    EXPECT_EQ(oms.orders_filled(), 1U);
}

TEST(OMSTest, HandlePartialFill) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order order = oms.create_order(req);

    ExecutionReport report{};
    report.cl_ord_id = order.client_order_id;
    report.exec_type = ExecType::PARTIAL;
    report.filled_qty = 30;
    report.leaves_qty = 70;

    oms.on_execution_report(report);

    const Order* found = oms.find_order(order.client_order_id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->status, OrderStatus::PARTIALLY_FILLED);
    EXPECT_EQ(found->filled_qty, 30);
    EXPECT_EQ(found->leaves_qty, 70);
}

TEST(OMSTest, HandleCancel) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order order = oms.create_order(req);

    ExecutionReport report{};
    report.cl_ord_id = order.client_order_id;
    report.exec_type = ExecType::CANCELLED;

    oms.on_execution_report(report);

    const Order* found = oms.find_order(order.client_order_id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->status, OrderStatus::CANCELLED);
    EXPECT_EQ(oms.orders_cancelled(), 1U);
}

// ---- OMS + ExchangeThrottle integration ----

TEST(OMSTest, ThrottleBlocksExcessNewOrders) {
    OrderManagementSystem<> oms;
    ExchangeThrottle thr;
    // 3 tokens burst, refill so slowly the test never sees a second drop-in.
    thr.configure(/*new*/1, /*cancel*/1, /*total*/10, /*burst*/3,
                  /*tsc_freq*/1e9);
    oms.set_throttle(&thr);

    OrderRequest req{};
    req.side = Side::BUY;
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order slot{};
    // First 3 fit in the burst; 4th must be throttled.
    EXPECT_TRUE (oms.try_create_order(req, slot));
    EXPECT_TRUE (oms.try_create_order(req, slot));
    EXPECT_TRUE (oms.try_create_order(req, slot));
    EXPECT_FALSE(oms.try_create_order(req, slot))
        << "throttle must stop us after burst is exhausted";

    EXPECT_EQ(oms.orders_sent(),      3U);
    EXPECT_EQ(oms.orders_throttled(), 1U);
}

TEST(OMSTest, ThrottleBlocksExcessCancels) {
    OrderManagementSystem<> oms;
    ExchangeThrottle thr;
    // burst=10 so that the messages_total_ bucket (2*burst=20) comfortably
    // covers the 3 new + 3 cancels we attempt; cancel bucket is the bottleneck.
    thr.configure(/*new*/100, /*cancel*/1, /*total*/1000, /*burst*/10,
                  /*tsc_freq*/1e9);
    oms.set_throttle(&thr);
    // Reduce the cancel bucket down to 2 tokens to be the test's bottleneck.
    // (Done by draining — cancel bucket starts at burst_limit=10.)
    for (int i = 0; i < 8; ++i) (void) thr.allow_cancel();

    OrderRequest req{};
    req.side = Side::BUY;
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order o1{}, o2{}, o3{};
    ASSERT_TRUE(oms.try_create_order(req, o1));
    ASSERT_TRUE(oms.try_create_order(req, o2));
    ASSERT_TRUE(oms.try_create_order(req, o3));

    // Burst = 2 → two cancels allowed, third must be throttled.
    EXPECT_TRUE (oms.try_cancel(o1.client_order_id));
    EXPECT_TRUE (oms.try_cancel(o2.client_order_id));
    EXPECT_FALSE(oms.try_cancel(o3.client_order_id));

    EXPECT_EQ(oms.find_order(o1.client_order_id)->status, OrderStatus::PENDING_CANCEL);
    EXPECT_EQ(oms.find_order(o2.client_order_id)->status, OrderStatus::PENDING_CANCEL);
    // The throttled cancel must NOT have flipped the status
    EXPECT_EQ(oms.find_order(o3.client_order_id)->status, OrderStatus::PENDING_NEW);
    EXPECT_GE(oms.orders_throttled(), 1U);
}

TEST(OMSTest, NoThrottleMeansUnlimitedSends) {
    OrderManagementSystem<> oms;
    // No set_throttle() call — sends must never block.

    OrderRequest req{};
    req.side = Side::BUY;
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order slot{};
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(oms.try_create_order(req, slot))
            << "unlimited path must never reject";
    }
    EXPECT_EQ(oms.orders_sent(),      100U);
    EXPECT_EQ(oms.orders_throttled(), 0U);
}

TEST(OMSTest, HandleReject) {
    OrderManagementSystem<> oms;

    OrderRequest req{};
    req.instrument_id = 1;
    req.price = 10000;
    req.quantity = 100;

    Order order = oms.create_order(req);

    ExecutionReport report{};
    report.cl_ord_id = order.client_order_id;
    report.exec_type = ExecType::REJECTED;

    oms.on_execution_report(report);

    const Order* found = oms.find_order(order.client_order_id);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->status, OrderStatus::REJECTED);
    EXPECT_EQ(oms.orders_rejected(), 1U);
}
