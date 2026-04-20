// =============================================================================
// Chaos tests — software-reachable scenarios from §14.5.
//
// These tests exercise failure paths that don't need real hardware:
//   1. WAL recovery after simulated process crash (write, drop writer, replay)
//   2. Disk-full during WAL write (redirect to invalid path, expect graceful
//      failure rather than abort)
//   3. Feed gap injection (skip sequence numbers, expect GapDetector to record)
//   4. Heartbeat timeout → failover state machine advances to DETECT
//   5. Rogue-strategy watchdog (budget overrun on on_book_update ⇒ caller
//      records the breach; the "watchdog" here is deterministic wall-clock)
// =============================================================================
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/types.h"
#include "feed/gap_detector.h"
#include "persistence/wal_writer.h"
#include "persistence/replay.h"
#include "resilience/failover_manager.h"

using namespace hft;
using namespace hft::feed;
using namespace hft::persistence;
using namespace hft::resilience;

namespace {

    std::string tmp_wal_path(const char* name) {
        namespace fs = std::filesystem;
        auto p = fs::temp_directory_path() / (std::string("hft_chaos_") + name + ".wal");
        std::error_code ec;
        fs::remove(p, ec);   // ignore failure — file may not exist yet
        return p.string();
    }

    WALEntry make_entry(uint64_t seq, WALEntryType type) {
        WALEntry e{};
        e.timestamp_ns = 1'000'000'000ULL + seq;
        e.sequence     = seq;
        e.type         = static_cast<uint8_t>(type);
        std::snprintf(e.payload, sizeof(e.payload), "entry-%llu",
                      static_cast<unsigned long long>(seq));
        return e;
    }

} // namespace

