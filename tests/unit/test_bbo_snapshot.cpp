#include <gtest/gtest.h>
#include <memory>
#include "feed/bbo_snapshot.h"

using namespace hft::core;
using namespace hft::feed;

using Snap = BBOSnapshot<1024>;

TEST(BBOSnapshotTest, InitiallyZero) {
    auto snap = std::make_unique<Snap>();
    EXPECT_EQ(snap->best_bid(5), 0);
    EXPECT_EQ(snap->best_ask(5), 0);
    EXPECT_EQ(snap->mid(5), 0);
    EXPECT_EQ(snap->last_update(5), 0U);
}

TEST(BBOSnapshotTest, UpdateAndRead) {
    auto snap = std::make_unique<Snap>();
    snap->update(10, 9995, 10005, 100, 200, 1000);
    EXPECT_EQ(snap->best_bid(10), 9995);
    EXPECT_EQ(snap->best_ask(10), 10005);
    EXPECT_EQ(snap->mid(10), 10000);
    EXPECT_EQ(snap->last_update(10), 1000U);
}

TEST(BBOSnapshotTest, OutOfRangeSafe) {
    auto snap = std::make_unique<Snap>();
    constexpr InstrumentId oor = 2000;  // > capacity (1024) but fits in uint16_t
    snap->update(oor, 1, 2, 3, 4, 5);    // beyond capacity
    EXPECT_EQ(snap->best_bid(oor), 0);
}

TEST(BBOSnapshotTest, Prewarm) {
    auto snap = std::make_unique<Snap>();
    snap->prewarm_pages();  // should not crash
    snap->update(100, 1000, 1010, 10, 20, 42);
    EXPECT_EQ(snap->best_bid(100), 1000);
}

TEST(BBOSnapshotTest, Clear) {
    auto snap = std::make_unique<Snap>();
    snap->update(1, 100, 110, 10, 20, 5);
    snap->clear();
    EXPECT_EQ(snap->best_bid(1), 0);
    EXPECT_EQ(snap->last_update(1), 0U);
}

TEST(BBOSnapshotTest, Alignment) {
    auto snap = std::make_unique<Snap>();
    EXPECT_EQ(reinterpret_cast<uintptr_t>(snap.get()) % 4096, 0U);
}

TEST(BBOSnapshotTest, MidZeroIfEitherSideZero) {
    auto snap = std::make_unique<Snap>();
    snap->update(5, 0, 100, 0, 10, 1);
    EXPECT_EQ(snap->mid(5), 0);
}
