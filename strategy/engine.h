#pragma once

#include <cstdint>

#include "core/types.h"
#include "core/market_data.h"
#include "core/spsc_ring.h"

namespace hft::strategy {

    using namespace hft::core;

    // =========================================================================
    // TypedStrategyEngine — CRTP-dispatched engine loop
    //
    // Busy-polls the signal ring for BookSignal updates, feeds them to the
    // strategy, and pushes resulting OrderRequests to the order ring.
    // Runs on a dedicated isolated core at 100% CPU.
    // =========================================================================
    template <typename Strategy>
    class TypedStrategyEngine {
    public:
        using SignalRing = SPSCRingBuffer<BookSignal, 65536>;
        using OrderRing  = SPSCRingBuffer<OrderRequest, 4096>;

    private:
        Strategy&   strategy_;
        SignalRing& signal_ring_;
        OrderRing&  order_ring_;
        uint64_t    signals_processed_{0};
        uint64_t    orders_generated_{0};
        bool        running_{false};

    public:
        TypedStrategyEngine(Strategy& strategy, SignalRing& signal_ring, OrderRing& order_ring)
            : strategy_(strategy), signal_ring_(signal_ring), order_ring_(order_ring) {}

        // Process a single update (non-blocking, for testing / integration)
        [[gnu::hot]] bool process_one() noexcept {
            BookSignal signal{};
            if (!signal_ring_.try_pop(signal)) return false;

            Signal result = strategy_.on_book_update(signal.instrument_id, signal);
            ++signals_processed_;

            if (result.action != Action::NONE) {
                OrderRequest req{};
                req.action = result.action;
                req.side = result.side;
                req.order_type = OrderType::LIMIT;
                req.tif = TimeInForce::DAY;
                req.instrument_id = result.instrument_id;
                req.price = result.price;
                req.quantity = result.quantity;
                order_ring_.try_push(req);
                ++orders_generated_;
            }
            return true;
        }

        // Busy-poll loop — runs forever on dedicated core
        // Call stop() from another thread to terminate
        [[gnu::hot]] void run() noexcept {
            running_ = true;
            while (running_) {
                process_one();
                // No sleep — pure spin-wait on dedicated core
            }
        }

        void stop() noexcept { running_ = false; }

        [[nodiscard]] uint64_t signals_processed() const noexcept { return signals_processed_; }
        [[nodiscard]] uint64_t orders_generated() const noexcept { return orders_generated_; }
        [[nodiscard]] bool is_running() const noexcept { return running_; }
    };

} // namespace hft::strategy
