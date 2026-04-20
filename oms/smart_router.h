#pragma once

#include <array>
#include <cstdint>
#include <algorithm>

#include "core/types.h"
#include "oms/order_types.h"

namespace hft::oms {

    using namespace hft::core;

    // =========================================================================
    // SmartRouter — venue selection based on price, latency, and fees
    //
    // Score = price_improvement - latency_penalty - fee_cost
    // All arithmetic is integer to avoid FPU penalties on hot path.
    // =========================================================================
    class SmartRouter {
        static constexpr int MAX_VENUES = 16;

        std::array<VenueMetrics, MAX_VENUES> venues_{};
        int num_venues_{0};

    public:
        void set_venue_count(int count) noexcept {
            num_venues_ = std::min(count, MAX_VENUES);
        }

        void update_venue(int idx, const VenueMetrics& metrics) noexcept {
            if (idx >= 0 && idx < MAX_VENUES) {
                venues_[idx] = metrics;
            }
        }

        [[nodiscard]] const VenueMetrics& venue(int idx) const noexcept {
            return venues_[idx];
        }

        [[gnu::hot, nodiscard]]
        VenueId select_venue(Side side, Price price, Quantity qty) const noexcept {
            VenueId best = VenueId::UNKNOWN;
            int64_t best_score = INT64_MIN;

            for (int i = 0; i < num_venues_; ++i) {
                if (!venues_[i].active) continue;

                Price price_at_venue = (side == Side::BUY)
                    ? venues_[i].best_ask : venues_[i].best_bid;
                Quantity size_at_venue = (side == Side::BUY)
                    ? venues_[i].ask_size : venues_[i].bid_size;

                // Skip venues without sufficient liquidity
                if (size_at_venue < qty) continue;
                if (price_at_venue == 0) continue;

                // Score = price improvement - latency cost - fee
                int64_t price_improvement = (price - price_at_venue)
                    * ((side == Side::BUY) ? 1 : -1);
                int64_t latency_penalty = venues_[i].latency_p50_ns / 100;
                int64_t fee_cost = venues_[i].taker_fee_bps;

                int64_t score = price_improvement - latency_penalty - fee_cost;

                if (score > best_score) {
                    best_score = score;
                    best = static_cast<VenueId>(i + 1);  // VenueId 0 = UNKNOWN
                }
            }
            return best;
        }
    };

} // namespace hft::oms
