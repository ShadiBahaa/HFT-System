#include <gtest/gtest.h>
#include <thread>
#include <cstdlib>
#include "core/platform.h"

// Portable aligned allocation wrapper (mingw libstdc++ lacks std::aligned_alloc)
static inline void* hft_aligned_alloc(size_t alignment, size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}

static inline void hft_aligned_free(void* p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

using namespace hft::core;

TEST(PlatformTest, CpuCountPositive) {
    EXPECT_GT(cpu_count(), 0);
}

TEST(PlatformTest, CurrentCoreInRange) {
    int core = current_core();
    EXPECT_GE(core, 0);
    EXPECT_LT(core, cpu_count());
}

TEST(PlatformTest, PinCurrentThread) {
    // Pin to core 0 (always exists)
    bool ok = pin_current_thread(0);
    EXPECT_TRUE(ok);
    // Verify we're on core 0
    EXPECT_EQ(current_core(), 0);
}

TEST(PlatformTest, PrewarmPages) {
    // Allocate a buffer and pre-warm it
    constexpr size_t size = 64 * 1024;  // 64KB
    auto* buf = static_cast<char*>(hft_aligned_alloc(4096, size));
    ASSERT_NE(buf, nullptr);

    // Should not crash or throw
    prewarm_pages(buf, size);

    hft_aligned_free(buf);
}

TEST(PlatformTest, LockUnlockMemory) {
    constexpr size_t size = 4096;
    auto* buf = static_cast<char*>(hft_aligned_alloc(4096, size));
    ASSERT_NE(buf, nullptr);

    // Lock may fail without privileges, but should not crash
    lock_memory(buf, size);
    unlock_memory(buf, size);

    hft_aligned_free(buf);
}
