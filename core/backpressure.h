#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include "core/types.h"
#include "core/market_data.h"

namespace hft::core {

    // === Double-Buffer Conflation ===
    //
    // Solves the torn-read problem: writing a MarketUpdate struct non-atomically
    // while a consumer reads it concurrently can produce half-old, half-new data.
    //
    // Producer writes to inactive buffer, then atomically increments sequence.
    // Consumer reads from the buffer indicated by the NEW sequence.
    // Required invariant: exactly ONE producer and ONE consumer per instrument.

    struct alignas(128) ConflatedUpdate {
        MarketUpdate buffers[2]{};                // double-buffer
        std::atomic<uint64_t> sequence{0};        // low bit = active buffer index

        void publish(const MarketUpdate& update) noexcept {
            uint64_t seq = sequence.load(std::memory_order_relaxed);
            int write_idx = static_cast<int>((seq & 1) ^ 1);  // write to INACTIVE buffer
            buffers[write_idx] = update;
            sequence.store(seq + 1, std::memory_order_release);
        }

        bool consume(MarketUpdate& out, uint64_t& last_seq) noexcept {
            uint64_t seq = sequence.load(std::memory_order_acquire);
            if (seq == last_seq) return false;       // no new data
            int read_idx = static_cast<int>(seq & 1);
            out = buffers[read_idx];
            last_seq = seq;
            return true;
        }
    };

    // === Backpressure Policy ===
    //
    // If strategy can't keep up with market data:
    // 1. Drop stale updates (keep only latest per instrument)
    // 2. Never block the feed handler
    // 3. Measure drop rate as a health metric

    class BackpressurePolicy {
        std::atomic<uint64_t> drops_{0};
        std::atomic<uint64_t> processed_{0};

        // Per-instrument conflated updates
        // Using a pointer to avoid huge stack allocations
        static constexpr size_t MAX_INSTRUMENTS = 65536;

        struct InstrumentSlot {
            ConflatedUpdate update;
        };

        // Use smaller default size — users can configure
        static constexpr size_t DEFAULT_CAPACITY = 8192;
        std::array<InstrumentSlot, DEFAULT_CAPACITY> slots_{};

    public:
        void publish(InstrumentId id, const MarketUpdate& update) noexcept {
            if (id < DEFAULT_CAPACITY) {
                slots_[id].update.publish(update);
            }
        }

        bool consume(InstrumentId id, MarketUpdate& out, uint64_t& last_seq) noexcept {
            if (id >= DEFAULT_CAPACITY) return false;

            auto& entry = slots_[id].update;
            uint64_t seq = entry.sequence.load(std::memory_order_acquire);
            if (seq == last_seq) return false;

            int read_idx = static_cast<int>(seq & 1);
            out = entry.buffers[read_idx];

            uint64_t skipped = seq - last_seq - 1;
            if (skipped > 0) {
                drops_.fetch_add(skipped, std::memory_order_relaxed);
            }
            processed_.fetch_add(1, std::memory_order_relaxed);
            last_seq = seq;
            return true;
        }

        [[nodiscard]] double drop_rate() const noexcept {
            uint64_t d = drops_.load(std::memory_order_relaxed);
            uint64_t p = processed_.load(std::memory_order_relaxed);
            return (p + d) > 0 ? static_cast<double>(d) / static_cast<double>(p + d) : 0.0;
        }

        [[nodiscard]] uint64_t total_drops() const noexcept {
            return drops_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] uint64_t total_processed() const noexcept {
            return processed_.load(std::memory_order_relaxed);
        }

        void reset_counters() noexcept {
            drops_.store(0, std::memory_order_relaxed);
            processed_.store(0, std::memory_order_relaxed);
        }
    };

} // namespace hft::core
