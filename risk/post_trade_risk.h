#pragma once

#include <cstdint>
#include <cmath>
#include <array>
#include <atomic>
#include "core/types.h"
#include "risk/kill_switch.h"

namespace hft::risk {

    using namespace hft::core;

    // Position snapshot for a single instrument
    struct PositionSnapshot {
        InstrumentId instrument_id{0};
        int64_t net_position{0};
        int64_t gross_notional{0};
        Price avg_entry_price{0};
        Price mark_price{0};
        int64_t realized_pnl{0};
        int64_t unrealized_pnl{0};
    };

    // Portfolio-level risk metrics
    struct PortfolioMetrics {
        int64_t total_pnl{0};              // realized + unrealized
        int64_t total_realized_pnl{0};
        int64_t total_unrealized_pnl{0};
        int64_t gross_notional{0};
        int64_t net_delta{0};              // net exposure across all instruments
        double var_95{0.0};                // Value-at-Risk 95th percentile
    };

    // Post-trade risk aggregation — runs off critical path on separate core.
    // Aggregates position snapshots and checks portfolio-level limits.
    class PostTradeRisk {
    public:
        static constexpr int MAX_INSTRUMENTS = 256;

        struct Config {
            int64_t max_drawdown{0};            // max cumulative loss before kill switch
            double var_limit{0.0};              // VaR 95% limit
            int64_t delta_limit{0};             // max abs portfolio delta
            int64_t max_gross_notional{0};      // max total exposure
            double min_capital_ratio{0.0};      // regulatory capital adequacy
        };

    private:
        Config config_;
        KillSwitch* kill_switch_{nullptr};

        std::array<PositionSnapshot, MAX_INSTRUMENTS> positions_{};
        int num_instruments_{0};

        int64_t high_water_mark_pnl_{0};
        int64_t current_drawdown_{0};

        PortfolioMetrics latest_metrics_{};

    public:
        PostTradeRisk() = default;

        void set_config(const Config& cfg) noexcept { config_ = cfg; }
        void set_kill_switch(KillSwitch* ks) noexcept { kill_switch_ = ks; }

        // Update a position snapshot
        void update_position(const PositionSnapshot& snap) noexcept {
            for (int i = 0; i < num_instruments_; ++i) {
                if (positions_[static_cast<size_t>(i)].instrument_id == snap.instrument_id) {
                    positions_[static_cast<size_t>(i)] = snap;
                    return;
                }
            }
            if (num_instruments_ < MAX_INSTRUMENTS) {
                positions_[static_cast<size_t>(num_instruments_++)] = snap;
            }
        }

        // Calculate portfolio metrics from current snapshots
        PortfolioMetrics calculate_metrics() noexcept {
            PortfolioMetrics m{};

            for (int i = 0; i < num_instruments_; ++i) {
                const auto& pos = positions_[static_cast<size_t>(i)];
                m.total_realized_pnl += pos.realized_pnl;
                m.total_unrealized_pnl += pos.unrealized_pnl;
                m.gross_notional += std::abs(pos.net_position * pos.mark_price);
                m.net_delta += pos.net_position;
            }

            m.total_pnl = m.total_realized_pnl + m.total_unrealized_pnl;

            // Simple parametric VaR: 1.65 * sqrt(sum of squared position values)
            // Real implementation would use historical volatilities
            double sum_sq = 0.0;
            for (int i = 0; i < num_instruments_; ++i) {
                const auto& pos = positions_[static_cast<size_t>(i)];
                double notional = static_cast<double>(std::abs(pos.net_position))
                                * static_cast<double>(pos.mark_price);
                sum_sq += notional * notional;
            }
            m.var_95 = 1.65 * std::sqrt(sum_sq) * 0.01;  // assume 1% daily vol

            latest_metrics_ = m;
            return m;
        }

        // Check portfolio-level limits and trigger kill switch if breached
        // Returns true if all limits pass, false if any breached
        bool evaluate() noexcept {
            auto m = calculate_metrics();

            // Update drawdown tracking
            if (m.total_pnl > high_water_mark_pnl_) {
                high_water_mark_pnl_ = m.total_pnl;
            }
            current_drawdown_ = high_water_mark_pnl_ - m.total_pnl;

            // Check drawdown limit
            if (config_.max_drawdown > 0 && current_drawdown_ > config_.max_drawdown) {
                if (kill_switch_) kill_switch_->activate_global();
                return false;
            }

            // Check VaR limit
            if (config_.var_limit > 0.0 && m.var_95 > config_.var_limit) {
                return false;
            }

            // Check delta limit
            if (config_.delta_limit > 0 && std::abs(m.net_delta) > config_.delta_limit) {
                return false;
            }

            // Check gross notional
            if (config_.max_gross_notional > 0 && m.gross_notional > config_.max_gross_notional) {
                return false;
            }

            return true;
        }

        [[nodiscard]] const PortfolioMetrics& latest_metrics() const noexcept {
            return latest_metrics_;
        }

        [[nodiscard]] int64_t current_drawdown() const noexcept {
            return current_drawdown_;
        }

        [[nodiscard]] int num_instruments() const noexcept {
            return num_instruments_;
        }

        void reset() noexcept {
            num_instruments_ = 0;
            high_water_mark_pnl_ = 0;
            current_drawdown_ = 0;
            latest_metrics_ = {};
        }
    };

} // namespace hft::risk
