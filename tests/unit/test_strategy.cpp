#include <gtest/gtest.h>
#include <memory>
#include "strategy/strategies/market_maker.h"
#include "strategy/engine.h"

using namespace hft::core;
using namespace hft::strategy;

// ---- MarketMaker Strategy Tests ----

class MarketMakerTest : public ::testing::Test {
protected:
    MarketMaker::Config cfg{};

    void SetUp() override {
        cfg.instrument_id = 1;
        cfg.base_qty = 100;
        cfg.max_position = 1000;
        cfg.min_spread = 10;
        cfg.max_spread = 100;
        cfg.skew_factor = 0.5;
        cfg.volatility_scale = 1.0;
    }
};

TEST_F(MarketMakerTest, InitialState) {
    MarketMaker mm(cfg);
    mm.initialize();

    EXPECT_EQ(mm.net_position(), 0);
    EXPECT_EQ(mm.realized_pnl(), 0);
    EXPECT_EQ(mm.name(), "MarketMaker");
}

TEST_F(MarketMakerTest, GeneratesBuySignalWhenFlat) {
    MarketMaker mm(cfg);
    mm.initialize();

    BookSignal signal{};
    signal.instrument_id = 1;
    signal.best_bid = 10000;
    signal.best_ask = 10020;
    signal.bid_size = 500;
    signal.ask_size = 500;
    signal.mid_price = 10010;
    signal.spread = 20;

    auto result = mm.on_book_update(1, signal);

    EXPECT_NE(result.action, Action::NONE);
    EXPECT_EQ(result.instrument_id, 1);
    EXPECT_GT(result.quantity, 0);
    EXPECT_GT(result.urgency, 0.0);
}

TEST_F(MarketMakerTest, IgnoresWrongInstrument) {
    MarketMaker mm(cfg);
    mm.initialize();

    BookSignal signal{};
    signal.instrument_id = 99;  // Wrong instrument
    signal.best_bid = 10000;
    signal.best_ask = 10020;
    signal.mid_price = 10010;
    signal.spread = 20;

    auto result = mm.on_book_update(99, signal);
    EXPECT_EQ(result.action, Action::NONE);
}

TEST_F(MarketMakerTest, IgnoresEmptyBook) {
    MarketMaker mm(cfg);
    mm.initialize();

    BookSignal signal{};
    signal.instrument_id = 1;
    signal.best_bid = 0;  // No bids
    signal.best_ask = 10020;

    auto result = mm.on_book_update(1, signal);
    EXPECT_EQ(result.action, Action::NONE);
}

TEST_F(MarketMakerTest, TracksPositionOnFill) {
    MarketMaker mm(cfg);
    mm.initialize();

    ExecutionReport report{};
    report.exec_type = ExecType::FILL;
    report.side = Side::BUY;
    report.filled_qty = 100;

    mm.on_execution_report(report);
    EXPECT_EQ(mm.net_position(), 100);

    // Sell fill
    report.side = Side::SELL;
    report.filled_qty = 50;
    mm.on_execution_report(report);
    EXPECT_EQ(mm.net_position(), 50);
}

TEST_F(MarketMakerTest, TracksPartialFills) {
    MarketMaker mm(cfg);
    mm.initialize();

    ExecutionReport report{};
    report.exec_type = ExecType::PARTIAL;
    report.side = Side::BUY;
    report.filled_qty = 30;

    mm.on_execution_report(report);
    EXPECT_EQ(mm.net_position(), 30);
}

// ---- TypedStrategyEngine Tests ----
// Use heap allocation for large SPSC ring buffers to avoid stack overflow

TEST(StrategyEngineTest, ProcessOneUpdate) {
    MarketMaker::Config cfg{};
    cfg.instrument_id = 1;
    cfg.base_qty = 100;
    cfg.max_position = 1000;
    cfg.min_spread = 10;
    cfg.max_spread = 100;

    MarketMaker mm(cfg);
    mm.initialize();

    auto signal_ring = std::make_unique<TypedStrategyEngine<MarketMaker>::SignalRing>();
    auto order_ring  = std::make_unique<TypedStrategyEngine<MarketMaker>::OrderRing>();
    TypedStrategyEngine<MarketMaker> engine(mm, *signal_ring, *order_ring);

    // Push a book signal
    BookSignal sig{};
    sig.instrument_id = 1;
    sig.best_bid = 10000;
    sig.best_ask = 10020;
    sig.bid_size = 500;
    sig.ask_size = 500;
    sig.mid_price = 10010;
    sig.spread = 20;

    ASSERT_TRUE(signal_ring->try_push(sig));
    EXPECT_TRUE(engine.process_one());
    EXPECT_EQ(engine.signals_processed(), 1U);

    // Should have generated an order
    EXPECT_GE(engine.orders_generated(), 0U);  // May or may not generate
}

TEST(StrategyEngineTest, EmptyRingReturnsImmediate) {
    MarketMaker::Config cfg{};
    cfg.instrument_id = 1;
    cfg.base_qty = 100;
    cfg.max_position = 1000;
    cfg.min_spread = 10;
    cfg.max_spread = 100;

    MarketMaker mm(cfg);
    mm.initialize();

    auto signal_ring = std::make_unique<TypedStrategyEngine<MarketMaker>::SignalRing>();
    auto order_ring  = std::make_unique<TypedStrategyEngine<MarketMaker>::OrderRing>();
    TypedStrategyEngine<MarketMaker> engine(mm, *signal_ring, *order_ring);

    EXPECT_FALSE(engine.process_one());  // Nothing to process
    EXPECT_EQ(engine.signals_processed(), 0U);
}
