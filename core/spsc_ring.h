#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace hft::core {

// L1 Cache Line size is typically 64 bytes on modern x86.
// C++17 provides std::hardware_constructive_interference_size, 
// but it's not well supported everywhere, so we fallback to 64.
#if defined(__cpp_lib_hardware_interference_size)
    constexpr size_t CACHE_LINE_SIZE = std::hardware_constructive_interference_size;
#else
    constexpr size_t CACHE_LINE_SIZE = 64;
#endif

// Lock-Free Single-Producer Single-Consumer (SPSC) Ring Buffer.
//
// Designed for ultra-low latency inter-thread communication.
// Producer and Consumer atomic indices are padded to separate 
// cache lines to avoid "false sharing" performance cliffs.
template <typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), 
                  "Capacity must be a power of 2 for fast modulo arithmetic.");

private:
    static constexpr size_t MASK = Capacity - 1;

    // Shared buffer - pre-allocated, value-initialized to suppress
    // GCC -O3 -Wmaybe-uninitialized false positive on the read path
    T buffer_[Capacity]{};

    // Padding ensures 'head_' is on its own cache line
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0}; // Written by Producer

    // Padding ensures 'tail_' is on its own cache line
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0}; // Written by Consumer

    // Cached indices (avoid touching atomics if we don't have to)
    size_t cached_tail_{0}; // Used by Producer
    size_t cached_head_{0}; // Used by Consumer

public:
    SPSCRingBuffer() = default;

    // Non-copyable, non-movable
    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    // Called by PRODUCER Thread
    // Returns false if the buffer is full
    template <typename U>
    [[gnu::hot]] bool try_push(U&& item) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        
        // Next index where the head will arrive
        const size_t next_head = current_head + 1;

        // Check against cached tail to avoid atomic read if possible
        if (next_head - cached_tail_ > Capacity) {
            // Buffer appears full, sync tail from consumer
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next_head - cached_tail_ > Capacity) {
                return false; // Still full
            }
        }

        // Store item into buffer
        buffer_[current_head & MASK] = std::forward<U>(item);

        // Memory release: ensure the memory write to buffer_ 
        // completes before head_ is visible to Consumer
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Called by CONSUMER Thread
    // Returns false if the buffer is empty
    [[gnu::hot]] bool try_pop(T& out_item) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == cached_head_) {
            // Buffer appears empty, sync head from producer
            cached_head_ = head_.load(std::memory_order_acquire);
            if (current_tail == cached_head_) {
                return false; // Still empty
            }
        }

        // Read item from buffer
        out_item = buffer_[current_tail & MASK];

        // Memory release: ensure memory read finishes before 
        // Producer thinks this slot is free
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    // Rough size estimate. Not fully thread-safe for exact sizing.
    [[nodiscard]] size_t size() const noexcept {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return h >= t ? h - t : Capacity + h - t;
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept {
        return Capacity;
    }
};

} // namespace hft::core
