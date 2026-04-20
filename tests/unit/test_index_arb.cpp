#include <gtest/gtest.h>
#include "strategy/strategies/index_arb.h"

using namespace hft::core;
using namespace hft::strategy;

using IA = IndexArb<8>;

class IndexArbTest : public ::testing::Test {
protected:
    IA::Config cfg{};

    void SetUp() override {
        cfg.etf_instrument_id = 100;
        cfg.num_constituents = 3;
        cfg.constituent_ids = {1, 2, 3, 0, 0, 0, 0, 0};
        cfg.constituent_weights = {0.5, 0.3, 0.2, 0, 0, 0, 0, 0};
        cfg.basis_threshold = 20;
        cfg.trade_qty = 100;
    }

    BookSignal sig(InstrumentId id, Price mid) {
        BookSignal s{};
        s.instrument_id = id;
        s.best_bid = mid - 5;
        s.best_ask = mid + 5;
        s.mid_price = mid;
        return s;
    }
};

TEST_F(IndexArbTest, InitialFairValueZero) {
    IA a(cfg);
    a.initialize();
    EXPECT_EQ(a.fair_value(), 0);
}

TEST_F(IndexArbTest, FairValueFromConstituents) {
    IA a(cfg);
    a.initialize();
    // Warm all three constituents: 0.5*1000 + 0.3*2000 + 0.2*5000 = 2100
    a.on_book_update(1, sig(1, 1000));
    a.on_book_update(2, sig(2, 2000));
    a.on_book_update(3, sig(3, 5000));
    EXPECT_EQ(a.fair_value(), 2100);
}

TEST_F(IndexArbTest, NoSignalBelowThreshold) {
    IA a(cfg);
    a.initialize();
    a.on_book_update(1, sig(1, 1000));
    a.on_book_update(2, sig(2, 2000));
    a.on_book_update(3, sig(3, 5000));
    // ETF at exactly fair value
    Signal s = a.on_book_update(100, sig(100, 2100));
    EXPECT_EQ(s.action, Action::NONE);
}

TEST_F(IndexArbTest, SellSignalWhenETFRich) {
    IA a(cfg);
    a.initialize();
    a.on_book_update(1, sig(1, 1000));
    a.on_book_update(2, sig(2, 2000));
    a.on_book_update(3, sig(3, 5000));
    // ETF 50 points above fair -> sell
    Signal s = a.on_book_update(100, sig(100, 2150));
    EXPECT_EQ(s.action, Action::SELL);
    EXPECT_EQ(s.instrument_id, cfg.etf_instrument_id);
    EXPECT_EQ(s.quantity, cfg.trade_qty);
}

TEST_F(IndexArbTest, BuySignalWhenETFCheap) {
    IA a(cfg);
    a.initialize();
    a.on_book_update(1, sig(1, 1000));
    a.on_book_update(2, sig(2, 2000));
    a.on_book_update(3, sig(3, 5000));
    Signal s = a.on_book_update(100, sig(100, 2050));
    EXPECT_EQ(s.action, Action::BUY);
}
