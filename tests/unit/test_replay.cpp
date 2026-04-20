#include <gtest/gtest.h>
#include <vector>
#include "persistence/replay.h"
#include "strategy/strategies/market_maker.h"

using namespace hft::persistence;
using namespace hft::core;
using namespace hft::strategy;

class MarketReplayTest : public ::testing::Test {
protected:
    std::vector<TickRecord> make_tick_data(int count, Price start_bid, Price start_ask) {
        std::vector<TickRecord> ticks;
        for (int i = 0; i < count; ++i) {
            TickRecord r{};
            r.timestamp_ns = static_cast<uint64_t>((i + 1) * 1000);
            r.update.instrument_id = 1;
            r.update.type = UpdateType::ADD;
            r.update.side = (i % 2 == 0) ? Side::BUY : Side::SELL;
            r.update.price = (i % 2 == 0) ? start_bid : start_ask;
            r.update.quantity = 100;
            r.update.order_ref = static_cast<uint64_t>(i + 1);
            ticks.push_back(r);
        }
        return ticks;
    }
};

TEST_F(MarketReplayTest, TickIteratorBasic) {
    auto ticks = make_tick_data(10, 10000, 10020);
    TickIterator it(ticks.data(), ticks.size());

    EXPECT_TRUE(it.valid());
    EXPECT_EQ(it.timestamp(), 1000U);
    EXPECT_EQ(it.total(), 10U);

    ++it;
    EXPECT_EQ(it.timestamp(), 2000U);
}

TEST_F(MarketReplayTest, TickIteratorSeekToStart) {
    auto ticks = make_tick_data(10, 10000, 10020);
    TickIterator it(ticks.data(), ticks.size(), 5000);  // start at 5000ns

    EXPECT_TRUE(it.valid());
    EXPECT_GE(it.timestamp(), 5000U);
}

TEST_F(MarketReplayTest, EmptyTickData) {
    TickIterator it(nullptr, 0);
    EXPECT_FALSE(it.valid());
}

TEST_F(MarketReplayTest, ReplayWithStrategy) {
    auto ticks = make_tick_data(20, 10000, 10020);

    MarketMaker::Config cfg;
    cfg.instrument_id = 1;
    cfg.base_qty = 100;
    cfg.max_position = 1000;
    cfg.min_spread = 10;
    cfg.max_spread = 100;

    MarketMaker mm(cfg);
    mm.initialize();

    MarketReplay replay;
    replay.run(mm, ticks.data(), ticks.size());

    EXPECT_EQ(replay.ticks_replayed(), 20U);
}

TEST_F(MarketReplayTest, ReplayWithTimeRange) {
    auto ticks = make_tick_data(20, 10000, 10020);

    MarketMaker::Config cfg;
    cfg.instrument_id = 1;
    cfg.base_qty = 100;
    cfg.max_position = 1000;
    cfg.min_spread = 10;
    cfg.max_spread = 100;

    MarketMaker mm(cfg);
    mm.initialize();

    ReplayConfig rc;
    rc.start_ns = 5000;
    rc.end_ns = 15000;

    MarketReplay replay;
    replay.run(mm, ticks.data(), ticks.size(), rc);

    EXPECT_GT(replay.ticks_replayed(), 0U);
    EXPECT_LT(replay.ticks_replayed(), 20U);
}

TEST_F(MarketReplayTest, SimulatedClockAdvances) {
    auto ticks = make_tick_data(5, 10000, 10020);

    MarketMaker::Config cfg;
    cfg.instrument_id = 1;
    cfg.base_qty = 100;
    cfg.max_position = 1000;
    cfg.min_spread = 10;
    cfg.max_spread = 100;

    MarketMaker mm(cfg);
    mm.initialize();

    MarketReplay replay;
    replay.run(mm, ticks.data(), ticks.size());

    // Clock should be at last tick timestamp
    EXPECT_EQ(replay.clock().now_ns(), 5000U);
}

TEST_F(MarketReplayTest, LatencyModelDefaultReasonable) {
    LatencyModel model;

    Signal sig;
    sig.action = Action::BUY;
    sig.instrument_id = 1;
    sig.quantity = 100;
    sig.price = 0;  // market order

    hft::feed::OrderBook<> book;
    book.add_order(1, Side::BUY, 10000, 100);
    book.add_order(2, Side::SELL, 10020, 100);

    auto fill = model.simulate_fill(sig, book, 1000);
    EXPECT_TRUE(fill.has_value());
    if (fill) {
        EXPECT_EQ(fill->side, Side::BUY);
        EXPECT_EQ(fill->price, 10020);  // fills at best ask
        EXPECT_EQ(fill->filled_qty, 100);
    }
}
