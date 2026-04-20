#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>
#include "core/types.h"
#include "core/market_data.h"

namespace hft::risk {

    using namespace hft::core;

    // =========================================================================
    // PositionManager — per-instrument net position + realized/unrealized P&L.
    //
    // The hot path (OMS thread) calls on_fill() on every execution report.
    // Off-path readers (risk / telemetry threads) call snapshot() which
    // returns a consistent copy via a seqlock-style pattern.
    // =========================================================================

    struct PositionSnapshot {
        InstrumentId    instrument_id{0};
        int64_t         net_quantity{0};     // Signed: positive=long, negative=short
        int64_t         gross_notional{0};   // Absolute exposure
        Price           avg_entry_price{0};
        int64_t         realized_pnl{0};     // In price * qty units
        uint64_t        fill_count{0};
    };

    template <size_t MaxInstruments = 4096>
    class PositionManager {
    public:
        PositionManager() = default;

        // Called on each filled execution. Updates position, realized P&L, and
        // average entry price.
        void on_fill(const ExecutionReport& er) noexcept {
            if (er.exec_type != ExecType::FILL && er.exec_type != ExecType::PARTIAL) return;
            if (er.instrument_id >= MaxInstruments) return;

            auto& slot = slots_[er.instrument_id];
            int64_t signed_qty = (er.side == Side::BUY)
                ? static_cast<int64_t>(er.filled_qty)
                : -static_cast<int64_t>(er.filled_qty);

            // Seqlock write: odd = in progress, even = complete
            uint64_t seq = slot.seq.load(std::memory_order_relaxed);
            slot.seq.store(seq + 1, std::memory_order_release);

            int64_t old_qty = slot.net_quantity;
            int64_t new_qty = old_qty + signed_qty;

            // Realized P&L accrues when we close or flip direction
            if ((old_qty > 0 && signed_qty < 0) || (old_qty < 0 && signed_qty > 0)) {
                int64_t closing = (std::abs(signed_qty) < std::abs(old_qty))
                    ? std::abs(signed_qty) : std::abs(old_qty);
                int64_t pnl = (old_qty > 0)
                    ? closing * (er.price - slot.avg_entry_price)
                    : closing * (slot.avg_entry_price - er.price);
                slot.realized_pnl += pnl;
            }

            // Update average entry price when adding to an existing position
            if ((old_qty >= 0 && signed_qty > 0) || (old_qty <= 0 && signed_qty < 0)) {
                int64_t total_abs = std::abs(old_qty) + std::abs(signed_qty);
                if (total_abs != 0) {
                    slot.avg_entry_price = static_cast<Price>(
                        (std::abs(old_qty) * static_cast<int64_t>(slot.avg_entry_price) +
                         std::abs(signed_qty) * static_cast<int64_t>(er.price)) / total_abs);
                }
            } else if (old_qty == 0 || ((old_qty ^ new_qty) < 0)) {
                // Position flipped sign — reset avg entry to current fill
                slot.avg_entry_price = er.price;
            }

            slot.net_quantity = new_qty;
            slot.gross_notional = std::abs(new_qty) * static_cast<int64_t>(er.price);
            slot.instrument_id = er.instrument_id;
            ++slot.fill_count;

            slot.seq.store(seq + 2, std::memory_order_release);
        }

        // Consistent read: retry on seqlock collision
        [[nodiscard]] PositionSnapshot get(InstrumentId id) const noexcept {
            if (id >= MaxInstruments) return {};
            const auto& slot = slots_[id];
            for (;;) {
                uint64_t s1 = slot.seq.load(std::memory_order_acquire);
                if (s1 & 1) continue;  // Writer in progress
                PositionSnapshot snap;
                snap.instrument_id   = slot.instrument_id;
                snap.net_quantity    = slot.net_quantity;
                snap.gross_notional  = slot.gross_notional;
                snap.avg_entry_price = slot.avg_entry_price;
                snap.realized_pnl    = slot.realized_pnl;
                snap.fill_count      = slot.fill_count;
                uint64_t s2 = slot.seq.load(std::memory_order_acquire);
                if (s1 == s2) return snap;
            }
        }

        // Snapshot all non-zero positions
        [[nodiscard]] std::vector<PositionSnapshot> snapshot() const {
            std::vector<PositionSnapshot> out;
            out.reserve(64);
            for (size_t i = 0; i < MaxInstruments; ++i) {
                if (slots_[i].fill_count == 0) continue;
                auto s = get(static_cast<InstrumentId>(i));
                if (s.fill_count > 0) out.push_back(s);
            }
            return out;
        }

        // Aggregate gross notional across all instruments
        [[nodiscard]] int64_t total_gross_notional() const noexcept {
            int64_t total = 0;
            for (size_t i = 0; i < MaxInstruments; ++i) {
                total += slots_[i].gross_notional;
            }
            return total;
        }

        [[nodiscard]] int64_t total_realized_pnl() const noexcept {
            int64_t total = 0;
            for (size_t i = 0; i < MaxInstruments; ++i) {
                total += slots_[i].realized_pnl;
            }
            return total;
        }

        void reset(InstrumentId id) noexcept {
            if (id >= MaxInstruments) return;
            auto& slot = slots_[id];
            uint64_t seq = slot.seq.load(std::memory_order_relaxed);
            slot.seq.store(seq + 1, std::memory_order_release);
            slot.net_quantity = 0;
            slot.gross_notional = 0;
            slot.avg_entry_price = 0;
            slot.realized_pnl = 0;
            slot.fill_count = 0;
            slot.seq.store(seq + 2, std::memory_order_release);
        }

    private:
        struct alignas(64) Slot {
            std::atomic<uint64_t> seq{0};
            InstrumentId instrument_id{0};
            int64_t      net_quantity{0};
            int64_t      gross_notional{0};
            Price        avg_entry_price{0};
            int64_t      realized_pnl{0};
            uint64_t     fill_count{0};
        };

        std::array<Slot, MaxInstruments> slots_{};
    };

} // namespace hft::risk
