#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "core/types.h"
#include "core/market_data.h"

namespace hft::telemetry {

    using namespace hft::core;

    // =========================================================================
    // TradeLogger — audit trail of every order request, execution report, and
    // cancellation for compliance and post-trade analysis.
    //
    // Entries are appended to an in-memory ring (fixed capacity, wraps) and
    // can be flushed in either a compact binary format (for replay) or a
    // human-readable text format (for compliance export).
    //
    // Hot-path cost: one std::memcpy into a 128-byte slot. Flushing is done
    // off-path by a telemetry thread.
    // =========================================================================

    enum class TradeEventType : uint8_t {
        ORDER_SENT      = 1,
        ORDER_ACK       = 2,
        ORDER_FILLED    = 3,
        ORDER_PARTIAL   = 4,
        ORDER_CANCELLED = 5,
        ORDER_REJECTED  = 6,
        ORDER_REPLACED  = 7
    };

    struct alignas(128) TradeEvent {
        TimestampNs         timestamp{0};
        TradeEventType      type{TradeEventType::ORDER_SENT};
        Side                side{Side::UNKNOWN};
        InstrumentId        instrument_id{0};
        OrderId             cl_ord_id{0};
        OrderId             order_id{0};
        Price               price{0};
        Quantity             quantity{0};
        Quantity             filled_qty{0};
        Quantity             leaves_qty{0};
        VenueId             venue{VenueId::UNKNOWN};
        uint8_t             _pad[67]{};
    };
    static_assert(sizeof(TradeEvent) == 128, "TradeEvent must be 128 bytes");

    class TradeLogger {
    public:
        explicit TradeLogger(size_t capacity = 65536)
            : capacity_(capacity), ring_(capacity) {}

        void log_order(const OrderRequest& req) noexcept {
            TradeEvent e{};
            e.timestamp     = req.timestamp;
            e.type          = TradeEventType::ORDER_SENT;
            e.side          = req.side;
            e.instrument_id = req.instrument_id;
            e.cl_ord_id     = req.client_order_id;
            e.price         = req.price;
            e.quantity      = req.quantity;
            append(e);
        }

        void log_execution(const ExecutionReport& er) noexcept {
            TradeEvent e{};
            e.timestamp     = er.timestamp;
            switch (er.exec_type) {
                case ExecType::NEW:       e.type = TradeEventType::ORDER_ACK;       break;
                case ExecType::PARTIAL:   e.type = TradeEventType::ORDER_PARTIAL;   break;
                case ExecType::FILL:      e.type = TradeEventType::ORDER_FILLED;    break;
                case ExecType::CANCELLED: e.type = TradeEventType::ORDER_CANCELLED; break;
                case ExecType::REPLACED:  e.type = TradeEventType::ORDER_REPLACED;  break;
                case ExecType::REJECTED:  e.type = TradeEventType::ORDER_REJECTED;  break;
            }
            e.side          = er.side;
            e.instrument_id = er.instrument_id;
            e.cl_ord_id     = er.cl_ord_id;
            e.order_id      = er.order_id;
            e.price         = er.price;
            e.filled_qty    = er.filled_qty;
            e.leaves_qty    = er.leaves_qty;
            e.venue         = er.venue;
            append(e);
        }

        void log_cancel(OrderId cl_ord_id, TimestampNs ts) noexcept {
            TradeEvent e{};
            e.timestamp = ts;
            e.type      = TradeEventType::ORDER_CANCELLED;
            e.cl_ord_id = cl_ord_id;
            append(e);
        }

        [[nodiscard]] size_t size() const noexcept {
            return (count_ < capacity_) ? count_ : capacity_;
        }

        [[nodiscard]] size_t total_logged() const noexcept { return count_; }

        [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

        [[nodiscard]] bool wrapped() const noexcept { return count_ > capacity_; }

        // Snapshot all currently-held entries in insertion order
        [[nodiscard]] std::vector<TradeEvent> snapshot() const {
            std::vector<TradeEvent> out;
            size_t n = size();
            out.reserve(n);
            if (!wrapped()) {
                for (size_t i = 0; i < n; ++i) out.push_back(ring_[i]);
            } else {
                size_t start = count_ % capacity_;
                for (size_t i = 0; i < n; ++i) {
                    out.push_back(ring_[(start + i) % capacity_]);
                }
            }
            return out;
        }

        // Write all currently-held entries to a binary file (128-byte records)
        bool flush_binary(const char* path) const {
            std::FILE* f = std::fopen(path, "wb");
            if (!f) return false;
            auto snap = snapshot();
            std::fwrite(snap.data(), sizeof(TradeEvent), snap.size(), f);
            std::fclose(f);
            return true;
        }

        // Format all currently-held entries as CSV for compliance export
        std::string to_csv() const {
            std::string out;
            out.reserve(size() * 80);
            out.append("timestamp,type,side,instrument,cl_ord_id,order_id,price,qty,filled,leaves\n");
            char line[192];
            auto snap = snapshot();
            for (const auto& e : snap) {
                int n = std::snprintf(line, sizeof(line),
                    "%llu,%u,%u,%u,%llu,%llu,%lld,%lld,%lld,%lld\n",
                    static_cast<unsigned long long>(e.timestamp),
                    static_cast<unsigned>(e.type),
                    static_cast<unsigned>(e.side),
                    static_cast<unsigned>(e.instrument_id),
                    static_cast<unsigned long long>(e.cl_ord_id),
                    static_cast<unsigned long long>(e.order_id),
                    static_cast<long long>(e.price),
                    static_cast<long long>(e.quantity),
                    static_cast<long long>(e.filled_qty),
                    static_cast<long long>(e.leaves_qty));
                if (n > 0) out.append(line, static_cast<size_t>(n));
            }
            return out;
        }

        void clear() noexcept { count_ = 0; }

    private:
        void append(const TradeEvent& e) noexcept {
            ring_[count_ % capacity_] = e;
            ++count_;
        }

        size_t                  capacity_;
        size_t                  count_{0};
        std::vector<TradeEvent> ring_;
    };

} // namespace hft::telemetry
