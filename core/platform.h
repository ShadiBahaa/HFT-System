#pragma once

#include <cstdint>
#include <cstring>
#include <thread>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef DELETE
#undef DELETE
#endif
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/mman.h>
#endif
#endif

namespace hft::core {

    // === CPU Pinning ===
    //
    // Pin a thread to a specific core to eliminate scheduler migrations.
    // Each hot-path thread (feed, strategy, gateway) should be pinned to its own isolated core.

    inline bool pin_thread_to_core(std::thread& t, int core_id) noexcept {
#ifdef _WIN32
        DWORD_PTR mask = 1ULL << core_id;
        return SetThreadAffinityMask(reinterpret_cast<HANDLE>(t.native_handle()), mask) != 0;
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        return pthread_setaffinity_np(t.native_handle(), sizeof(cpuset), &cpuset) == 0;
#endif
    }

    // Pin current thread
    inline bool pin_current_thread(int core_id) noexcept {
#ifdef _WIN32
        DWORD_PTR mask = 1ULL << core_id;
        return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        return sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0;
#endif
    }

    // Get current thread's CPU core
    [[nodiscard]] inline int current_core() noexcept {
#ifdef _WIN32
        return static_cast<int>(GetCurrentProcessorNumber());
#else
        return sched_getcpu();
#endif
    }

    // Get number of available CPU cores
    [[nodiscard]] inline int cpu_count() noexcept {
        return static_cast<int>(std::thread::hardware_concurrency());
    }

    // === NUMA Allocation ===
    //
    // Allocate memory on the same NUMA node as the pinned core.
    // Falls back to regular allocation if NUMA not available.

    inline void* numa_alloc(size_t size, [[maybe_unused]] int numa_node = 0) {
#if defined(__linux__) && __has_include(<numa.h>)
        void* ptr = numa_alloc_onnode(size, numa_node);
        if (!ptr) throw std::bad_alloc();
        return ptr;
#elif defined(_WIN32)
        // Windows NUMA allocation (requires Win7+)
        void* ptr = VirtualAllocExNuma(
            GetCurrentProcess(), nullptr, size,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE,
            static_cast<DWORD>(numa_node));
        if (!ptr) {
            // Fallback to non-NUMA allocation
            ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        if (!ptr) throw std::bad_alloc();
        return ptr;
#else
        // Portable fallback
        void* ptr = std::aligned_alloc(64, size);
        if (!ptr) throw std::bad_alloc();
        return ptr;
#endif
    }

    inline void numa_free(void* ptr, [[maybe_unused]] size_t size) noexcept {
#if defined(__linux__) && __has_include(<numa.h>)
        numa_free(ptr, size);
#elif defined(_WIN32)
        VirtualFree(ptr, 0, MEM_RELEASE);
#else
        std::free(ptr);
#endif
    }

    // === Page Pre-Warming ===
    //
    // Touch every page to fault it into TLB, avoiding latency spikes on hot path.
    // Must be called after allocation, before trading begins.

    inline void prewarm_pages(void* base, size_t size) noexcept {
        volatile char* p = static_cast<volatile char*>(base);
        for (size_t i = 0; i < size; i += 4096) {
            (void)p[i];   // Read each page to trigger page fault now
        }
    }

    // === Hugepages ===
    //
    // Allocate memory using 2MB hugepages (reduces TLB misses for large structures).

    inline void* alloc_hugepages(size_t size) {
#ifdef __linux__
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            // Fallback to regular allocation
            ptr = std::aligned_alloc(4096, size);
            if (!ptr) throw std::bad_alloc();
        }
        return ptr;
#elif defined(_WIN32)
        // Windows large pages (requires SeLockMemoryPrivilege)
        SIZE_T large_page_size = GetLargePageMinimum();
        if (large_page_size == 0) {
            void* ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!ptr) throw std::bad_alloc();
            return ptr;
        }
        // Round up to large page boundary
        size = (size + large_page_size - 1) & ~(large_page_size - 1);
        void* ptr = VirtualAlloc(nullptr, size,
                                 MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                                 PAGE_READWRITE);
        if (!ptr) {
            ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        if (!ptr) throw std::bad_alloc();
        return ptr;
#else
        void* ptr = std::aligned_alloc(4096, size);
        if (!ptr) throw std::bad_alloc();
        return ptr;
#endif
    }

    inline void free_hugepages(void* ptr, [[maybe_unused]] size_t size) noexcept {
#ifdef __linux__
        munmap(ptr, size);
#elif defined(_WIN32)
        VirtualFree(ptr, 0, MEM_RELEASE);
#else
        std::free(ptr);
#endif
    }

    // === Memory Locking ===
    //
    // Prevent memory from being swapped to disk (for secrets + hot-path data).

    inline bool lock_memory(void* ptr, size_t size) noexcept {
#ifdef _WIN32
        return VirtualLock(ptr, size) != 0;
#else
        return mlock(ptr, size) == 0;
#endif
    }

    inline bool unlock_memory(void* ptr, size_t size) noexcept {
#ifdef _WIN32
        return VirtualUnlock(ptr, size) != 0;
#else
        return munlock(ptr, size) == 0;
#endif
    }

} // namespace hft::core
