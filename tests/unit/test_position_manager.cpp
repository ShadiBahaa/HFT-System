#include <gtest/gtest.h>
#include "risk/position_manager.h"

using namespace hft::core;
using namespace hft::risk;

using PM = PositionManager<256>;

namespace {
    ExecutionReport make_fill(InstrumentId id, Side side, Price px, Quantity qty) {
        ExecutionReport er{};
        er.exec_type = ExecType::FILL;
        er.instrument_id = id;
        er.side = side;
        er.price = px;
        er.filled_qty = qty;
        return er;
    }
}

TEST(PositionManagerTest, EmptySnapshot) {
    PM pm;
    auto snap = pm.snapshot();
    EXPECT_TRUE(snap.empty());
    auto p = pm.get(5);
    EXPECT_EQ(p.fill_count, 0U);
}

TEST(PositionManagerTest, SingleBuyFill) {
    PM pm;
    pm.on_fill(make_fill(5, Side::BUY, 10000, 100));
    auto p = pm.get(5);
    EXPECT_EQ(p.net_quantity, 100);
    EXPECT_EQ(p.avg_entry_price, 10000);
    EXPECT_EQ(p.fill_count, 1U);
    EXPECT_EQ(p.realized_pnl, 0);
}

TEST(PositionManagerTest, AverageEntryPriceOnAdd) {
    PM pm;
    pm.on_fill(make_fill(1, Side::BUY, 10000, 100));
    pm.on_fill(make_fill(1, Side::BUY, 12000, 100));
    auto p = pm.get(1);
    EXPECT_EQ(p.net_quantity, 200);
    EXPECT_EQ(p.avg_entry_price, 11000);  // (100*10000 + 100*12000) / 200
}

TEST(PositionManagerTest, CloseLongRealizesPnl) {
    PM pm;
    pm.on_fill(make_fill(1, Side::BUY, 10000, 100));
    pm.on_fill(make_fill(1, Side::SELL, 11000, 100));
    auto p = pm.get(1);
    EXPECT_EQ(p.net_quantity, 0);
    EXPECT_EQ(p.realized_pnl, 100 * 1000);  // 100 @ (11000-10000)
}

TEST(PositionManagerTest, CloseShortRealizesPnl) {
    PM pm;
    pm.on_fill(make_fill(1, Side::SELL, 10000, 100));
    pm.on_fill(make_fill(1, Side::BUY, 9000, 100));
    auto p = pm.get(1);
    EXPECT_EQ(p.net_quantity, 0);
    EXPECT_EQ(p.realized_pnl, 100 * 1000);  // Short @ 10000, cover @ 9000
}

TEST(PositionManagerTest, PartialClosePartialOpen) {
    PM pm;
    pm.on_fill(make_fill(1, Side::BUY, 10000, 100));
    pm.on_fill(make_fill(1, Side::SELL, 11000, 50));
    auto p = pm.get(1);
    EXPECT_EQ(p.net_quantity, 50);
    EXPECT_EQ(p.realized_pnl, 50 * 1000);
}

TEST(PositionManagerTest, MultipleInstruments) {
    PM pm;
    pm.on_fill(make_fill(1, Side::BUY, 10000, 100));
    pm.on_fill(make_fill(2, Side::SELL, 20000, 50));
    pm.on_fill(make_fill(3, Side::BUY, 5000, 200));

    auto snap = pm.snapshot();
    EXPECT_EQ(snap.size(), 3U);
    EXPECT_EQ(pm.get(1).net_quantity, 100);
    EXPECT_EQ(pm.get(2).net_quantity, -50);
    EXPECT_EQ(pm.get(3).net_quantity, 200);
}

TEST(PositionManagerTest, Reset) {
    PM pm;
    pm.on_fill(make_fill(1, Side::BUY, 10000, 100));
    pm.reset(1);
    auto p = pm.get(1);
    EXPECT_EQ(p.net_quantity, 0);
    EXPECT_EQ(p.fill_count, 0U);
}

TEST(PositionManagerTest, TotalAggregates) {
    PM pm;
    pm.on_fill(make_fill(1, Side::BUY, 10000, 100));
    pm.on_fill(make_fill(2, Side::SELL, 20000, 50));
    int64_t gross = pm.total_gross_notional();
    // 100*10000 + 50*20000
    EXPECT_EQ(gross, 2'000'000);
}
