#pragma once

#include <atomic>
#include <cstdint>
#include "core/types.h"

namespace hft::resilience {

    using namespace hft::core;

    // =========================================================================
    // FailoverManager — 5-state primary/secondary reconciliation machine.
    //
    // Design (from hft_system_design.md):
    //   DETECT     — heartbeat miss on primary
    //   VERIFY     — confirm via secondary channel (drop-copy feed)
    //   QUARANTINE — primary is fenced out via fencing token CAS
    //   RECONCILE  — rebuild position state from WAL + drop-copy
    //   RESUME     — secondary begins accepting orders
    //
    // Fencing via a monotonic token in shared memory prevents split-brain:
    // once the secondary bumps the token, the old primary's writes are
    // rejected by whatever holds the authoritative ledger.
    // =========================================================================

    enum class FailoverState : uint8_t {
        ACTIVE_PRIMARY      = 0,   // We are primary, all good
        DETECT              = 1,   // (Secondary) missed heartbeat
        VERIFY              = 2,   // Cross-checking via drop-copy
        QUARANTINE          = 3,   // Fenced out primary
        RECONCILE           = 4,   // Rebuilding position state
        RESUME              = 5,   // Taking over as new primary
        ACTIVE_SECONDARY    = 6    // We are secondary, primary is healthy
    };

    struct FailoverConfig {
        uint64_t    heartbeat_timeout_ns{10'000'000};   // 10ms miss → DETECT
        uint64_t    verify_timeout_ns{50'000'000};      // 50ms for drop-copy confirmation
        uint32_t    min_reconcile_entries{0};            // WAL entries required before RESUME
    };

    // -------------------------------------------------------------------------
    // HeartbeatMonitor — tracks last-seen timestamp from primary
    // -------------------------------------------------------------------------
    class HeartbeatMonitor {
    public:
        HeartbeatMonitor() = default;

        void beat(TimestampNs ts) noexcept {
            last_beat_ns_.store(ts, std::memory_order_release);
        }

        [[nodiscard]] TimestampNs last_beat() const noexcept {
            return last_beat_ns_.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool is_alive(TimestampNs now, uint64_t timeout_ns) const noexcept {
            auto last = last_beat_ns_.load(std::memory_order_acquire);
            if (last == 0) return false;
            return (now - last) < timeout_ns;
        }

    private:
        std::atomic<TimestampNs> last_beat_ns_{0};
    };

    // -------------------------------------------------------------------------
    // FencingToken — monotonic CAS-bumped token for split-brain prevention
    // -------------------------------------------------------------------------
    class FencingToken {
    public:
        FencingToken() = default;

        [[nodiscard]] uint64_t current() const noexcept {
            return token_.load(std::memory_order_acquire);
        }

        // Attempt to advance to `new_token`. Succeeds only if new_token > current.
        bool advance(uint64_t new_token) noexcept {
            auto cur = token_.load(std::memory_order_acquire);
            while (new_token > cur) {
                if (token_.compare_exchange_weak(cur, new_token,
                    std::memory_order_release, std::memory_order_acquire)) {
                    return true;
                }
            }
            return false;
        }

        // Check whether a writer holding `writer_token` is still authorized.
        [[nodiscard]] bool is_fenced(uint64_t writer_token) const noexcept {
            return writer_token < token_.load(std::memory_order_acquire);
        }

    private:
        std::atomic<uint64_t> token_{0};
    };

    // -------------------------------------------------------------------------
    // FailoverManager — state machine
    // -------------------------------------------------------------------------
    class FailoverManager {
    public:
        FailoverManager() = default;

        void set_config(const FailoverConfig& c) noexcept { config_ = c; }
        [[nodiscard]] const FailoverConfig& config() const noexcept { return config_; }

        // Role transitions
        void become_primary() noexcept { state_ = FailoverState::ACTIVE_PRIMARY; }
        void become_secondary() noexcept { state_ = FailoverState::ACTIVE_SECONDARY; }

        [[nodiscard]] FailoverState state() const noexcept { return state_; }
        [[nodiscard]] bool is_primary() const noexcept { return state_ == FailoverState::ACTIVE_PRIMARY; }

        HeartbeatMonitor& heartbeat() noexcept { return hb_; }
        [[nodiscard]] const HeartbeatMonitor& heartbeat() const noexcept { return hb_; }

        FencingToken& fencing_token() noexcept { return token_; }
        [[nodiscard]] const FencingToken& fencing_token() const noexcept { return token_; }

        // Periodic evaluation driven by the failover thread.
        // Returns the new state.
        FailoverState evaluate(TimestampNs now) noexcept {
            switch (state_) {
                case FailoverState::ACTIVE_PRIMARY:
                    // Nothing to do — primary doesn't watch itself
                    break;

                case FailoverState::ACTIVE_SECONDARY:
                    // Watch heartbeat; trigger DETECT on timeout
                    if (!hb_.is_alive(now, config_.heartbeat_timeout_ns)) {
                        state_ = FailoverState::DETECT;
                        detect_entered_ns_ = now;
                    }
                    break;

                case FailoverState::DETECT:
                    // Immediately move to VERIFY — detection confirmed
                    state_ = FailoverState::VERIFY;
                    verify_entered_ns_ = now;
                    break;

                case FailoverState::VERIFY:
                    // Wait verify_timeout to let drop-copy confirm; if heartbeat
                    // resumes, return to secondary.
                    if (hb_.is_alive(now, config_.heartbeat_timeout_ns)) {
                        state_ = FailoverState::ACTIVE_SECONDARY;
                    } else if ((now - verify_entered_ns_) >= config_.verify_timeout_ns) {
                        state_ = FailoverState::QUARANTINE;
                        // Bump fencing token — old primary's writes are now stale
                        token_.advance(token_.current() + 1);
                    }
                    break;

                case FailoverState::QUARANTINE:
                    state_ = FailoverState::RECONCILE;
                    break;

                case FailoverState::RECONCILE:
                    if (reconciled_entries_ >= config_.min_reconcile_entries) {
                        state_ = FailoverState::RESUME;
                    }
                    break;

                case FailoverState::RESUME:
                    state_ = FailoverState::ACTIVE_PRIMARY;
                    break;
            }
            return state_;
        }

        // Called by WAL replay to indicate reconciliation progress
        void mark_reconciled_entry() noexcept { ++reconciled_entries_; }
        [[nodiscard]] uint32_t reconciled_entries() const noexcept { return reconciled_entries_; }

        // Force a primary failure for testing
        void force_failover() noexcept {
            if (state_ == FailoverState::ACTIVE_SECONDARY) {
                state_ = FailoverState::DETECT;
            }
        }

    private:
        FailoverConfig      config_{};
        FailoverState       state_{FailoverState::ACTIVE_SECONDARY};
        HeartbeatMonitor    hb_{};
        FencingToken        token_{};
        TimestampNs         detect_entered_ns_{0};
        TimestampNs         verify_entered_ns_{0};
        uint32_t            reconciled_entries_{0};
    };

} // namespace hft::resilience
