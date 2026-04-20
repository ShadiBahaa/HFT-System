// Simple hot-path microbenchmark. Builds with the rest of the project but is
// NOT wired into ctest. Run manually:
//
//     ./build/tests/benchmarks/hft_benchmarks
//
// Reports p50/p99/p999/max wall-clock nanoseconds per call. Not a substitute
// for Google Benchmark — just enough to detect regressions locally.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "core/clock.h"
#include "core/market_data.h"
#include "feed/order_book.h"
#include "risk/kill_switch.h"
#include "risk/pre_trade_risk.h"
#include "strategy/strategies/market_maker.h"

using namespace hft::core;
using namespace hft::feed;
using namespace hft::risk;
using namespace hft::strategy;

namespace {

    constexpr int WARMUP     = 1'000;
    constexpr int ITERATIONS = 50'000;

    uint64_t percentile(std::vector<uint64_t>& v, double p) {
        if (v.empty()) return 0;
        size_t idx = static_cast<size_t>(p * v.size());
        if (idx >= v.size()) idx = v.size() - 1;
        std::nth_element(v.begin(), v.begin() + idx, v.end());
        return v[idx];
    }

    void report(const char* name, std::vector<uint64_t>& samples) {
        uint64_t p50  = percentile(samples, 0.50);
        uint64_t p99  = percentile(samples, 0.99);
        uint64_t p999 = percentile(samples, 0.999);
        uint64_t max  = 0;
        for (auto v : samples) if (v > max) max = v;
        std::printf("%-30s p50=%6llu p99=%6llu p999=%6llu max=%6llu ns  (N=%zu)\n",
                    name,
                    static_cast<unsigned long long>(p50),
                    static_cast<unsigned long long>(p99),
                    static_cast<unsigned long long>(p999),
                    static_cast<unsigned long long>(max),
                    samples.size());
    }

    MarketUpdate make_add(uint64_t seq, Side side, Price px, Quantity qty, OrderId ref) {
        MarketUpdate u{};
        u.timestamp     = seq;
        u.instrument_id = 1;
        u.type          = UpdateType::ADD;
        u.side          = side;
        u.price         = px;
        u.quantity      = qty;
        u.order_ref     = ref;
        u.sequence      = seq;
        return u;
    }

} // namespace

int main() {
    WallClock clock;

    // ----- Order book apply -----
    {
        OrderBook<> book{};
        OrderId ref = 1;
        for (int i = 0; i < WARMUP; ++i) {
            book.apply(make_add(static_cast<uint64_t>(i), Side::BUY,
                                10000 - (i % 10), 100, ref++));
        }
        std::vector<uint64_t> samples;
        samples.reserve(ITERATIONS);
        for (int i = 0; i < ITERATIONS; ++i) {
            auto u = make_add(static_cast<uint64_t>(WARMUP + i),
                              (i & 1) ? Side::BUY : Side::SELL,
                              10000 + (i & 7), 100, ref++);
            auto t0 = clock.now_ns();
            book.apply(u);
            auto t1 = clock.now_ns();
            samples.push_back(t1 - t0);
        }
        report("OrderBook::apply", samples);
    }

    // ----- Strategy on_book_update -----
    {
        MarketMaker::Config cfg{};
        cfg.instrument_id    = 1;
        cfg.base_qty         = 100;
        cfg.max_position     = 1000;
        cfg.min_spread       = 10;
        cfg.max_spread       = 100;
        cfg.skew_factor      = 0.5;
        cfg.volatility_scale = 1.0;
        MarketMaker mm(cfg);
        mm.initialize();

        BookSignal bs{};
        bs.instrument_id = 1;
        bs.best_bid = 9990; bs.best_ask = 10010;
        bs.bid_size = 300;  bs.ask_size = 300;
        bs.mid_price = 10000; bs.spread = 20;

        std::vector<uint64_t> samples;
        samples.reserve(ITERATIONS);
        for (int i = 0; i < WARMUP; ++i) (void)mm.on_book_update(1, bs);
        for (int i = 0; i < ITERATIONS; ++i) {
            bs.mid_price = 10000 + (i & 15);
            auto t0 = clock.now_ns();
            auto s = mm.on_book_update(1, bs);
            auto t1 = clock.now_ns();
            (void)s;
            samples.push_back(t1 - t0);
        }
        report("MarketMaker::on_book_update", samples);
    }

    // ----- Pre-trade risk -----
    {
        auto global_positions = std::make_unique<
            std::array<GlobalPosition, MAX_INSTRUMENTS>>();
        KillSwitch kill_switch;
        PreTradeRisk risk(*global_positions, kill_switch, /*strategy_id=*/0);

        InstrumentLimits lim{};
        lim.max_position        = 1'000'000;
        lim.max_order_size      = 10'000;
        lim.max_notional        = 1'000'000'000;
        lim.fat_finger_price    = 10'000;
        lim.max_orders_per_sec  = 100'000;
        risk.set_limits(1, lim);

        std::vector<uint64_t> samples;
        samples.reserve(ITERATIONS);
        for (int i = 0; i < WARMUP; ++i) (void)risk.check(1, Side::BUY, 100, 10000, 10000);
        for (int i = 0; i < ITERATIONS; ++i) {
            auto t0 = clock.now_ns();
            auto r = risk.check(1, Side::BUY, 100, 10000, 10000);
            auto t1 = clock.now_ns();
            (void)r;
            samples.push_back(t1 - t0);
        }
        report("PreTradeRisk::check", samples);
    }

    return 0;
}
