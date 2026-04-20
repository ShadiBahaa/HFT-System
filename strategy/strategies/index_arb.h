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
    // IndexArb — ETF / index arbitrage
    //
    // Computes an ETF's fair value from a weighted basket of constituents
    // and emits signals when the ETF's market mid deviates from fair value
    // by more than basis_threshold.
    // =========================================================================
    template <size_t MaxConstituents = 32>
    class IndexArb : public StrategyBase<IndexArb<MaxConstituents>> {
        friend class StrategyBase<IndexArb<MaxConstituents>>;

    public:
        struct Config {
            InstrumentId etf_instrument_id{0};
            uint32_t     num_constituents{0};
            std::array<InstrumentId, MaxConstituents> constituent_ids{};
            std::array<double,       MaxConstituents> constituent_weights{};
            Price        basis_threshold{50};      // Fixed-point deviation trigger
            Quantity     trade_qty{100};
        };

    private:
        Config config_{};

        // Last known mids per instrument
        Price etf_mid_{0};
        std::array<Price, MaxConstituents> constituent_mids_{};

        [[nodiscard]] Price compute_fair_value() const noexcept {
            double fv = 0.0;
            for (uint32_t i = 0; i < config_.num_constituents; ++i) {
                if (constituent_mids_[i] == 0) return 0;   // not warm
                fv += config_.constituent_weights[i] *
                      static_cast<double>(constituent_mids_[i]);
            }
            return static_cast<Price>(fv);
        }

        [[nodiscard]] int find_constituent(InstrumentId id) const noexcept {
            for (uint32_t i = 0; i < config_.num_constituents; ++i) {
                if (config_.constituent_ids[i] == id) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        Signal on_book_update_impl(InstrumentId id, const BookSignal& sig) noexcept {
            if (sig.mid_price == 0) return {};

            if (id == config_.etf_instrument_id) {
                etf_mid_ = sig.mid_price;
            } else {
                int idx = find_constituent(id);
                if (idx < 0) return {};
                constituent_mids_[static_cast<size_t>(idx)] = sig.mid_price;
            }

            if (etf_mid_ == 0) return {};
            Price fair = compute_fair_value();
            if (fair == 0) return {};

            Price basis = etf_mid_ - fair;
            if (std::abs(basis) < config_.basis_threshold) return {};

            Signal out{};
            out.instrument_id = config_.etf_instrument_id;
            out.quantity = config_.trade_qty;
            out.price = 0;
            out.urgency = std::min(1.0,
                std::abs(static_cast<double>(basis)) /
                (4.0 * static_cast<double>(config_.basis_threshold)));

            if (basis > 0) {
                // ETF rich -> sell ETF, buy constituents
                out.action = Action::SELL;
                out.side   = Side::SELL;
            } else {
                out.action = Action::BUY;
                out.side   = Side::BUY;
            }
            return out;
        }

        void on_execution_report_impl(const ExecutionReport&) noexcept {}

        void initialize_impl() {
            etf_mid_ = 0;
            constituent_mids_.fill(0);
        }

        [[nodiscard]] std::string_view name_impl() const noexcept {
            return "IndexArb";
        }

    public:
        explicit IndexArb(const Config& cfg) : config_(cfg) {}

        [[nodiscard]] Price fair_value() const noexcept { return compute_fair_value(); }
        [[nodiscard]] Price etf_mid() const noexcept { return etf_mid_; }
        [[nodiscard]] const Config& config() const noexcept { return config_; }
    };

} // namespace hft::strategy
