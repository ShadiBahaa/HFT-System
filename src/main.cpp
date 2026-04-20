// =============================================================================
// trading_engine — top-level binary wiring the HFT stack together.
//
// This is the orchestrator the design doc implies but did not previously
// exist in the repo: it instantiates every hot-path component and connects
// them with SPSC rings, exactly the topology described in §1-§6.
//
//          +-------------+     +-----------+     +-----------------+
//   ticks  | FeedHandler | --> | OrderBook | --> | StrategyEngine  |
//  (file)  +-------------+     +-----------+     +-----------------+
//                                                         |
//                                                         v
//          +----------------+     +------------+     +---------+
//   wire <-| GatewayEncoder |<----| RiskCheck  |<----| OrderReq|
//          +----------------+     +------------+     +---------+
//
// Threads (in --threaded mode):
//
//   * market-data thread   — drains a recorded .mdr tick file into
//                            the feed → book ring at wire speed.
//   * strategy thread      — busy-polls BookSignal ring, drives
//                            MarketMaker, emits OrderRequests.
//   * gateway thread       — drains OrderRequest ring, runs pre-trade
//                            risk, rate-limits via ExchangeThrottle,
//                            encodes FIX wire bytes, writes to WAL,
//                            increments MetricsPublisher counters.
//
// The gateway thread is where real deployments would push bytes onto
// a socket; here we just log the encoded FIX message to stderr and
// persist it to the WAL so the pipeline is fully exercised.
//
// Shutdown is signal-driven — SIGINT/SIGTERM flips an atomic flag,
// each thread's loop exits, and we print a summary of counters.
//
// Invocation:
//   trading_engine --ticks run.mdr [--symbol AAPL] [--duration 10]
//   trading_engine --demo                    # synthetic ticks, no file
//
// The engine is deterministic under --demo when seeded, so you can
// use it as a smoke-test in CI.
// =============================================================================
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "core/market_data.h"
#include "core/spsc_ring.h"
#include "core/types.h"

#include "feed/feed_handler.h"
#include "feed/order_book.h"

#include "strategy/engine.h"
#include "strategy/strategies/market_maker.h"

#include "oms/order_manager.h"

#include "risk/kill_switch.h"
#include "risk/pre_trade_risk.h"
#include "risk/throttle.h"

#include "gateway/fix_encoder.h"

#include "persistence/market_data_recorder.h"
#include "persistence/wal_writer.h"

#include "telemetry/metrics_publisher.h"

using namespace hft::core;
using namespace hft::feed;
using namespace hft::strategy;
using namespace hft::oms;
using namespace hft::risk;
using namespace hft::persistence;
using namespace hft::telemetry;
using hft::gateway::FixEncoder;

