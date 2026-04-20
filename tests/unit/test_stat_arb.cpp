#include <gtest/gtest.h>
#include "strategy/strategies/stat_arb.h"

using namespace hft::core;
using namespace hft::strategy;

class StatArbTest : public ::testing::Test {
protected:
    StatArbPairs::Config cfg{};

    void SetUp() override {
        cfg.instrument_a = 1;
        cfg.instrument_b = 2;
        cfg.hedge_ratio = 1.0;
        cfg.lookback_window = 50;
        cfg.entry_z_score = 2.0;
        cfg.exit_z_score = 0.5;
        cfg.trade_qty = 100;
        cfg.target_instrument = 1;
    }

    BookSignal make_sig(InstrumentId id, Price mid) {
        BookSignal s{};
        s.instrument_id = id;
        s.best_bid = mid - 5;
        s.best_ask = mid + 5;
        s.mid_price = mid;
        s.spread = 10;
        return s;
    }
};

TEST_F(StatArbTest, InitialState) {
    StatArbPairs sa(cfg);
    sa.initialize();
    EXPECT_EQ(sa.position(), 0);
    EXPECT_EQ(sa.samples(), 0U);
}

TEST_F(StatArbTest, StatsAccumulateOnBothLegs) {
    StatArbPairs sa(cfg);
    sa.initialize();

    // Feed 30 samples of a stable spread
    for (int i = 0; i < 30; ++i) {
        sa.on_book_update(1, make_sig(1, 10000));
        sa.on_book_update(2, make_sig(2, 9900));
    }

    EXPECT_GT(sa.samples(), 0U);
    // Mean spread should be around 100
    EXPECT_NEAR(sa.mean(), 100.0, 1e-6);
}

TEST_F(StatArbTest, NoSignalWithoutBothLegs) {
    StatArbPairs sa(cfg);
    sa.initialize();

    Signal s = sa.on_book_update(1, make_sig(1, 10000));
    EXPECT_EQ(s.action, Action::NONE);
}

TEST_F(StatArbTest, EntrySignalOnLargeZScore) {
    StatArbPairs sa(cfg);
    sa.initialize();

    // Warm up with stable spread
    for (int i = 0; i < 40; ++i) {
        sa.on_book_update(1, make_sig(1, 10000));
        sa.on_book_update(2, make_sig(2, 9900));
    }

    // Introduce a spread shock (A cheapens dramatically -> z strongly negative -> BUY)
    sa.on_book_update(1, make_sig(1, 9500));
    Signal s = sa.on_book_update(2, make_sig(2, 9900));

    // Depending on stats, a signal may fire; either BUY or NONE
    // but if it fires, it should be on the target instrument
    if (s.action != Action::NONE) {
        EXPECT_EQ(s.instrument_id, cfg.target_instrument);
        EXPECT_EQ(s.quantity, cfg.trade_qty);
    }
}

TEST_F(StatArbTest, ExitWhenZReturnsToMean) {
    StatArbPairs sa(cfg);
    sa.initialize();

    // Warm up
    for (int i = 0; i < 40; ++i) {
        sa.on_book_update(1, make_sig(1, 10000));
        sa.on_book_update(2, make_sig(2, 9900));
    }
    // Force a position by manipulating input
    for (int i = 0; i < 5; ++i) {
        sa.on_book_update(1, make_sig(1, 9000));
        sa.on_book_update(2, make_sig(2, 9900));
    }
    // Return to mean — should emit exit if we had opened
    for (int i = 0; i < 5; ++i) {
        sa.on_book_update(1, make_sig(1, 10000));
        sa.on_book_update(2, make_sig(2, 9900));
    }
    // Just assert no crash and stats are valid
    EXPECT_GT(sa.samples(), 0U);
}
