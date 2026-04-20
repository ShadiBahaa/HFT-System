#pragma once

#include <cstdint>
#include <cmath>
#include <array>
#include <string_view>

#include "core/types.h"
#include "core/market_data.h"
#include "strategy/strategy_interface.h"

namespace hft::strategy {

    using namespace hft::core;

    // =========================================================================
    // StatArbPairs — statistical arbitrage (pairs trading)
    //
    // Tracks the spread between two instruments, computes its rolling mean
    // and standard deviation using Welford's online algorithm, and emits
    // mean-reversion signals when the z-score breaches configured thresholds.
    // =========================================================================
    class StatArbPairs : public StrategyBase<StatArbPairs> {
        friend class StrategyBase<StatArbPairs>;

    public:
        struct Config {
            InstrumentId instrument_a{0};          // Long leg
            InstrumentId instrument_b{0};          // Short leg
            double       hedge_ratio{1.0};         // B shares per 1 A
            uint32_t     lookback_window{200};     // Samples for mean/std
            double       entry_z_score{2.0};       // |z| >= entry -> open
            double       exit_z_score{0.5};        // |z| <= exit -> close
            Quantity     trade_qty{100};
            InstrumentId target_instrument{0};     // Which leg to actually trade
        };

    private:
        Config config_{};

        // Welford's online mean/variance
        uint64_t count_{0};
        double   mean_{0.0};
        double   m2_{0.0};                         // Sum of squared deltas

        // Last seen prices on each leg
        Price last_a_{0};
        Price last_b_{0};

        // Position bookkeeping
        int32_t position_{0};                      // -1 short, +1 long, 0 flat

        [[nodiscard]] double compute_spread(Price a, Price b) const noexcept {
            return static_cast<double>(a) - config_.hedge_ratio * static_cast<double>(b);
        }

        void update_stats(double spread) noexcept {
            ++count_;
            double delta = spread - mean_;
            mean_ += delta / static_cast<double>(count_);
            double delta2 = spread - mean_;
            m2_ += delta * delta2;

            // Cap window so stats slowly forget very old samples.
            if (count_ > config_.lookback_window) {
                // Exponential decay on m2 to approximate a moving window.
                double decay = static_cast<double>(config_.lookback_window)
                             / static_cast<double>(count_);
                m2_ *= decay;
                count_ = config_.lookback_window;
            }
        }

        [[nodiscard]] double std_dev() const noexcept {
            if (count_ < 2) return 0.0;
            return std::sqrt(m2_ / static_cast<double>(count_ - 1));
        }

        Signal on_book_update_impl(InstrumentId id, const BookSignal& sig) noexcept {
            if (sig.mid_price == 0) return {};

            if (id == config_.instrument_a) last_a_ = sig.mid_price;
            else if (id == config_.instrument_b) last_b_ = sig.mid_price;
            else return {};

            if (last_a_ == 0 || last_b_ == 0) return {};

            double spread = compute_spread(last_a_, last_b_);
            update_stats(spread);

            double sd = std_dev();
            if (sd <= 0.0) return {};

            double z = (spread - mean_) / sd;

            Signal out{};
            out.instrument_id = (config_.target_instrument != 0)
                                ? config_.target_instrument
                                : config_.instrument_a;
            out.quantity = config_.trade_qty;
            out.price = 0;                         // market
            out.urgency = std::min(1.0, std::abs(z) / 4.0);

            if (position_ == 0) {
                // Opening: spread high (>= entry) -> short A (sell), buy B
                if (z >= config_.entry_z_score) {
                    out.action = Action::SELL;
                    out.side = Side::SELL;
                    position_ = -1;
                    return out;
                }
                if (z <= -config_.entry_z_score) {
                    out.action = Action::BUY;
                    out.side = Side::BUY;
                    position_ = 1;
                    return out;
                }
            } else if (std::abs(z) <= config_.exit_z_score) {
                // Close position
                out.action = (position_ > 0) ? Action::SELL : Action::BUY;
                out.side   = (position_ > 0) ? Side::SELL   : Side::BUY;
                position_ = 0;
                return out;
            }
            return {};
        }

        void on_execution_report_impl(const ExecutionReport&) noexcept {}

        void initialize_impl() {
            count_ = 0;
            mean_ = 0.0;
            m2_ = 0.0;
            last_a_ = 0;
            last_b_ = 0;
            position_ = 0;
        }

        [[nodiscard]] std::string_view name_impl() const noexcept {
            return "StatArbPairs";
        }

    public:
        explicit StatArbPairs(const Config& cfg) : config_(cfg) {}

        [[nodiscard]] double mean() const noexcept { return mean_; }
        [[nodiscard]] double stddev() const noexcept { return std_dev(); }
        [[nodiscard]] uint64_t samples() const noexcept { return count_; }
        [[nodiscard]] int32_t position() const noexcept { return position_; }
        [[nodiscard]] const Config& config() const noexcept { return config_; }
    };

} // namespace hft::strategy