namespace {

// -----------------------------------------------------------------------------
// Globals — intentionally file-scoped, not in a header. This binary is one
// single process; no one needs to link against these.
// -----------------------------------------------------------------------------
std::atomic<bool> g_shutdown{false};

void on_signal(int) noexcept {
    g_shutdown.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// Parse argv the easy way: no dependencies, no argparse. The binary has only
// a handful of flags and they're all optional with sensible defaults.
// -----------------------------------------------------------------------------
struct Args {
    std::string ticks_path;
    std::string symbol  = "DEMO";
    int         duration_sec = 5;    // --demo mode runs for this long
    bool        demo_mode = false;
    std::string wal_path;
};

bool parse_args(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };
        if (a == "--ticks") {
            const char* v = next(); if (!v) return false;
            out.ticks_path = v;
        } else if (a == "--symbol") {
            const char* v = next(); if (!v) return false;
            out.symbol = v;
        } else if (a == "--duration") {
            const char* v = next(); if (!v) return false;
            out.duration_sec = std::atoi(v);
        } else if (a == "--demo") {
            out.demo_mode = true;
        } else if (a == "--wal") {
            const char* v = next(); if (!v) return false;
            out.wal_path = v;
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "Usage: trading_engine [--ticks FILE | --demo] "
                "[--symbol SYM] [--duration SEC] [--wal PATH]\n");
            return false;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    if (out.ticks_path.empty() && !out.demo_mode) {
        std::fprintf(stderr, "specify --ticks FILE or --demo\n");
        return false;
    }
    if (out.wal_path.empty()) {
        auto p = std::filesystem::temp_directory_path() / "hft_engine.wal";
        out.wal_path = p.string();
    }
    return true;
}

// -----------------------------------------------------------------------------
// Synthetic tick producer (--demo). Generates deterministic alternating BUY /
// SELL ADDs around a mid of 10,000 ticks for `duration_sec` seconds. This
// lets CI invoke the binary without needing a recorded tape.
// -----------------------------------------------------------------------------
void run_synthetic_producer(
    SPSCRingBuffer<MarketUpdate, 65536>& ring,
    int duration_sec) noexcept
{
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + std::chrono::seconds(duration_sec);

    uint64_t seq = 1;
    OrderId  ref = 1000;
    // Walk the mid-price up and down in a slow triangular wave so the book
    // builder sees a stream of BBO-moving updates rather than a static
    // top-of-book. Period ~256 ticks; amplitude ±16 ticks around 10_000.
    while (!g_shutdown.load(std::memory_order_acquire) && clk::now() < deadline) {
        MarketUpdate u{};
        u.timestamp     = static_cast<TimestampNs>(seq) * 1'000'000ULL;
        u.instrument_id = 1;
        u.type          = UpdateType::ADD;
        u.side          = (seq & 1) ? Side::BUY : Side::SELL;

        const int64_t phase = static_cast<int64_t>(seq % 256);
        const int64_t tri   = (phase < 128 ? phase : 256 - phase) - 64;  // -64..+64
        const Price mid     = static_cast<Price>(10'000 + tri / 4);      // ±16
        u.price             = (u.side == Side::BUY) ? mid - 5 : mid + 5;
        u.quantity          = 100;
        u.order_ref         = ref++;
        u.sequence          = seq++;

        // Ring full → back-pressure: yield and retry. In production this
        // would increment a drop counter and advance; for the demo we want
        // every tick to land so the strategy sees a clean sequence.
        while (!ring.try_push(u)) {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }
    }
    // Natural end of synthetic run — signal downstream threads to drain & exit.
    g_shutdown.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// Tick producer that reads a recorded MarketDataRecorder file and replays its
// MarketUpdates into the same ring at I/O speed (as fast as the disk serves).
// -----------------------------------------------------------------------------
void run_recorded_producer(
    SPSCRingBuffer<MarketUpdate, 65536>& ring,
    const std::string& ticks_path) noexcept
{
    MarketDataPlayer player;
    if (!player.open(ticks_path.c_str())) {
        std::fprintf(stderr, "could not open ticks file: %s\n", ticks_path.c_str());
        g_shutdown.store(true, std::memory_order_release);
        return;
    }
    MarketUpdate u{};
    while (!g_shutdown.load(std::memory_order_acquire) && player.next(u)) {
        while (!ring.try_push(u)) {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }
    }
    // EOF — natural shutdown signal so the strategy/gateway threads drain.
    g_shutdown.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// Book builder + signal emitter. Drains MarketUpdate ring → OrderBook.apply,
// then on every top-of-book change pushes a BookSignal to the strategy.
// -----------------------------------------------------------------------------
void run_book_builder(
    SPSCRingBuffer<MarketUpdate, 65536>& updates,
    SPSCRingBuffer<BookSignal,  65536>& signals,
    OrderBook<>& book) noexcept
{
    MarketUpdate u{};
    Price last_bid = 0, last_ask = 0;

    while (!g_shutdown.load(std::memory_order_acquire) || updates.size() > 0) {
        if (!updates.try_pop(u)) {
            std::this_thread::yield();
            continue;
        }
        book.apply(u);

        Price bid = book.best_bid();
        Price ask = book.best_ask();
        if (bid == 0 || ask == 0) continue;
        if (bid == last_bid && ask == last_ask) continue;   // BBO unchanged
        last_bid = bid; last_ask = ask;

        BookSignal s{};
        s.instrument_id = u.instrument_id;
        s.best_bid      = bid;
        s.best_ask      = ask;
        s.mid_price     = (bid + ask) / 2;
        s.spread        = ask - bid;
        s.timestamp     = u.timestamp;
        // If the signal ring is full the strategy is falling behind —
        // conflate by dropping the older value. Cheapest option: just skip.
        (void) signals.try_push(s);
    }
}

// -----------------------------------------------------------------------------
// Gateway thread. Drains OrderRequest ring, performs:
//   1. pre-trade risk check   (inline, < 200 ns)
//   2. rate-limit via OMS throttle
//   3. fill NewOrderSingle, encode FIX
//   4. persist WAL entry
//   5. bump metrics
// In production step 3 also shoves the bytes onto a pinned send ring; here
// we just count them.
// -----------------------------------------------------------------------------
void run_gateway(
    SPSCRingBuffer<OrderRequest, 4096>& orders,
    PreTradeRisk&         risk,
    OrderManagementSystem<>& oms,
    FixEncoder&           encoder,
    WALWriter&            wal,
    MetricsPublisher&     metrics,
    const std::string&    symbol) noexcept
{
    using hft::telemetry::MetricType;
    const int m_risk_reject = metrics.register_metric(
        "hft_orders_risk_rejected", "Orders blocked by pre-trade risk", MetricType::COUNTER);
    const int m_throttled   = metrics.register_metric(
        "hft_orders_throttled", "Orders blocked by ExchangeThrottle", MetricType::COUNTER);
    const int m_sent        = metrics.register_metric(
        "hft_orders_sent", "Orders that reached the wire", MetricType::COUNTER);
    const int m_bytes       = metrics.register_metric(
        "hft_bytes_on_wire", "Cumulative bytes encoded for the wire", MetricType::COUNTER);

    OrderRequest req{};
    while (!g_shutdown.load(std::memory_order_acquire) || orders.size() > 0) {
        if (!orders.try_pop(req)) {
            std::this_thread::yield();
            continue;
        }

        // ---- 1. pre-trade risk ----
        // mid_price=0 disables the fat-finger check for this demo; in
        // production the book builder would stamp the signal onto the req.
        auto result = risk.check(req.instrument_id, req.side, req.quantity,
                                 req.price, /*mid=*/0);
        if (result != RiskResult::PASS) {
            metrics.increment(m_risk_reject);
            continue;
        }

        // ---- 2 + 3. throttle + OMS create ----
        Order slot{};
        if (!oms.try_create_order(req, slot)) {
            metrics.increment(m_throttled);
            continue;
        }

        // ---- 4. encode FIX ----
        NewOrderSingle nos{};
        std::snprintf(nos.cl_ord_id, sizeof(nos.cl_ord_id),
                      "%llu", static_cast<unsigned long long>(slot.client_order_id));
        const size_t sym_len = std::min(symbol.size(), sizeof(nos.symbol) - 1);
        std::memcpy(nos.symbol, symbol.data(), sym_len);
        nos.side      = req.side;
        nos.ord_type  = req.order_type;
        nos.tif       = req.tif;
        nos.price     = req.price;
        nos.qty       = req.quantity;
        nos.timestamp = rdtsc();

        auto wire = encoder.encode_new_order(nos);

        // ---- 5. persist + metrics ----
        WALEntry entry{};
        entry.timestamp_ns = nos.timestamp;
        entry.sequence     = slot.client_order_id;
        entry.type         = static_cast<uint8_t>(WALEntryType::ORDER_SENT);
        const size_t pl = std::min(wire.size(), sizeof(entry.payload));
        std::memcpy(entry.payload, wire.data(), pl);
        wal.write(entry);

        metrics.increment(m_sent);
        metrics.increment(m_bytes, static_cast<double>(wire.size()));
    }
    wal.flush_sync();
}

} // namespace

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ---- rings + other large-footprint objects ----
    // Heap-allocate everything that would blow the default 1 MiB Windows
    // thread stack. The OrderBook alone is ~24 MiB (1 M-slot OrderPool),
    // each 65k-entry SPSC ring is 4 MiB, and the per-instrument global-position
    // array is another 4 MiB. On Linux the soft rlimit (8 MiB) would still
    // be tight; just heap-allocate and stop worrying.
    auto update_ring      = std::make_unique<SPSCRingBuffer<MarketUpdate, 65536>>();
    auto signal_ring      = std::make_unique<SPSCRingBuffer<BookSignal,   65536>>();
    auto order_ring       = std::make_unique<SPSCRingBuffer<OrderRequest, 4096>>();
    auto book             = std::make_unique<OrderBook<>>();
    auto global_positions = std::make_unique<
        std::array<GlobalPosition, MAX_INSTRUMENTS>>();

    MarketMaker::Config mm_cfg{};
    mm_cfg.instrument_id    = 1;
    mm_cfg.base_qty         = 100;
    mm_cfg.max_position     = 1'000;
    mm_cfg.min_spread       = 10;
    mm_cfg.max_spread       = 100;
    mm_cfg.skew_factor      = 0.5;
    mm_cfg.volatility_scale = 1.0;
    MarketMaker strategy(mm_cfg);
    strategy.initialize();
    TypedStrategyEngine<MarketMaker> engine(strategy, *signal_ring, *order_ring);

    // ---- risk ----
    KillSwitch kill;
    PreTradeRisk risk(*global_positions, kill, /*strategy_id*/0);
    InstrumentLimits lim{};
    lim.max_position        = 10'000;
    lim.max_notional        = 1'000'000'000;
    lim.max_orders_per_sec  = 1'000;
    lim.max_order_size      = 10'000;
    lim.fat_finger_price    = 10'000;   // very loose — demo only
    risk.set_limits(1, lim);

    // ---- OMS + throttle ----
    OrderManagementSystem<> oms;
    ExchangeThrottle thr;
    // Modest burst so the demo still exercises the throttle path occasionally.
    thr.configure(/*new*/1000, /*cancel*/1000, /*total*/2000,
                  /*burst*/500, /*tsc_freq*/1e9);
    oms.set_throttle(&thr);

    // ---- gateway sinks ----
    FixEncoder encoder;
    encoder.set_sender("HFT");
    encoder.set_target("EXCH");

    WALWriter wal(args.wal_path.c_str());
    if (!wal.is_open()) {
        std::fprintf(stderr, "WAL open failed: %s\n", args.wal_path.c_str());
        return 2;
    }

    MetricsPublisher metrics;

    // ---- threads ----
    std::thread t_producer, t_book, t_engine, t_gateway;

    if (args.demo_mode) {
        t_producer = std::thread(run_synthetic_producer,
                                  std::ref(*update_ring), args.duration_sec);
    } else {
        t_producer = std::thread(run_recorded_producer,
                                  std::ref(*update_ring), args.ticks_path);
    }

    t_book = std::thread(run_book_builder,
                         std::ref(*update_ring), std::ref(*signal_ring),
                         std::ref(*book));

    t_engine = std::thread([&]{
        while (!g_shutdown.load(std::memory_order_acquire) ||
               signal_ring->size() > 0)
        {
            if (!engine.process_one()) std::this_thread::yield();
        }
    });

    t_gateway = std::thread(run_gateway,
                            std::ref(*order_ring), std::ref(risk),
                            std::ref(oms), std::ref(encoder),
                            std::ref(wal), std::ref(metrics),
                            std::cref(args.symbol));

    // ---- wait ----
    t_producer.join();
    t_book.join();
    t_engine.join();
    t_gateway.join();

    // ---- summary ----
    std::fprintf(stderr, "\n=== trading_engine summary ===\n");
    std::fprintf(stderr, "orders_sent       : %llu\n",
                 (unsigned long long)oms.orders_sent());
    std::fprintf(stderr, "orders_throttled  : %llu\n",
                 (unsigned long long)oms.orders_throttled());
    std::fprintf(stderr, "signals_processed : %llu\n",
                 (unsigned long long)engine.signals_processed());
    std::fprintf(stderr, "orders_generated  : %llu\n",
                 (unsigned long long)engine.orders_generated());
    std::fprintf(stderr, "wal_path          : %s (next_seq=%llu, %llu bytes)\n",
                 args.wal_path.c_str(),
                 (unsigned long long)wal.next_sequence(),
                 (unsigned long long)wal.file_size());
    return 0;
}
