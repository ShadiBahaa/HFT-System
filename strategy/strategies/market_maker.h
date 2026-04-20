#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string_view>

#include "core/types.h"
#include "core/market_data.h"
#include "strategy/strategy_interface.h"

namespace hft::strategy {

    using namespace hft::core;

    // =========================================================================
    // MarketMaker — sample strategy using CRTP
    //
    // Two-sided quoting with dynamic spread based on inventory and volatility.
    // Fully inlineable by the compiler through the CRTP base.
    // =========================================================================
    class MarketMaker : public StrategyBase<MarketMaker> {
        friend class StrategyBase<MarketMaker>;

    public:
        struct Config {
            InstrumentId instrument_id{0};
            Quantity     base_qty{100};          // Base quote size
            Quantity     max_position{1000};      // Max net position
            Price        min_spread{10};          // Minimum spread (fixed-point)
            Price        max_spread{100};         // Maximum spread
            double       skew_factor{0.5};        // Inventory skew strength
            double       volatility_scale{1.0};   // Spread widening factor
        };

    private:
        Config config_{};

        // Strategy state
        Quantity net_position_{0};
        int64_t  realized_pnl_{0};
        Price    last_mid_{0};

        Signal on_book_update_impl(InstrumentId id, const BookSignal& signal) noexcept {
            if (id != config_.instrument_id) return {};
            if (signal.best_bid == 0 || signal.best_ask == 0) return {};

            Price mid = signal.mid_price;
            Price raw_spread = signal.spread;
            last_mid_ = mid;

            // Dynamic spread: widen based on inventory risk
            double inventory_ratio = static_cast<double>(net_position_) /
                                     static_cast<double>(config_.max_position);
            Price spread = std::max(config_.min_spread,
                static_cast<Price>(raw_spread * config_.volatility_scale));
            spread = std::min(spread, config_.max_spread);

            // Skew quotes based on inventory: if long, lower bid more
            Price skew = static_cast<Price>(
                inventory_ratio * config_.skew_factor * spread);

            Price bid_price = mid - spread / 2 - skew;
            Price ask_price = mid + spread / 2 - skew;

            // Decide which side to quote based on position limits
            Signal sig{};
            sig.instrument_id = id;

            if (net_position_ < config_.max_position) {
                // Can buy more — post bid
                sig.action = Action::BUY;
                sig.side = Side::BUY;
                sig.price = bid_price;
                sig.quantity = config_.base_qty;
                sig.urgency = 1.0 - std::abs(inventory_ratio);
            } else if (net_position_ > -config_.max_position) {
                // Can sell more — post ask
                sig.action = Action::SELL;
                sig.side = Side::SELL;
                sig.price = ask_price;
                sig.quantity = config_.base_qty;
                sig.urgency = 1.0 - std::abs(inventory_ratio);
            }

            return sig;
        }

        void on_execution_report_impl(const ExecutionReport& report) noexcept {
            if (report.exec_type == ExecType::FILL ||
                report.exec_type == ExecType::PARTIAL) {
                Quantity delta = (report.side == Side::BUY)
                    ? report.filled_qty : -report.filled_qty;
                net_position_ += delta;
            }
        }

        void initialize_impl() {
            net_position_ = 0;
            realized_pnl_ = 0;
            last_mid_ = 0;
        }

        [[nodiscard]] std::string_view name_impl() const noexcept {
            return "MarketMaker";
        }

    public:
        explicit MarketMaker(const Config& cfg) : config_(cfg) {}

        [[nodiscard]] Quantity net_position() const noexcept { return net_position_; }
        [[nodiscard]] int64_t realized_pnl() const noexcept { return realized_pnl_; }
        [[nodiscard]] const Config& config() const noexcept { return config_; }
    };

} // namespace hft::strategy
