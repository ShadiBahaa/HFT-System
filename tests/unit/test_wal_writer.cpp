#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "persistence/wal_writer.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <unistd.h>
#endif

using namespace hft::persistence;

class WALWriterTest : public ::testing::Test {
protected:
    // Unique path per test instance to avoid collisions when CTest runs in
    // parallel (multiple WAL tests would otherwise trample test_wal.bin).
    std::string path_storage;
    const char* test_path{nullptr};

    void SetUp() override {
        char buf[128];
#if defined(_WIN32)
        const unsigned pid = static_cast<unsigned>(::GetCurrentProcessId());
#else
        const unsigned pid = static_cast<unsigned>(::getpid());
#endif
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::snprintf(buf, sizeof(buf), "test_wal_%u_%s.bin", pid,
                      info ? info->name() : "x");
        path_storage = buf;
        test_path = path_storage.c_str();
        std::remove(test_path);
    }

    void TearDown() override {
        std::remove(test_path);
    }
};

TEST_F(WALWriterTest, OpenAndClose) {
    WALWriter writer;
    EXPECT_TRUE(writer.open(test_path));
    EXPECT_TRUE(writer.is_open());
    writer.close();
    EXPECT_FALSE(writer.is_open());
}

TEST_F(WALWriterTest, WriteAndFlush) {
    WALWriter writer(test_path);
    ASSERT_TRUE(writer.is_open());

    WALEntry entry{};
    entry.timestamp_ns = 1000;
    entry.type = static_cast<uint8_t>(WALEntryType::ORDER_SENT);
    std::strncpy(entry.payload, "test_order_1", sizeof(entry.payload) - 1);

    writer.write(entry);
    writer.flush_sync();

    EXPECT_EQ(writer.next_sequence(), 2U);  // first entry was seq 1
}

TEST_F(WALWriterTest, WriteAndReplay) {
    // Write entries
    {
        WALWriter writer(test_path);
        ASSERT_TRUE(writer.is_open());

        for (int i = 0; i < 5; ++i) {
            WALEntry entry{};
            entry.timestamp_ns = static_cast<uint64_t>(i * 1000);
            entry.type = static_cast<uint8_t>(WALEntryType::ORDER_SENT);
            std::snprintf(entry.payload, sizeof(entry.payload), "order_%d", i);
            writer.write(entry);
        }
        writer.flush_sync();
    }

    // Replay entries
    WALWriter reader;
    ASSERT_TRUE(reader.open_readonly(test_path));

    std::vector<WALEntry> replayed;
    uint64_t count = reader.replay([&](const WALEntry& e) {
        replayed.push_back(e);
    });

    EXPECT_EQ(count, 5U);
    EXPECT_EQ(replayed.size(), 5U);

    for (size_t i = 0; i < replayed.size(); ++i) {
        EXPECT_EQ(replayed[i].sequence, i + 1);
        EXPECT_EQ(replayed[i].timestamp_ns, i * 1000U);
    }
}

TEST_F(WALWriterTest, SequenceMonotonic) {
    {
        WALWriter writer(test_path);
        ASSERT_TRUE(writer.is_open());

        for (int i = 0; i < 100; ++i) {
            WALEntry entry{};
            entry.timestamp_ns = static_cast<uint64_t>(i);
            entry.type = static_cast<uint8_t>(WALEntryType::POSITION_CHANGE);
            writer.write(entry);
        }
        writer.flush_sync();
    }

    WALWriter reader;
    reader.open_readonly(test_path);

    uint64_t last_seq = 0;
    reader.replay([&](const WALEntry& e) {
        EXPECT_GT(e.sequence, last_seq);
        last_seq = e.sequence;
    });

    EXPECT_EQ(last_seq, 100U);
}

TEST_F(WALWriterTest, EntrySize128Bytes) {
    static_assert(sizeof(WALEntry) == 128);
}
