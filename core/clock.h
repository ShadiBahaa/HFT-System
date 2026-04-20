#pragma once

#include <cstdint>
#include <atomic>
#include <cmath>
#include <chrono>
#include <thread>

// Cross-platform intrinsic includes
#ifdef _WIN32
#include <intrin.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Windows headers pollute the global namespace with macros that clash with
// our enumerators. Undefine the worst offenders after inclusion.
#ifdef DELETE
#undef DELETE
#endif
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <x86intrin.h>
#include <time.h>
#endif

namespace hft::core {

    // === Hardware Timestamping (RDTSC) ===
    //
    // Measure time in CPU cycles without making system calls.
    // Extremely low latency (~20 cycles).

    // Start measurement: lfence ensures all prior instructions retire first
    // Compiler fence memory clobber prevents reordering
    [[nodiscard]] inline uint64_t rdtsc_start() noexcept {
#ifdef _WIN32
        _mm_lfence();
        return __rdtsc();
#else
        uint32_t lo, hi;
        __asm__ __volatile__(
            "lfence\n\t"
            "rdtsc"
            : "=a"(lo), "=d"(hi)
            :: "memory");
        return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
    }

    // End measurement: rdtscp waits for all prior instructions to complete
    [[nodiscard]] inline uint64_t rdtsc_end() noexcept {
#ifdef _WIN32
        unsigned int aux;
        return __rdtscp(&aux);
#else
        uint32_t lo, hi, aux;
        __asm__ __volatile__(
            "rdtscp"
            : "=a"(lo), "=d"(hi), "=c"(aux)
            :: "memory");
        return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
    }

    // Generic fallback for timestamps where strict serialization is not
    // as critical (e.g., generating unique IDs)
    [[nodiscard]] inline uint64_t rdtsc() noexcept {
        return rdtsc_end();
    }

    // === TSC Calibration ===
    //
    // Convert TSC cycles to nanoseconds using calibrated frequency.
    // Calibrate at startup against high-resolution wall clock, then
    // re-calibrate every 60 seconds on telemetry thread for thermal drift (~100ppm).

    struct TSCCalibration {
        double tsc_to_ns_ratio{0.0};       // ns per tick (e.g., ~0.3 for 3.5GHz)
        uint64_t base_tsc{0};
        uint64_t base_ns{0};
        std::atomic<double> correction_factor{1.0};

        // Get current wall-clock nanoseconds (platform-specific)
        static uint64_t wall_clock_ns() noexcept {
#ifdef _WIN32
            static const double qpc_to_ns = [] {
                LARGE_INTEGER freq;
                QueryPerformanceFrequency(&freq);
                return 1'000'000'000.0 / static_cast<double>(freq.QuadPart);
            }();
            LARGE_INTEGER count;
            QueryPerformanceCounter(&count);
            return static_cast<uint64_t>(count.QuadPart * qpc_to_ns);
#else
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                 + static_cast<uint64_t>(ts.tv_nsec);
#endif
        }

        // Initial calibration — call once at startup
        void calibrate() noexcept {
            // Measure TSC frequency by sampling over a short interval
            uint64_t ns_start = wall_clock_ns();
            uint64_t tsc_start = rdtsc_end();

            // Sleep briefly to get a measurable time delta
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            uint64_t ns_end = wall_clock_ns();
            uint64_t tsc_end_val = rdtsc_end();

            uint64_t ns_delta = ns_end - ns_start;
            uint64_t tsc_delta = tsc_end_val - tsc_start;

            if (tsc_delta > 0) {
                tsc_to_ns_ratio = static_cast<double>(ns_delta)
                                / static_cast<double>(tsc_delta);
            }

            base_tsc = tsc_start;
            base_ns = ns_start;
            correction_factor.store(1.0, std::memory_order_relaxed);
        }

        // Convert a TSC value to nanoseconds
        [[nodiscard]] uint64_t tsc_to_ns(uint64_t tsc) const noexcept {
            return base_ns + static_cast<uint64_t>(
                static_cast<double>(tsc - base_tsc) * tsc_to_ns_ratio
                * correction_factor.load(std::memory_order_relaxed));
        }

        // TSC frequency in Hz (approximate)
        [[nodiscard]] double tsc_freq_hz() const noexcept {
            return (tsc_to_ns_ratio > 0.0) ? (1'000'000'000.0 / tsc_to_ns_ratio) : 0.0;
        }

        // Called every ~60s by telemetry thread to correct for thermal drift
        void recalibrate() noexcept {
            uint64_t current_ns = wall_clock_ns();
            uint64_t current_tsc = rdtsc_end();
            double actual_ratio = static_cast<double>(current_ns - base_ns)
                                / static_cast<double>(current_tsc - base_tsc);
            if (tsc_to_ns_ratio > 0.0) {
                correction_factor.store(actual_ratio / tsc_to_ns_ratio,
                                       std::memory_order_relaxed);
            }
        }
    };

    // === Clock Abstraction (for backtesting) ===

    class Clock {
    public:
        virtual ~Clock() = default;
        [[nodiscard]] virtual uint64_t now_ns() const noexcept = 0;
    };

    class WallClock : public Clock {
        TSCCalibration cal_;
    public:
        WallClock() { cal_.calibrate(); }

        [[nodiscard]] uint64_t now_ns() const noexcept override {
            return cal_.tsc_to_ns(rdtsc_end());
        }

        [[nodiscard]] const TSCCalibration& calibration() const noexcept { return cal_; }
        TSCCalibration& calibration() noexcept { return cal_; }
    };

    class SimulatedClock : public Clock {
        uint64_t current_ns_{0};
    public:
        [[nodiscard]] uint64_t now_ns() const noexcept override { return current_ns_; }
        void set(uint64_t ns) noexcept { current_ns_ = ns; }
        void advance(uint64_t delta_ns) noexcept { current_ns_ += delta_ns; }
    };

} // namespace hft::core
