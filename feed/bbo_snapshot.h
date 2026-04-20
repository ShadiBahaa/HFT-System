#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include "core/types.h"

namespace hft::feed {

    using namespace hft::core;

    // =========================================================================
    // BBOSnapshot — Struct-of-Arrays best bid/offer table for multi-instrument
    // scanning. Designed to fit SIMD-friendly sweeps over many symbols.
    //
    // Each field array is page-aligned so large strategies (index arb,
    // cross-instrument alpha) can scan the entire table in a tight loop.
    // =========================================================================
    template <size_t MaxInstruments = 8192>
    struct alignas(4096) BBOSnapshot {
        static constexpr size_t CAPACITY = MaxInstruments;

        alignas(64) std::array<Price,    CAPACITY> best_bids{};
        alignas(64) std::array<Price,    CAPACITY> best_asks{};
        alignas(64) std::array<Quantity, CAPACITY> bid_sizes{};
        alignas(64) std::array<Quantity, CAPACITY> ask_sizes{};
        alignas(64) std::array<uint64_t, CAPACITY> last_update_ns{};

        // Pre-warm all pages so the first real update doesn't page-fault.
        // Note: touching every 4KB page is enough to fault them in.
        void prewarm_pages() noexcept {
            volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(this);
            constexpr size_t total = sizeof(*this);
            for (size_t off = 0; off < total; off += 4096) {
                p[off] = 0;
            }
        }

        [[gnu::hot]] void update(InstrumentId id,
                                 Price bid, Price ask,
                                 Quantity bid_sz, Quantity ask_sz,
                                 uint64_t ts_ns) noexcept
        {
            const size_t idx = static_cast<size_t>(id);
            if (idx >= CAPACITY) return;
            best_bids[idx]       = bid;
            best_asks[idx]       = ask;
            bid_sizes[idx]       = bid_sz;
            ask_sizes[idx]       = ask_sz;
            last_update_ns[idx]  = ts_ns;
        }

        [[nodiscard]] Price best_bid(InstrumentId id) const noexcept {
            const size_t idx = static_cast<size_t>(id);
            return (idx < CAPACITY) ? best_bids[idx] : 0;
        }

        [[nodiscard]] Price best_ask(InstrumentId id) const noexcept {
            const size_t idx = static_cast<size_t>(id);
            return (idx < CAPACITY) ? best_asks[idx] : 0;
        }

        [[nodiscard]] Price mid(InstrumentId id) const noexcept {
            const size_t idx = static_cast<size_t>(id);
            if (idx >= CAPACITY) return 0;
            Price b = best_bids[idx], a = best_asks[idx];
            if (b == 0 || a == 0) return 0;
            return (b + a) / 2;
        }

        [[nodiscard]] uint64_t last_update(InstrumentId id) const noexcept {
            const size_t idx = static_cast<size_t>(id);
            return (idx < CAPACITY) ? last_update_ns[idx] : 0;
        }

        void clear() noexcept {
            std::memset(this, 0, sizeof(*this));
        }
    };

} // namespace hft::feed
