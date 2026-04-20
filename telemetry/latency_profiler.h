#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <array>
#include <atomic>
#include <algorithm>
#include "core/clock.h"

namespace hft::telemetry {

    using namespace hft::core;

    // === HdrHistogram-compatible Latency Tracker ===
    //
    // O(1) record, O(1) percentile query, constant memory (~130KB).
    // Tracks values from 1ns to 10 seconds with 3 significant figures.
    // Self-contained — no external HdrHistogram library needed.

    class LatencyTracker {
        // Sub-bucket structure: linear within each power-of-2 range
        // Gives 3 significant figures of precision
        static constexpr int SUB_BUCKET_BITS = 10;            // 1024 sub-buckets per magnitude
        static constexpr int SUB_BUCKET_COUNT = 1 << SUB_BUCKET_BITS;
        static constexpr int SUB_BUCKET_MASK = SUB_BUCKET_COUNT - 1;
        static constexpr int BUCKET_COUNT = 24;               // covers 1ns to ~16 seconds
        static constexpr int TOTAL_COUNTS = BUCKET_COUNT * SUB_BUCKET_COUNT;

        std::array<uint64_t, TOTAL_COUNTS> counts_{};
        uint64_t total_count_{0};
        uint64_t min_value_{UINT64_MAX};
        uint64_t max_value_{0};

        [[nodiscard]] static int bucket_index(uint64_t value) noexcept {
            if (value < SUB_BUCKET_COUNT) return 0;
            int leading = 63 - __builtin_clzll(value);
            return leading - (SUB_BUCKET_BITS - 1);
        }

        [[nodiscard]] static int sub_bucket_index(uint64_t value, int bucket) noexcept {
            return static_cast<int>(value >> bucket);
        }

        [[nodiscard]] static int count_index(int bucket, int sub_bucket) noexcept {
            return bucket * SUB_BUCKET_COUNT + (sub_bucket & SUB_BUCKET_MASK);
        }

        [[nodiscard]] static uint64_t value_from_index(int bucket, int sub_bucket) noexcept {
            return static_cast<uint64_t>(sub_bucket) << bucket;
        }

    public:
        void record(uint64_t value_ns) noexcept {
            if (value_ns == 0) value_ns = 1;
            int bucket = bucket_index(value_ns);
            if (bucket >= BUCKET_COUNT) bucket = BUCKET_COUNT - 1;
            int sub = sub_bucket_index(value_ns, bucket);
            int idx = count_index(bucket, sub);
            if (idx >= 0 && idx < TOTAL_COUNTS) {
                ++counts_[static_cast<size_t>(idx)];
            }
            ++total_count_;
            if (value_ns < min_value_) min_value_ = value_ns;
            if (value_ns > max_value_) max_value_ = value_ns;
        }

        [[nodiscard]] uint64_t percentile(double p) const noexcept {
            if (total_count_ == 0) return 0;
            double target = std::ceil(p / 100.0 * static_cast<double>(total_count_));
            uint64_t cumulative = 0;

            for (int bucket = 0; bucket < BUCKET_COUNT; ++bucket) {
                for (int sub = 0; sub < SUB_BUCKET_COUNT; ++sub) {
                    int idx = count_index(bucket, sub);
                    cumulative += counts_[static_cast<size_t>(idx)];
                    if (static_cast<double>(cumulative) >= target) {
                        return value_from_index(bucket, sub);
                    }
                }
            }
            return max_value_;
        }

        [[nodiscard]] uint64_t min() const noexcept {
            return total_count_ > 0 ? min_value_ : 0;
        }

        [[nodiscard]] uint64_t max() const noexcept { return max_value_; }
        [[nodiscard]] uint64_t count() const noexcept { return total_count_; }

        [[nodiscard]] double mean() const noexcept {
            if (total_count_ == 0) return 0.0;
            double sum = 0.0;
            for (int bucket = 0; bucket < BUCKET_COUNT; ++bucket) {
                for (int sub = 0; sub < SUB_BUCKET_COUNT; ++sub) {
                    int idx = count_index(bucket, sub);
                    uint64_t c = counts_[static_cast<size_t>(idx)];
                    if (c > 0) {
                        sum += static_cast<double>(value_from_index(bucket, sub))
                             * static_cast<double>(c);
                    }
                }
            }
            return sum / static_cast<double>(total_count_);
        }

