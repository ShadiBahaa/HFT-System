#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace hft::telemetry {

    // =========================================================================
    // MetricsPublisher — lightweight registry of named metrics that can be
    // scraped by Prometheus or any text-based monitoring backend.
    //
    // Design notes:
    //   - Metric storage is a fixed-size array so registration is O(1) and
    //     lock-free once warmed up.
    //   - Counter increments are std::atomic, safe from the hot path.
    //   - Snapshot + Prometheus exposition run off-path.
    // =========================================================================

    enum class MetricType : uint8_t {
        COUNTER     = 0,
        GAUGE       = 1,
        HISTOGRAM   = 2
    };

    struct Metric {
        std::string         name;
        std::string         help;
        MetricType          type{MetricType::COUNTER};
        double              value{0.0};
    };

    class MetricsPublisher {
    public:
        static constexpr size_t MAX_METRICS = 256;

        MetricsPublisher() = default;

        // Register a metric and return its handle (index). Returns -1 if full
        // or if the name is already registered with a different type.
        int register_metric(std::string_view name, std::string_view help, MetricType type) noexcept {
            // Find by name
            for (size_t i = 0; i < count_.load(std::memory_order_acquire); ++i) {
                if (slots_[i].name_len == name.size() &&
                    std::memcmp(slots_[i].name, name.data(), name.size()) == 0) {
                    return (slots_[i].type == type) ? static_cast<int>(i) : -1;
                }
            }
            size_t idx = count_.load(std::memory_order_relaxed);
            if (idx >= MAX_METRICS) return -1;
            if (name.size() >= sizeof(slots_[idx].name)) return -1;
            if (help.size() >= sizeof(slots_[idx].help)) return -1;

            std::memcpy(slots_[idx].name, name.data(), name.size());
            slots_[idx].name[name.size()] = '\0';
            slots_[idx].name_len = name.size();
            std::memcpy(slots_[idx].help, help.data(), help.size());
            slots_[idx].help[help.size()] = '\0';
            slots_[idx].help_len = help.size();
            slots_[idx].type = type;
            slots_[idx].value.store(0.0, std::memory_order_release);

            count_.store(idx + 1, std::memory_order_release);
            return static_cast<int>(idx);
        }

        void increment(int handle, double delta = 1.0) noexcept {
            if (handle < 0 || static_cast<size_t>(handle) >= MAX_METRICS) return;
            if (static_cast<size_t>(handle) >= count_.load(std::memory_order_acquire)) return;
            auto& v = slots_[static_cast<size_t>(handle)].value;
            double cur = v.load(std::memory_order_relaxed);
            while (!v.compare_exchange_weak(cur, cur + delta, std::memory_order_release, std::memory_order_relaxed)) {}
        }

        void set(int handle, double value) noexcept {
            if (handle < 0 || static_cast<size_t>(handle) >= MAX_METRICS) return;
            if (static_cast<size_t>(handle) >= count_.load(std::memory_order_acquire)) return;
            slots_[static_cast<size_t>(handle)].value.store(value, std::memory_order_release);
        }

        [[nodiscard]] double get(int handle) const noexcept {
            if (handle < 0 || static_cast<size_t>(handle) >= MAX_METRICS) return 0.0;
            if (static_cast<size_t>(handle) >= count_.load(std::memory_order_acquire)) return 0.0;
            return slots_[static_cast<size_t>(handle)].value.load(std::memory_order_acquire);
        }

        [[nodiscard]] size_t count() const noexcept { return count_.load(std::memory_order_acquire); }

        [[nodiscard]] std::vector<Metric> snapshot() const {
            std::vector<Metric> out;
            size_t n = count_.load(std::memory_order_acquire);
            out.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                Metric m;
                m.name.assign(slots_[i].name, slots_[i].name_len);
                m.help.assign(slots_[i].help, slots_[i].help_len);
                m.type = slots_[i].type;
                m.value = slots_[i].value.load(std::memory_order_acquire);
                out.push_back(std::move(m));
            }
            return out;
        }

    private:
        struct Slot {
            char                name[64]{};
            size_t              name_len{0};
            char                help[128]{};
            size_t              help_len{0};
            MetricType          type{MetricType::COUNTER};
            std::atomic<double> value{0.0};
        };

        std::array<Slot, MAX_METRICS>   slots_{};
        std::atomic<size_t>             count_{0};
    };

    // =========================================================================
    // PrometheusExporter — formats a MetricsPublisher snapshot into the
    // Prometheus text exposition format.
    // =========================================================================
    class PrometheusExporter {
    public:
        static std::string format(const MetricsPublisher& pub) {
            std::string out;
            out.reserve(2048);
            auto snap = pub.snapshot();
            for (const auto& m : snap) {
                if (!m.help.empty()) {
                    out.append("# HELP ").append(m.name).append(" ").append(m.help).append("\n");
                }
                out.append("# TYPE ").append(m.name).append(" ").append(type_str(m.type)).append("\n");
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.9g", m.value);
                out.append(m.name).append(" ").append(buf).append("\n");
            }
            return out;
        }

    private:
        static const char* type_str(MetricType t) noexcept {
            switch (t) {
                case MetricType::COUNTER:   return "counter";
                case MetricType::GAUGE:     return "gauge";
                case MetricType::HISTOGRAM: return "histogram";
            }
            return "untyped";
        }
    };

} // namespace hft::telemetry
