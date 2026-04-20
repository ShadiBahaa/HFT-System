#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <functional>
#include <optional>
#include "core/types.h"
#include "core/clock.h"
#include "core/market_data.h"
#include "strategy/strategy_interface.h"
#include "feed/order_book.h"

namespace hft::persistence {

    using namespace hft::core;

    // Replay configuration
    struct ReplayConfig {
        uint64_t start_ns{0};
        uint64_t end_ns{UINT64_MAX};
        double speed_multiplier{0.0};      // 0.0 = as fast as possible, 1.0 = real-time
        bool simulate_fills{false};
    };

    // Simulated exchange latency model
    class LatencyModel {
        uint64_t mean_latency_ns_{1000};   // 1μs default
        uint64_t jitter_ns_{200};          // ±200ns jitter

        uint32_t lfsr_{0xDEADBEEF};        // simple PRNG state

        uint32_t next_random() noexcept {
            // Simple xorshift32
            lfsr_ ^= lfsr_ << 13;
            lfsr_ ^= lfsr_ >> 17;
            lfsr_ ^= lfsr_ << 5;
            return lfsr_;
        }

    public:
        void set_latency(uint64_t mean_ns, uint64_t jitter_ns) noexcept {
            mean_latency_ns_ = mean_ns;
            jitter_ns_ = jitter_ns;
        }

        // Simulate a fill based on the signal and current book state
        std::optional<ExecutionReport> simulate_fill(
            const Signal& signal,
            const feed::OrderBook<>& book,
            uint64_t current_ns) noexcept
        {
            if (signal.action == Action::NONE) return std::nullopt;

            // Check if the order would fill at current book prices
            Price fill_price = 0;
            if (signal.action == Action::BUY) {
                fill_price = book.best_ask();
                if (fill_price == 0) return std::nullopt;
                if (signal.price > 0 && signal.price < fill_price) return std::nullopt;
            } else if (signal.action == Action::SELL) {
                fill_price = book.best_bid();
                if (fill_price == 0) return std::nullopt;
                if (signal.price > 0 && signal.price > fill_price) return std::nullopt;
            } else {
                return std::nullopt;
            }

            // Simulate latency
            uint64_t latency = mean_latency_ns_;
            if (jitter_ns_ > 0) {
                latency += next_random() % (jitter_ns_ * 2) - jitter_ns_;
            }

            ExecutionReport report{};
            report.exec_type = ExecType::FILL;
            report.side = (signal.action == Action::BUY) ? Side::BUY : Side::SELL;
            report.instrument_id = signal.instrument_id;
            report.price = fill_price;
            report.filled_qty = signal.quantity;
            report.leaves_qty = 0;
            report.timestamp = current_ns + latency;

            return report;
        }
    };

    // Binary tick data stored in a flat array
    struct TickRecord {
        uint64_t timestamp_ns;
        MarketUpdate update;
    };

    // Iterator over tick records (can be backed by memory-mapped file or vector)
    class TickIterator {
        const TickRecord* data_{nullptr};
        size_t size_{0};
        size_t pos_{0};

    public:
        TickIterator() = default;

        TickIterator(const TickRecord* data, size_t size, uint64_t start_ns = 0)
            : data_(data), size_(size), pos_(0)
        {
            // Seek to start timestamp
            while (pos_ < size_ && data_[pos_].timestamp_ns < start_ns) {
                ++pos_;
            }
        }

        [[nodiscard]] bool valid() const noexcept {
            return data_ && pos_ < size_;
        }

        [[nodiscard]] uint64_t timestamp() const noexcept {
            return valid() ? data_[pos_].timestamp_ns : UINT64_MAX;
        }

        [[nodiscard]] const MarketUpdate& update() const noexcept {
            return data_[pos_].update;
        }

        TickIterator& operator++() noexcept {
            if (pos_ < size_) ++pos_;
            return *this;
        }

        [[nodiscard]] size_t position() const noexcept { return pos_; }
        [[nodiscard]] size_t total() const noexcept { return size_; }
    };

    // Market replay engine — replays tick data through a strategy
    // Uses the same strategy interface as production (IStrategy)
    class MarketReplay {
        SimulatedClock clock_;
        LatencyModel latency_model_;
        feed::OrderBook<> book_;

        uint64_t ticks_replayed_{0};
        uint64_t signals_generated_{0};
        uint64_t fills_simulated_{0};

    public:
        void set_latency_model(uint64_t mean_ns, uint64_t jitter_ns) noexcept {
            latency_model_.set_latency(mean_ns, jitter_ns);
        }

        // Run replay with any strategy exposing on_book_update / on_execution_report
        // (works with IStrategy virtual dispatch AND StrategyBase<T> CRTP)
        template <typename Strategy>
        void run(Strategy& strat,
                 const TickRecord* data, size_t data_size,
                 const ReplayConfig& config = {})
        {
            TickIterator it(data, data_size, config.start_ns);

            while (it.valid() && it.timestamp() <= config.end_ns) {
                clock_.set(it.timestamp());

                const auto& update = it.update();
                book_.apply(update);

                BookSignal bsig = book_.to_signal(update.instrument_id, it.timestamp());
                Signal signal = strat.on_book_update(update.instrument_id, bsig);
                ++ticks_replayed_;

                if (signal.action != Action::NONE) {
                    ++signals_generated_;

                    if (config.simulate_fills) {
                        auto fill = latency_model_.simulate_fill(
                            signal, book_, it.timestamp());
                        if (fill.has_value()) {
                            strat.on_execution_report(fill.value());
                            ++fills_simulated_;
                        }
                    }
                }

                ++it;
            }
        }

        [[nodiscard]] uint64_t ticks_replayed() const noexcept { return ticks_replayed_; }
        [[nodiscard]] uint64_t signals_generated() const noexcept { return signals_generated_; }
        [[nodiscard]] uint64_t fills_simulated() const noexcept { return fills_simulated_; }
        [[nodiscard]] const SimulatedClock& clock() const noexcept { return clock_; }
        [[nodiscard]] const feed::OrderBook<>& book() const noexcept { return book_; }

        void reset() noexcept {
            ticks_replayed_ = 0;
            signals_generated_ = 0;
            fills_simulated_ = 0;
            clock_.set(0);
            book_.clear();
        }
    };

} // namespace hft::persistence
