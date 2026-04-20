#pragma once

#include <cstdint>
#include <atomic>

namespace hft::telemetry {

    // =========================================================================
    // HealthMonitor — aggregates system metrics and produces a single
    // "degradation level" that downstream components (e.g. a kill switch or
    // strategy throttle) can react to.
    //
    // The monitor runs off the hot path — the hot path publishes raw metrics
    // via atomic stores and a telemetry thread calls evaluate() periodically.
    // =========================================================================

    enum class DegradationLevel : uint8_t {
        NORMAL      = 0,   // All green
        WARNING     = 1,   // One soft metric over budget — reduce aggressiveness
        DEGRADED    = 2,   // Multiple breaches or one hard breach — go passive
        CRITICAL    = 3    // Halt trading — engage kill switch
    };

    struct HealthMetrics {
        uint64_t    latency_p99_ns{0};        // Tick-to-trade p99 latency
        double      packet_loss_rate{0.0};    // Fraction of packets dropped
        bool        exchange_disconnect{false};
        double      pnl_drawdown{0.0};        // Fraction of peak equity lost
        uint64_t    wal_backlog{0};           // Pending WAL writes
        uint64_t    risk_rejects{0};          // Orders rejected by pre-trade risk
    };

    struct HealthThresholds {
        // Soft / hard budgets for each metric
        uint64_t    latency_warn_ns{1'000'000};    // 1ms
        uint64_t    latency_crit_ns{10'000'000};   // 10ms
        double      loss_warn{0.001};              // 0.1%
        double      loss_crit{0.01};               // 1%
        double      drawdown_warn{0.02};           // 2%
        double      drawdown_crit{0.05};           // 5%
        uint64_t    wal_backlog_warn{1024};
        uint64_t    wal_backlog_crit{8192};
    };

    class HealthMonitor {
    public:
        HealthMonitor() = default;

        void set_thresholds(const HealthThresholds& t) noexcept { thresholds_ = t; }
        [[nodiscard]] const HealthThresholds& thresholds() const noexcept { return thresholds_; }

        void update(const HealthMetrics& m) noexcept {
            metrics_ = m;
            current_level_.store(compute_level(m), std::memory_order_release);
        }

        [[nodiscard]] DegradationLevel level() const noexcept {
            return current_level_.load(std::memory_order_acquire);
        }

        [[nodiscard]] const HealthMetrics& metrics() const noexcept { return metrics_; }

        // Convenience: would a given metric snapshot trigger the kill switch?
        [[nodiscard]] bool should_halt(const HealthMetrics& m) const noexcept {
            return compute_level(m) == DegradationLevel::CRITICAL;
        }

        // Re-evaluate stored metrics (useful if thresholds changed)
        DegradationLevel evaluate() noexcept {
            auto lvl = compute_level(metrics_);
            current_level_.store(lvl, std::memory_order_release);
            return lvl;
        }

    private:
        [[nodiscard]] DegradationLevel compute_level(const HealthMetrics& m) const noexcept {
            // Any hard breach → CRITICAL
            if (m.exchange_disconnect) return DegradationLevel::CRITICAL;
            if (m.latency_p99_ns > thresholds_.latency_crit_ns) return DegradationLevel::CRITICAL;
            if (m.packet_loss_rate > thresholds_.loss_crit) return DegradationLevel::CRITICAL;
            if (m.pnl_drawdown > thresholds_.drawdown_crit) return DegradationLevel::CRITICAL;
            if (m.wal_backlog > thresholds_.wal_backlog_crit) return DegradationLevel::CRITICAL;

            // Count soft breaches
            int soft = 0;
            if (m.latency_p99_ns > thresholds_.latency_warn_ns) ++soft;
            if (m.packet_loss_rate > thresholds_.loss_warn) ++soft;
            if (m.pnl_drawdown > thresholds_.drawdown_warn) ++soft;
            if (m.wal_backlog > thresholds_.wal_backlog_warn) ++soft;

            if (soft >= 2) return DegradationLevel::DEGRADED;
            if (soft == 1) return DegradationLevel::WARNING;
            return DegradationLevel::NORMAL;
        }

        HealthThresholds    thresholds_{};
        HealthMetrics       metrics_{};
        std::atomic<DegradationLevel> current_level_{DegradationLevel::NORMAL};
    };

} // namespace hft::telemetry
