#pragma once

#include <array>
#include <cstdint>
#include <atomic>

#include "core/types.h"
#include "core/market_data.h"
#include "core/clock.h"
#include "oms/order_types.h"
#include "risk/throttle.h"

namespace hft::oms {

    using namespace hft::core;

    // =========================================================================
    // OrderManagementSystem — order lifecycle, ID assignment, state tracking
    //
    // Pre-allocated order slots indexed by client_order_id for O(1) lookup.
    // No heap allocation on the hot path.
    //
    // If an ExchangeThrottle is attached via set_throttle(), the send-side
    // helpers (try_create_order / try_cancel) consult the token bucket BEFORE
    // touching the order slot. Orders that exceed the exchange's published
    // rate limit are rejected locally (orders_throttled_ counter) rather than
    // shipped onto the wire, where they would get us port-disconnected.
    // =========================================================================
    template <size_t MaxOrdersBits = 12>  // Default 4K orders, use 16 for production
    class OrderManagementSystem {
        static constexpr size_t MAX_ORDERS = size_t{1} << MaxOrdersBits;
        static constexpr size_t ORDER_MASK = MAX_ORDERS - 1;

        std::array<Order, MAX_ORDERS> orders_{};
        std::atomic<uint64_t> next_cl_ord_id_{1};

        // Optional rate-limit gate. Non-owning — caller controls lifetime.
        hft::risk::ExchangeThrottle* throttle_{nullptr};

        // Counters
        uint64_t orders_sent_{0};
        uint64_t orders_filled_{0};
        uint64_t orders_cancelled_{0};
        uint64_t orders_rejected_{0};
        uint64_t orders_throttled_{0};

    public:
        // Attach (or detach with nullptr) a shared rate-limit gate. The OMS
        // does not take ownership — the throttle must outlive the OMS.
        void set_throttle(hft::risk::ExchangeThrottle* t) noexcept { throttle_ = t; }
        [[nodiscard]] hft::risk::ExchangeThrottle* throttle() const noexcept { return throttle_; }

        // Rate-limited send path. Returns an empty optional (signalled via
        // bool return) when the exchange token bucket is empty; the caller
        // must not attempt to encode/ship the order in that case.
        //
        // The order slot is NOT mutated when throttled, so retry logic can
        // reissue the same ClOrdID on the next send opportunity.
        [[gnu::hot, nodiscard]]
        bool try_create_order(const OrderRequest& req, Order& out) noexcept {
            if (throttle_ && !throttle_->allow_new_order()) [[unlikely]] {
                ++orders_throttled_;
                return false;
            }
            out = create_order(req);
            return true;
        }

        // Rate-limited cancel path. Marks the target order PENDING_CANCEL
        // only if the cancel passed the throttle — otherwise leaves it alone.
        [[gnu::hot, nodiscard]]
        bool try_cancel(OrderId cl_ord_id) noexcept {
            if (throttle_ && !throttle_->allow_cancel()) [[unlikely]] {
                ++orders_throttled_;
                return false;
            }
            Order* order = find_order(cl_ord_id);
            if (!order) return false;
            order->status = OrderStatus::PENDING_CANCEL;
            order->last_update_time = rdtsc();
            return true;
        }

        // Create a new order from an OrderRequest, assign ClOrdID
        [[gnu::hot]]
        Order create_order(const OrderRequest& req) noexcept {
            uint64_t id = next_cl_ord_id_.fetch_add(1, std::memory_order_relaxed);

            Order& order = orders_[id & ORDER_MASK];
            order.client_order_id = id;
            order.exchange_order_id = 0;
            order.instrument_id = req.instrument_id;
            order.side = req.side;
            order.order_type = req.order_type;
            order.price = req.price;
            order.quantity = req.quantity;
            order.filled_qty = 0;
            order.leaves_qty = req.quantity;
            order.status = OrderStatus::PENDING_NEW;
            order.venue = VenueId::UNKNOWN;
            order.create_time = rdtsc();
            order.last_update_time = order.create_time;

            ++orders_sent_;
            return order;
        }

        // Process an execution report from the exchange
        [[gnu::hot]]
        void on_execution_report(const ExecutionReport& report) noexcept {
            Order& order = orders_[report.cl_ord_id & ORDER_MASK];
            order.last_update_time = rdtsc();

            switch (report.exec_type) {
                case ExecType::NEW:
                    order.status = OrderStatus::OPEN;
                    order.exchange_order_id = report.order_id;
                    break;

                case ExecType::PARTIAL:
                    order.status = OrderStatus::PARTIALLY_FILLED;
                    order.filled_qty += report.filled_qty;
                    order.leaves_qty = report.leaves_qty;
                    break;

                case ExecType::FILL:
                    order.status = OrderStatus::FILLED;
                    order.filled_qty += report.filled_qty;
                    order.leaves_qty = 0;
                    ++orders_filled_;
                    break;

                case ExecType::CANCELLED:
                    order.status = OrderStatus::CANCELLED;
                    order.leaves_qty = 0;
                    ++orders_cancelled_;
                    break;

                case ExecType::REJECTED:
                    order.status = OrderStatus::REJECTED;
                    order.leaves_qty = 0;
                    ++orders_rejected_;
                    break;

                case ExecType::REPLACED:
                    order.price = report.price;
                    order.leaves_qty = report.leaves_qty;
                    break;
            }
        }

        // Look up an order by client order ID
        [[nodiscard]] const Order* find_order(OrderId cl_ord_id) const noexcept {
            const auto& order = orders_[cl_ord_id & ORDER_MASK];
            if (order.client_order_id == cl_ord_id) return &order;
            return nullptr;
        }

        [[nodiscard]] Order* find_order(OrderId cl_ord_id) noexcept {
            auto& order = orders_[cl_ord_id & ORDER_MASK];
            if (order.client_order_id == cl_ord_id) return &order;
            return nullptr;
        }

        // Counters for monitoring
        [[nodiscard]] uint64_t orders_sent() const noexcept { return orders_sent_; }
        [[nodiscard]] uint64_t orders_filled() const noexcept { return orders_filled_; }
        [[nodiscard]] uint64_t orders_cancelled() const noexcept { return orders_cancelled_; }
        [[nodiscard]] uint64_t orders_rejected() const noexcept { return orders_rejected_; }
        [[nodiscard]] uint64_t orders_throttled() const noexcept { return orders_throttled_; }
        [[nodiscard]] uint64_t next_cl_ord_id() const noexcept {
            return next_cl_ord_id_.load(std::memory_order_relaxed);
        }
    };

} // namespace hft::oms