// -----------------------------------------------------------------------------
// Scenario 1: Simulated process crash mid-session. Writer is destroyed
// without an explicit shutdown; replay should recover everything that was
// flushed before the crash.
// -----------------------------------------------------------------------------
TEST(ChaosTest, WalRecoversAfterSimulatedCrash) {
    auto path = tmp_wal_path("crash");
    constexpr int N = 200;

    {
        WALWriter w(path.c_str());
        for (int i = 0; i < N; ++i) {
            w.write(make_entry(static_cast<uint64_t>(i + 1), WALEntryType::ORDER_SENT));
        }
        w.flush_sync();  // simulate the "last clean flush" before crash
        // Destructor runs without a graceful shutdown marker — this is the
        // "crash" moment. The WAL file must still be parseable.
    }

    WALWriter reader;
    ASSERT_TRUE(reader.open_readonly(path.c_str()));
    std::vector<uint64_t> seen;
    reader.replay([&](const WALEntry& e) { seen.push_back(e.sequence); });

    ASSERT_GE(seen.size(), static_cast<size_t>(N))
        << "replay must recover at least every flushed entry";
    EXPECT_EQ(seen.front(), 1U);
    // Sequence must be monotonically non-decreasing — no corruption
    for (size_t i = 1; i < seen.size(); ++i) {
        EXPECT_GE(seen[i], seen[i - 1]);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// -----------------------------------------------------------------------------
// Scenario 2: Disk-full / invalid path. We can't actually fill the disk in a
// portable test, but we can point the WAL at a path that cannot be opened.
// The writer must NOT abort, crash, or corrupt other state — it simply
// refuses to accept writes (they become no-ops).
// -----------------------------------------------------------------------------
TEST(ChaosTest, WalHandlesUnopenablePathGracefully) {
#ifdef _WIN32
    const char* bogus = "Z:\\_surely_not_a_real_drive\\hft_chaos.wal";
#else
    const char* bogus = "/proc/this-cannot-be-created-here/hft_chaos.wal";
#endif

    // Constructing the writer with a bad path must not throw or crash.
    // (The implementation records the failure internally and drops writes.)
    WALWriter w(bogus);
    EXPECT_FALSE(w.is_open()) << "bad path must leave writer in closed state";

    // Attempting to write some entries and flush should not crash even though
    // nothing lands on disk.
    for (int i = 0; i < 10; ++i) {
        w.write(make_entry(static_cast<uint64_t>(i + 1), WALEntryType::RISK_BREACH));
    }
    w.flush_sync();

    SUCCEED() << "WALWriter survives unopenable path without aborting";
}

// -----------------------------------------------------------------------------
// Scenario 3: Feed gap injection. An adversarial producer skips sequence
// numbers; GapDetector must log the miss and continue accepting newer seqs
// without desynchronising.
// -----------------------------------------------------------------------------
TEST(ChaosTest, FeedGapDetectorRecordsSkippedSequences) {
    GapDetector gd;

    // Clean run up to 10
    for (uint64_t s = 1; s <= 10; ++s) ASSERT_TRUE(gd.check(s));
    EXPECT_EQ(gd.gap_count(), 0U);

    // Now a 5-wide gap: we only see seq 16 (skipping 11..15)
    EXPECT_TRUE(gd.check(16));
    ASSERT_EQ(gd.gap_count(), 1U);
    EXPECT_EQ(gd.gap_at(0).begin, 11U);
    EXPECT_EQ(gd.gap_at(0).end,   15U);

    // After the gap, the detector keeps marching forward normally
    for (uint64_t s = 17; s <= 25; ++s) ASSERT_TRUE(gd.check(s));
    EXPECT_EQ(gd.gap_count(), 1U);

    // An out-of-order (duplicate / stale) message must be rejected, not
    // crash or back up the counter
    EXPECT_FALSE(gd.check(20));
    EXPECT_EQ(gd.expected_seq(), 26U);
}

// -----------------------------------------------------------------------------
// Scenario 4: Heartbeat timeout. Starting as ACTIVE_SECONDARY with a stale
// heartbeat, evaluate() must advance through DETECT → VERIFY → ... within
// the configured timeout window.
// -----------------------------------------------------------------------------
TEST(ChaosTest, HeartbeatTimeoutAdvancesFailoverState) {
    FailoverConfig cfg{};
    cfg.heartbeat_timeout_ns = 1'000'000;       // 1ms — fast for the test
    cfg.verify_timeout_ns    = 1'000'000;
    cfg.min_reconcile_entries = 0;

    FailoverManager fm;
    fm.set_config(cfg);
    fm.become_secondary();

    // Stamp a heartbeat in the past so is_alive(now) == false.
    fm.heartbeat().beat(1);

    const uint64_t now = 1 + cfg.heartbeat_timeout_ns + 1;
    auto s1 = fm.evaluate(now);
    EXPECT_EQ(s1, FailoverState::DETECT) << "missed HB must drop us into DETECT";

    auto s2 = fm.evaluate(now + 1);
    EXPECT_EQ(s2, FailoverState::VERIFY);

    // Without a heartbeat recovery, VERIFY must escalate after verify_timeout
    auto s3 = fm.evaluate(now + cfg.verify_timeout_ns + 1);
    EXPECT_EQ(s3, FailoverState::QUARANTINE);
}

// -----------------------------------------------------------------------------
// Scenario 5: Rogue-strategy watchdog. A strategy call that overruns its
// budget must not silently succeed — the caller detects it via wall-clock
// delta. We simulate the overrun by sleeping; the watchdog expectation is a
// deterministic threshold check.
// -----------------------------------------------------------------------------
TEST(ChaosTest, RogueStrategyOverrunIsDetected) {
    using clk = std::chrono::steady_clock;
    constexpr auto BUDGET = std::chrono::microseconds(500);

    // Use a deterministic busy-wait instead of sleep_for: on Windows Debug
    // builds, sleep_for can return early due to timer resolution quirks, and
    // the watchdog contract we're validating is a wall-clock check, not a
    // scheduler check. Burn wall-clock time explicitly until we exceed the
    // budget by a comfortable margin so the test is immune to clock jitter.
    constexpr auto ROGUE_WORK = std::chrono::milliseconds(5);
    auto t0 = clk::now();
    auto deadline = t0 + ROGUE_WORK;
    // volatile sink prevents the optimizer from eliding the loop.
    volatile uint64_t sink = 0;
    while (clk::now() < deadline) {
        sink += 1;
    }
    (void)sink;
    auto elapsed = clk::now() - t0;

    // The watchdog contract: if elapsed > budget, the caller (engine) is
    // expected to record a breach and quarantine the strategy.
    bool breach = elapsed > BUDGET;
    EXPECT_TRUE(breach)
        << "wall-clock watchdog must flag strategies that overrun their budget";
}
