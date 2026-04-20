#pragma once

#include <cstdint>

#include "core/types.h"
#include "core/market_data.h"
#include "core/spsc_ring.h"
#include "feed/normalizer.h"
#include "feed/gap_detector.h"

namespace hft::feed {

    using namespace hft::core;

    // =========================================================================
    // FeedHandler — receives raw packets, decodes, and pushes normalized
    // MarketUpdates into an SPSC ring for the order book builder.
    //
    // In production this sits on an isolated core running a busy-poll loop
    // over ef_vi or DPDK. Here we expose a process_packet() method that
    // can be called from any packet source.
    // =========================================================================
    template <size_t RingCapacity = 65536>
    class FeedHandler {
    public:
        using UpdateRing = SPSCRingBuffer<MarketUpdate, RingCapacity>;

    private:
        UpdateRing& output_ring_;
        GapDetector gap_detector_;
        uint64_t packets_processed_{0};
        uint64_t packets_dropped_{0};

    public:
        explicit FeedHandler(UpdateRing& ring) noexcept
            : output_ring_(ring) {}

        // Process a raw ITCH packet buffer
        // Returns true if the update was decoded and enqueued
        [[gnu::hot]] bool process_packet(const uint8_t* data, size_t len, uint64_t seq) noexcept {
            // Gap detection
            if (!gap_detector_.check(seq)) {
                ++packets_dropped_;
                return false;  // Duplicate/old — discard
            }

            // Decode
            MarketUpdate update{};
            update.sequence = seq;
            if (!dispatch_itch(data, len, update)) {
                ++packets_dropped_;
                return false;  // Unknown or malformed message
            }

            // Enqueue for order book builder
            if (!output_ring_.try_push(update)) {
                ++packets_dropped_;
                return false;  // Ring full — back pressure
            }

            ++packets_processed_;
            return true;
        }

        [[nodiscard]] uint64_t packets_processed() const noexcept { return packets_processed_; }
        [[nodiscard]] uint64_t packets_dropped() const noexcept { return packets_dropped_; }
        [[nodiscard]] const GapDetector& gap_detector() const noexcept { return gap_detector_; }

        GapDetector& gap_detector() noexcept { return gap_detector_; }
    };

} // namespace hft::feed
