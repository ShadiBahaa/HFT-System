#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <cmath>

#include "core/types.h"
#include "core/market_data.h"
#include "risk/kill_switch.h"

namespace hft::risk {

    using namespace hft::core;

    // =========================================================================
    // Degradation levels based on system health
    // =========================================================================
    enum class DegradationLevel : uint8_t {
        NORMAL,           // Full trading
        REDUCED_SIZE,     // Halve position sizes
        PASSIVE_ONLY,     // Only take liquidity, no quoting
        FLATTEN_ONLY,     // Only reduce positions
        HALT              // Kill switch — cancel all, no new orders
    };

    // =========================================================================
    // PreTradeRisk — Two-tier inline risk checks (SEC Rule 15c3-5 mandated)
    //
    // Tier 1: Per-strategy LOCAL state (no contention, zero sharing)
    // Tier 2: GLOBAL atomic aggregation across all strategies
    //
    // All checks are O(1) and must complete in < 200ns.
    // =========================================================================

    // Maximum instruments tracked (indexed by InstrumentId = uint16_t)
    static constexpr size_t MAX_INSTRUMENTS = 65536;

    // Global position — shared across all strategies via atomics
    struct alignas(64) GlobalPosition {
        std::atomic<int64_t> net_position{0};
        std::atomic<int64_t> gross_notional{0};
    };

    // Instrument limits — read-only during trading, configured at startup
    struct InstrumentLimits {
        int64_t max_position{0};        // Max absolute net position
        int64_t max_notional{0};        // Max gross notional
        int32_t max_orders_per_sec{0};  // Rate limit per strategy
        int64_t max_order_size{0};      // Single order size limit
        int64_t fat_finger_price{0};    // Max deviation from mid price
    };

    class PreTradeRisk {
    public:
        // Per-strategy local view — only one writer, zero contention
        struct alignas(64) LocalInstrumentRisk {
            int64_t net_position{0};
            int64_t gross_notional{0};
            int32_t orders_this_second{0};
            int32_t _padding{0};
        };

    private:
        std::array<LocalInstrumentRisk, MAX_INSTRUMENTS> local_state_{};
        std::array<InstrumentLimits, MAX_INSTRUMENTS>     limits_{};
        std::array<GlobalPosition, MAX_INSTRUMENTS>*      global_positions_;
        KillSwitch*                                       kill_switch_;
        int                                               strategy_id_{0};

    public:
        PreTradeRisk(std::array<GlobalPosition, MAX_INSTRUMENTS>& global,
                     KillSwitch& kill_switch, int strategy_id = 0)
            : global_positions_(&global)
            , kill_switch_(&kill_switch)
            , strategy_id_(strategy_id) {}

        // Configure limits for an instrument (done at startup, not on hot path)
        void set_limits(InstrumentId id, const InstrumentLimits& lim) noexcept {
            limits_[id] = lim;
        }

        // Returns PASS/REJECT — uses branch hints for the common (pass) case
        [[gnu::hot, nodiscard]]
        RiskResult check(InstrumentId id, Side side, Quantity qty,
                         Price price, Price mid_price) noexcept
        {
            // Kill switch check
            if (kill_switch_->is_strategy_killed(strategy_id_)) [[unlikely]]
                return RiskResult::KILL_SWITCH;

            auto& local = local_state_[id];
            const auto& lim = limits_[id];

            int64_t delta = (side == Side::BUY) ? qty : -qty;

            // 1. Position limit — checked against GLOBAL position (cross-strategy)
            int64_t current_global = (*global_positions_)[id].net_position
                .load(std::memory_order_relaxed);
            int64_t new_global_pos = current_global + delta;
            if (std::abs(new_global_pos) > lim.max_position) [[unlikely]]
                return RiskResult::POSITION_BREACH;

            // 2. Order size limit
            if (qty > lim.max_order_size) [[unlikely]]
                return RiskResult::SIZE_BREACH;

            // 3. Fat finger check (price too far from mid)
            if (mid_price > 0) {
                int64_t deviation = std::abs(price - mid_price);
                if (deviation > lim.fat_finger_price) [[unlikely]]
                    return RiskResult::PRICE_BREACH;
            }

            // 4. Notional limit — checked against GLOBAL notional
            int64_t order_notional = price * qty;
            int64_t current_notional = (*global_positions_)[id].gross_notional
                .load(std::memory_order_relaxed);
            if (current_notional + order_notional > lim.max_notional) [[unlikely]]
                return RiskResult::NOTIONAL_BREACH;

            // 5. Rate limit — per-strategy (local), not global
            if (local.orders_this_second >= lim.max_orders_per_sec) [[unlikely]]
                return RiskResult::RATE_BREACH;

            // All checks passed — update LOCAL state (zero contention)
            local.net_position += delta;
            local.gross_notional += order_notional;
            ++local.orders_this_second;

            // Update GLOBAL state (atomic, relaxed — slight staleness acceptable)
            (*global_positions_)[id].net_position.fetch_add(
                delta, std::memory_order_relaxed);
            (*global_positions_)[id].gross_notional.fetch_add(
                order_notional, std::memory_order_relaxed);

            return RiskResult::PASS;
        }

        // Called once per second by timer on telemetry thread
        void reset_rate_counters() noexcept {
            for (auto& r : local_state_) {
                r.orders_this_second = 0;
            }
        }

        // For testing / monitoring
        [[nodiscard]] const LocalInstrumentRisk& local_risk(InstrumentId id) const noexcept {
            return local_state_[id];
        }

        [[nodiscard]] const InstrumentLimits& instrument_limits(InstrumentId id) const noexcept {
            return limits_[id];
        }
    };

} // namespace hft::risk
