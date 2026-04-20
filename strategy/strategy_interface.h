#pragma once

#include <cstdint>
#include <string_view>

#include "core/types.h"
#include "core/market_data.h"

namespace hft::strategy {

    using namespace hft::core;

    // =========================================================================
    // IStrategy — virtual dispatch interface
    // Use when: multiple strategies loaded at runtime, flexibility > performance
    // Overhead: ~2-5ns per call (indirect branch + icache miss)
    // =========================================================================
    class IStrategy {
    public:
        virtual ~IStrategy() = default;

        // Called on every book update — MUST be fast (< 1us)
        [[nodiscard]] virtual Signal on_book_update(
            InstrumentId id, const BookSignal& signal) noexcept = 0;

        // Called on order fill/cancel acknowledgment
        virtual void on_execution_report(const ExecutionReport& report) noexcept = 0;

        // Called once at startup — can be slow
        virtual void initialize() = 0;

        // Strategy metadata
        [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    };

    // =========================================================================
    // StrategyBase — CRTP static dispatch (zero-overhead, compiler can inline)
    //
    // Use when: maximum performance, strategy type known at compile time.
    // The real cost of virtual dispatch isn't the 2-5ns indirect call — it's
    // that the compiler CANNOT inline across a virtual call boundary.
    // =========================================================================
    template <typename Derived>
    class StrategyBase {
    public:
        [[gnu::hot, gnu::flatten]]
        Signal on_book_update(InstrumentId id, const BookSignal& signal) noexcept {
            return static_cast<Derived*>(this)->on_book_update_impl(id, signal);
        }

        void on_execution_report(const ExecutionReport& report) noexcept {
            static_cast<Derived*>(this)->on_execution_report_impl(report);
        }

        void initialize() {
            static_cast<Derived*>(this)->initialize_impl();
        }

        [[nodiscard]] std::string_view name() const noexcept {
            return static_cast<const Derived*>(this)->name_impl();
        }
    };

} // namespace hft::strategy
