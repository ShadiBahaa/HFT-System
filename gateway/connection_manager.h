#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include "core/types.h"

namespace hft::gateway {

    using namespace hft::core;

    // Exchange connection state machine from design doc (Section 6.3)
    enum class ConnState : uint8_t {
        DISCONNECTED,
        CONNECTING,
        LOGGING_ON,
        ACTIVE,
        RESENDING,
        DRAINING,
        DISCONNECTING
    };

    inline const char* to_string(ConnState s) noexcept {
        switch (s) {
            case ConnState::DISCONNECTED:   return "DISCONNECTED";
            case ConnState::CONNECTING:     return "CONNECTING";
            case ConnState::LOGGING_ON:     return "LOGGING_ON";
            case ConnState::ACTIVE:         return "ACTIVE";
            case ConnState::RESENDING:      return "RESENDING";
            case ConnState::DRAINING:       return "DRAINING";
            case ConnState::DISCONNECTING:  return "DISCONNECTING";
            default:                        return "UNKNOWN";
        }
    }

    class ExchangeConnection {
    public:
        static constexpr uint32_t MAX_RETRIES = 3;
        static constexpr std::array<uint32_t, 3> RETRY_DELAYS_MS = {100, 500, 2000};

        using DisconnectCallback = std::function<void()>;
        using AlertCallback = std::function<void(const char*)>;

    private:
        ConnState state_{ConnState::DISCONNECTED};
        uint32_t retry_count_{0};

        // FIX sequence number tracking
        uint32_t expected_seq_in_{1};
        uint32_t next_seq_out_{1};

        DisconnectCallback on_max_retries_;
        AlertCallback on_alert_;

    public:
        ExchangeConnection() = default;

        void set_disconnect_handler(DisconnectCallback cb) { on_max_retries_ = std::move(cb); }
        void set_alert_handler(AlertCallback cb) { on_alert_ = std::move(cb); }

        [[nodiscard]] ConnState state() const noexcept { return state_; }
        [[nodiscard]] uint32_t retry_count() const noexcept { return retry_count_; }
        [[nodiscard]] uint32_t expected_seq_in() const noexcept { return expected_seq_in_; }
        [[nodiscard]] uint32_t next_seq_out() const noexcept { return next_seq_out_; }

        // Initiate connection
        bool connect() noexcept {
            if (state_ != ConnState::DISCONNECTED) return false;
            state_ = ConnState::CONNECTING;
            return true;
        }

        // Connection established — begin logon
        bool on_connected() noexcept {
            if (state_ != ConnState::CONNECTING) return false;
            state_ = ConnState::LOGGING_ON;
            return true;
        }

        // Logon accepted by exchange
        // NOTE: retry_count_ is NOT reset here — it tracks cumulative failures
        // until explicitly cleared via clear_retry_count() or reset().
        // This lets callers detect a flapping connection even if each attempt
        // briefly reaches ACTIVE before dropping again.
        bool on_logon_accepted() noexcept {
            if (state_ != ConnState::LOGGING_ON) return false;
            state_ = ConnState::ACTIVE;
            return true;
        }

        // Caller can explicitly clear retry_count once the session is deemed
        // stable (e.g., after N seconds of uninterrupted activity).
        void clear_retry_count() noexcept { retry_count_ = 0; }

        // Handle disconnection with retry logic
        void on_disconnect() noexcept {
            state_ = ConnState::DISCONNECTED;
            if (retry_count_ < MAX_RETRIES) {
                ++retry_count_;
            } else {
                // Escalate: kill switch, alert on-call
                if (on_max_retries_) on_max_retries_();
                if (on_alert_) on_alert_("Exchange connection lost after max retries");
            }
        }

        // Get delay for next retry (0 if no more retries)
        [[nodiscard]] uint32_t next_retry_delay_ms() const noexcept {
            if (retry_count_ == 0 || retry_count_ > MAX_RETRIES) return 0;
            return RETRY_DELAYS_MS[retry_count_ - 1];
        }

        [[nodiscard]] bool can_retry() const noexcept {
            return retry_count_ < MAX_RETRIES;
        }

        // Handle FIX sequence gap — triggers ResendRequest
        bool on_sequence_gap(uint32_t received, uint32_t expected) noexcept {
            if (state_ != ConnState::ACTIVE) return false;
            if (received <= expected) return false;  // not a gap
            state_ = ConnState::RESENDING;
            // Gap range: [expected, received - 1]
            return true;
        }

        // Resend complete — back to active
        bool on_resend_complete() noexcept {
            if (state_ != ConnState::RESENDING) return false;
            state_ = ConnState::ACTIVE;
            return true;
        }

        // Begin graceful shutdown
        bool drain() noexcept {
            if (state_ != ConnState::ACTIVE) return false;
            state_ = ConnState::DRAINING;
            return true;
        }

        // Initiate disconnection
        bool disconnect() noexcept {
            state_ = ConnState::DISCONNECTING;
            return true;
        }

        // Disconnection complete
        void on_disconnected() noexcept {
            state_ = ConnState::DISCONNECTED;
        }

        // Track outgoing sequence
        uint32_t allocate_seq_out() noexcept {
            return next_seq_out_++;
        }

        // Validate incoming sequence
        bool check_seq_in(uint32_t seq) noexcept {
            if (seq == expected_seq_in_) {
                ++expected_seq_in_;
                return true;
            }
            if (seq > expected_seq_in_) {
                on_sequence_gap(seq, expected_seq_in_);
                expected_seq_in_ = seq + 1;
                return true;
            }
            return false;  // duplicate or old
        }

        // Reset for new session
        void reset() noexcept {
            state_ = ConnState::DISCONNECTED;
            retry_count_ = 0;
            expected_seq_in_ = 1;
            next_seq_out_ = 1;
        }

        [[nodiscard]] bool is_active() const noexcept {
            return state_ == ConnState::ACTIVE;
        }
    };

} // namespace hft::gateway
