#pragma once

#include <cstdint>
#include <algorithm>

#include "core/clock.h"

namespace hft::risk {

    using namespace hft::core;

    // =========================================================================
    // ExchangeThrottle — token bucket rate limiter matching exchange behavior
    //
    // Exchange rate limits (300-1000 orders/sec) are hard limits. Exceeding
    // them causes port disconnection, fines, or suspension.
    // =========================================================================
    class ExchangeThrottle {
    public:
        struct TokenBucket {
            int64_t  tokens{0};
            int64_t  max_tokens{0};         // Burst limit
            int64_t  refill_per_sec{0};     // Tokens added per second
            uint64_t last_refill_tsc{0};
            double   tsc_per_sec{1.0};      // TSC frequency (calibrated)

            void configure(int64_t max, int64_t refill, double tsc_freq) noexcept {
                tokens = max;
                max_tokens = max;
                refill_per_sec = refill;
                last_refill_tsc = rdtsc();
                tsc_per_sec = tsc_freq;
            }

            [[gnu::hot]] bool try_consume(uint64_t now_tsc, int64_t cost = 1) noexcept {
                // Refill tokens based on elapsed time
                uint64_t elapsed = now_tsc - last_refill_tsc;
                double elapsed_sec = static_cast<double>(elapsed) / tsc_per_sec;
                int64_t refill = static_cast<int64_t>(elapsed_sec * refill_per_sec);
                if (refill > 0) {
                    tokens = std::min(max_tokens, tokens + refill);
                    last_refill_tsc = now_tsc;
                }

                if (tokens >= cost) [[likely]] {
                    tokens -= cost;
                    return true;
                }
                return false;
            }
        };

    private:
        TokenBucket new_orders_;
        TokenBucket cancel_replace_;
        TokenBucket messages_total_;

    public:
        // Configure per-exchange limits
        // tsc_freq: approximate TSC ticks per second (calibrated at startup)
        void configure(int new_per_sec, int cancel_per_sec, int total_per_sec,
                       int burst_limit, double tsc_freq) noexcept {
            new_orders_.configure(burst_limit, new_per_sec, tsc_freq);
            cancel_replace_.configure(burst_limit, cancel_per_sec, tsc_freq);
            messages_total_.configure(burst_limit * 2, total_per_sec, tsc_freq);
        }

        [[gnu::hot, nodiscard]]
        bool allow_new_order() noexcept {
            auto now = rdtsc();
            return new_orders_.try_consume(now) && messages_total_.try_consume(now);
        }

        [[gnu::hot, nodiscard]]
        bool allow_cancel() noexcept {
            auto now = rdtsc();
            return cancel_replace_.try_consume(now) && messages_total_.try_consume(now);
        }

        // For testing: peek at remaining tokens
        [[nodiscard]] int64_t new_order_tokens() const noexcept { return new_orders_.tokens; }
        [[nodiscard]] int64_t cancel_tokens() const noexcept { return cancel_replace_.tokens; }
    };

} // namespace hft::risk
