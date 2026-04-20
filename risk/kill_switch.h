#pragma once

#include <atomic>
#include <cstdint>

namespace hft::risk {

    // =========================================================================
    // KillSwitch — atomic flag, can be flipped from any thread
    // =========================================================================
    class KillSwitch {
        std::atomic<bool> global_kill_{false};

        static constexpr int MAX_STRATEGIES = 32;
        std::atomic<bool> per_strategy_kill_[MAX_STRATEGIES]{};

    public:
        void activate_global() noexcept {
            global_kill_.store(true, std::memory_order_release);
        }

        void deactivate_global() noexcept {
            global_kill_.store(false, std::memory_order_release);
        }

        void activate_strategy(int strategy_id) noexcept {
            if (strategy_id >= 0 && strategy_id < MAX_STRATEGIES)
                per_strategy_kill_[strategy_id].store(true, std::memory_order_release);
        }

        void deactivate_strategy(int strategy_id) noexcept {
            if (strategy_id >= 0 && strategy_id < MAX_STRATEGIES)
                per_strategy_kill_[strategy_id].store(false, std::memory_order_release);
        }

        [[gnu::hot, nodiscard]]
        bool is_active() const noexcept {
            return global_kill_.load(std::memory_order_acquire);
        }

        [[gnu::hot, nodiscard]]
        bool is_strategy_killed(int strategy_id) const noexcept {
            if (strategy_id < 0 || strategy_id >= MAX_STRATEGIES) return true;
            return global_kill_.load(std::memory_order_acquire) ||
                   per_strategy_kill_[strategy_id].load(std::memory_order_acquire);
        }
    };

} // namespace hft::risk