        void reset() noexcept {
            counts_.fill(0);
            total_count_ = 0;
            min_value_ = UINT64_MAX;
            max_value_ = 0;
        }
    };

    // === Lightweight TracePoint for distributed tracing ===
    //
    // No span creation, no context propagation — just timestamps + sequence IDs.
    // Correlation is done offline.

    struct TracePoint {
        uint64_t sequence_id;       // monotonic per trading day
        uint64_t tsc_timestamp;     // RDTSC value
        uint16_t component_id;      // FEED_HANDLER=0, BOOK_BUILDER=1, etc.
        uint16_t event_type;        // ENTER=0, EXIT=1, CUSTOM=2+
        uint32_t extra;             // instrument_id or order_id
    };
    static_assert(sizeof(TracePoint) == 24);

    enum class ComponentId : uint16_t {
        FEED_HANDLER = 0,
        BOOK_BUILDER = 1,
        STRATEGY_ENGINE = 2,
        PRE_TRADE_RISK = 3,
        OMS = 4,
        SMART_ROUTER = 5,
        GATEWAY = 6,
        WAL_WRITER = 7
    };

    enum class EventType : uint16_t {
        ENTER = 0,
        EXIT = 1,
        SIGNAL_GENERATED = 2,
        ORDER_SUBMITTED = 3,
        FILL_RECEIVED = 4,
        RISK_CHECKED = 5
    };

    // === Per-Component Latency Profiler ===
    //
    // Records RDTSC timestamps at component boundaries on the hot path.
    // After the chain completes, deltas are recorded into per-component histograms.

    class LatencyProfiler {
    public:
        static constexpr int MAX_COMPONENTS = 8;

        struct TimestampChain {
            std::array<uint64_t, MAX_COMPONENTS + 1> stamps{};
            int count{0};

            void mark() noexcept {
                if (count <= MAX_COMPONENTS) {
                    stamps[static_cast<size_t>(count++)] = rdtsc_end();
                }
            }

            void reset() noexcept { count = 0; }
        };

    private:
        struct Component {
            const char* name{nullptr};
            LatencyTracker hist;
        };

        std::array<Component, MAX_COMPONENTS> components_{};
        LatencyTracker total_hist_;
        TSCCalibration* cal_{nullptr};
        int num_components_{0};

    public:
        void set_calibration(TSCCalibration* cal) noexcept { cal_ = cal; }

        void add_component(const char* name) noexcept {
            if (num_components_ < MAX_COMPONENTS) {
                components_[static_cast<size_t>(num_components_++)].name = name;
            }
        }

        void start(TimestampChain& chain) const noexcept {
            chain.reset();
            chain.mark();
        }

        void checkpoint(TimestampChain& chain) const noexcept {
            chain.mark();
        }

        void finish(TimestampChain& chain) noexcept {
            chain.mark();
            if (!cal_ || chain.count < 2) return;

            for (int i = 1; i < chain.count && (i - 1) < num_components_; ++i) {
                uint64_t delta_ns = cal_->tsc_to_ns(chain.stamps[static_cast<size_t>(i)])
                                  - cal_->tsc_to_ns(chain.stamps[static_cast<size_t>(i - 1)]);
                components_[static_cast<size_t>(i - 1)].hist.record(delta_ns);
            }

            uint64_t total = cal_->tsc_to_ns(chain.stamps[static_cast<size_t>(chain.count - 1)])
                           - cal_->tsc_to_ns(chain.stamps[0]);
            total_hist_.record(total);
        }

        [[nodiscard]] const LatencyTracker& component_hist(int idx) const noexcept {
            return components_[static_cast<size_t>(idx)].hist;
        }

        [[nodiscard]] const char* component_name(int idx) const noexcept {
            return components_[static_cast<size_t>(idx)].name;
        }

        [[nodiscard]] int num_components() const noexcept { return num_components_; }

        [[nodiscard]] const LatencyTracker& total_hist() const noexcept {
            return total_hist_;
        }

        void reset() noexcept {
            for (int i = 0; i < num_components_; ++i) {
                components_[static_cast<size_t>(i)].hist.reset();
            }
            total_hist_.reset();
        }
    };

} // namespace hft::telemetry
