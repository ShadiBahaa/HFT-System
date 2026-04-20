// =============================================================================
// Market-replay divergence test (§14.3).
//
// Records a strategy's decision stream during a "live" run (synthetic ticks
// fed through OrderBook → MarketMaker), then replays the SAME tick stream
// from disk and verifies the strategy emits an identical decision sequence.
//
// Catches regressions in:
//   - MarketMaker deterministic output for identical inputs
//   - MarketDataRecorder / MarketDataPlayer round-trip fidelity
//   - OrderBook::apply determinism under replay
//
// If this test ever diverges, SOMETHING on the hot path has grown hidden
// state (uninitialised field, wall-clock sample, RNG without a seed, etc.).
// =============================================================================
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/market_data.h"
#include "feed/order_book.h"
#include "persistence/market_data_recorder.h"
#include "strategy/strategies/market_maker.h"

using namespace hft::core;
using namespace hft::feed;
using namespace hft::persistence;
using namespace hft::strategy;

namespace {

    // Recorded decision — what the strategy chose to do for each tick.
    struct Decision {
        uint64_t        sequence;
        Action          action;
        Side            side;
        Price           price;
        Quantity        quantity;

        bool operator==(const Decision& o) const noexcept {
            return sequence == o.sequence
                && action   == o.action
                && side     == o.side
                && price    == o.price
                && quantity == o.quantity;
        }
    };

    MarketUpdate make_add(uint64_t seq, Side side, Price px, Quantity qty,
                          OrderId ref) {
        MarketUpdate u{};
        u.timestamp     = 1'000'000'000ULL + seq * 1'000ULL;
        u.instrument_id = 1;
        u.type          = UpdateType::ADD;
        u.side          = side;
        u.price         = px;
        u.quantity      = qty;
        u.order_ref     = ref;
        u.sequence      = seq;
        return u;
    }

    // Deterministic tick generator — same inputs → same output every run.
    std::vector<MarketUpdate> generate_tick_stream(size_t N) {
        std::vector<MarketUpdate> v;
        v.reserve(N);
        OrderId ref = 1000;
        for (size_t i = 0; i < N; ++i) {
            Side s = (i & 1) ? Side::BUY : Side::SELL;
            // Tight spread, mild random walk driven purely by i
            Price base = 10'000;
            Price px = (s == Side::BUY)
                ? base - 5 - static_cast<Price>(i % 8)
                : base + 5 + static_cast<Price>(i % 8);
            v.push_back(make_add(static_cast<uint64_t>(i + 1), s, px, 100, ref++));
        }
        return v;
    }

    MarketMaker::Config fresh_cfg() {
        MarketMaker::Config cfg{};
        cfg.instrument_id    = 1;
        cfg.base_qty         = 100;
        cfg.max_position     = 1'000;
        cfg.min_spread       = 10;
        cfg.max_spread       = 100;
        cfg.skew_factor      = 0.5;
        cfg.volatility_scale = 1.0;
        return cfg;
    }

    // Run MarketMaker against a tick stream, collecting its decisions.
    std::vector<Decision>
    drive_strategy(const std::vector<MarketUpdate>& ticks) {
        OrderBook<> book{};
        MarketMaker mm(fresh_cfg());
        mm.initialize();

        std::vector<Decision> out;
        out.reserve(ticks.size());

        for (const auto& u : ticks) {
            book.apply(u);
            if (book.best_bid() == 0 || book.best_ask() == 0) continue;

            BookSignal bs{};
            bs.instrument_id = u.instrument_id;
            bs.best_bid      = book.best_bid();
            bs.best_ask      = book.best_ask();
            bs.mid_price     = (bs.best_bid + bs.best_ask) / 2;
            bs.spread        = bs.best_ask - bs.best_bid;
            bs.timestamp     = u.timestamp;

            Signal s = mm.on_book_update(u.instrument_id, bs);
            out.push_back({u.sequence, s.action, s.side, s.price, s.quantity});
        }
        return out;
    }

    std::string tmp_capture_path() {
        namespace fs = std::filesystem;
        auto p = fs::temp_directory_path() / "hft_replay_divergence.mdr";
        std::error_code ec;
        fs::remove(p, ec);
        return p.string();
    }

} // namespace

// -----------------------------------------------------------------------------
// Test: record once, replay once, compare decision-by-decision.
// -----------------------------------------------------------------------------
TEST(MarketReplayDivergence, StrategyIsBitExactUnderReplay) {
    constexpr size_t N = 500;

    auto ticks = generate_tick_stream(N);
    auto path  = tmp_capture_path();

    // --- Phase A: "live" run. Record every tick as it's consumed. ---
    {
        MarketDataRecorder rec;
        ASSERT_TRUE(rec.open(path.c_str()));
        for (const auto& u : ticks) ASSERT_TRUE(rec.record(u));
        rec.close();
    }
    auto decisions_live = drive_strategy(ticks);

    // --- Phase B: replay from disk, feed fresh strategy instance. ---
    std::vector<MarketUpdate> replayed;
    replayed.reserve(N);
    {
        MarketDataPlayer player;
        ASSERT_TRUE(player.open(path.c_str()));
        MarketUpdate u{};
        while (player.next(u)) replayed.push_back(u);
    }

    ASSERT_EQ(replayed.size(), ticks.size())
        << "recorder/player lost or duplicated ticks";

    // Tick stream must round-trip bit-exact
    for (size_t i = 0; i < ticks.size(); ++i) {
        EXPECT_EQ(replayed[i].sequence,      ticks[i].sequence) << "i=" << i;
        EXPECT_EQ(replayed[i].price,         ticks[i].price)    << "i=" << i;
        EXPECT_EQ(replayed[i].quantity,      ticks[i].quantity) << "i=" << i;
        EXPECT_EQ(replayed[i].order_ref,     ticks[i].order_ref)<< "i=" << i;
    }

    auto decisions_replay = drive_strategy(replayed);

    // --- Phase C: decisions must match 1:1. ---
    ASSERT_EQ(decisions_replay.size(), decisions_live.size())
        << "strategy emitted a different number of decisions under replay";

    for (size_t i = 0; i < decisions_live.size(); ++i) {
        EXPECT_EQ(decisions_replay[i], decisions_live[i])
            << "divergence at decision " << i
            << " tick seq=" << decisions_live[i].sequence;
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// -----------------------------------------------------------------------------
// Test: two back-to-back "live" runs with fresh state must also match — this
// guards against the strategy picking up hidden state from static storage.
// -----------------------------------------------------------------------------
TEST(MarketReplayDivergence, StrategyIsDeterministicAcrossInstances) {
    auto ticks = generate_tick_stream(200);
    auto a = drive_strategy(ticks);
    auto b = drive_strategy(ticks);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i], b[i]) << "non-deterministic at " << i;
    }
}
