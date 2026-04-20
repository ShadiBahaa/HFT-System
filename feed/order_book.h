#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "core/types.h"
#include "core/market_data.h"
#include "core/spsc_ring.h"

namespace hft::feed {

    using namespace hft::core;

    // =========================================================================
    // PriceLevel — padded to exactly 32 bytes (2 per cache line, zero waste)
    // =========================================================================
    struct alignas(32) PriceLevel {
        Price       price{0};           // Fixed-point: actual_price * 10,000
        int64_t     quantity{0};        // Total shares at this level
        uint32_t    order_count{0};     // Number of orders (useful for signal)
        uint32_t    flags{0};           // Implied, RFQ, etc.
        uint64_t    _padding{0};        // Explicit alignment — 2 levels per cache line
    };
    static_assert(sizeof(PriceLevel) == 32);

    // =========================================================================
    // OrderPool — Pre-allocated O(1) lookup via Fibonacci hashing
    //
    // Hash maps — even flat ones — have unpredictable latency spikes during
    // rehashing. This pool never rehashes because it's pre-sized.
    // =========================================================================
    template <size_t CapacityBits = 20>
    struct alignas(64) OrderPool {
        static constexpr size_t CAPACITY = size_t{1} << CapacityBits;  // ~1M orders
        static constexpr size_t MASK = CAPACITY - 1;
        static constexpr int    MAX_PROBES = 8;

        struct Slot {
            uint64_t order_ref{0};    // 0 = empty
            Price    price{0};
            Quantity quantity{0};
            int8_t   side{0};
            int8_t   active{0};       // 0 = deleted/empty, 1 = live
            int16_t  padding{0};
        };
        static_assert(sizeof(Slot) == 24);

        Slot slots_[CAPACITY]{};

        // Fibonacci hashing — much better distribution than modulo for sequential IDs
        [[gnu::hot]] size_t hash(uint64_t ref) const noexcept {
            return static_cast<size_t>(
                (ref * 11400714819323198485ULL) >> (64 - CapacityBits));
        }

        [[gnu::hot]] Slot* find(uint64_t ref) noexcept {
            size_t idx = hash(ref);
            for (int probe = 0; probe < MAX_PROBES; ++probe) {
                auto& s = slots_[(idx + probe) & MASK];
                if (s.order_ref == ref && s.active) return &s;
                if (s.order_ref == 0) return nullptr;   // empty = not found
            }
            return nullptr;
        }

        [[gnu::hot]] const Slot* find(uint64_t ref) const noexcept {
            size_t idx = hash(ref);
            for (int probe = 0; probe < MAX_PROBES; ++probe) {
                auto& s = slots_[(idx + probe) & MASK];
                if (s.order_ref == ref && s.active) return &s;
                if (s.order_ref == 0) return nullptr;
            }
            return nullptr;
        }

        [[gnu::hot]] bool insert(uint64_t ref, Price price, Quantity qty, int8_t side) noexcept {
            size_t idx = hash(ref);
            for (int probe = 0; probe < MAX_PROBES; ++probe) {
                auto& s = slots_[(idx + probe) & MASK];
                if (s.order_ref == 0 || !s.active) {
                    s.order_ref = ref;
                    s.price = price;
                    s.quantity = qty;
                    s.side = side;
                    s.active = 1;
                    s.padding = 0;
                    return true;
                }
            }
            return false;  // Pool full in this probe chain
        }

        [[gnu::hot]] bool remove(uint64_t ref) noexcept {
            if (auto* s = find(ref)) {
                s->active = 0;
                return true;
            }
            return false;
        }

        void clear() noexcept {
            for (size_t i = 0; i < CAPACITY; ++i)
                slots_[i] = Slot{};
        }
    };

    // =========================================================================
    // OrderBook — cache-optimized, flat arrays, no heap allocation on hot path
    //
    // Bids sorted descending (best bid at index 0)
    // Asks sorted ascending  (best ask at index 0)
    // =========================================================================
    static constexpr int MAX_LEVELS = 20;

    // Use a smaller default OrderPool for the book to keep it practical for
    // non-production use (tests, backtesting). Production can template with 20.
    template <size_t PoolBits = 16>
    struct alignas(64) OrderBook {
        std::array<PriceLevel, MAX_LEVELS> bids{};
        std::array<PriceLevel, MAX_LEVELS> asks{};
        int bid_depth{0};
        int ask_depth{0};

        OrderPool<PoolBits> orders{};

        // ----- Top-of-book accessors -----

        [[nodiscard]] Price best_bid() const noexcept {
            return bid_depth > 0 ? bids[0].price : 0;
        }
        [[nodiscard]] Price best_ask() const noexcept {
            return ask_depth > 0 ? asks[0].price : 0;
        }
        [[nodiscard]] Price spread() const noexcept {
            return best_ask() - best_bid();
        }
        [[nodiscard]] Price mid_price() const noexcept {
            return (best_bid() + best_ask()) / 2;
        }

        // ----- L3 order operations -----

        [[gnu::hot]] void add_order(uint64_t ref, Side side, Price price, Quantity qty) noexcept {
            int8_t s = static_cast<int8_t>(side);
            orders.insert(ref, price, qty, s);
            if (side == Side::BUY)  insert_bid(price, qty);
            else                    insert_ask(price, qty);
        }

        [[gnu::hot]] void cancel_order(uint64_t ref) noexcept {
            auto* slot = orders.find(ref);
            if (!slot) return;
            Side side = static_cast<Side>(slot->side);
            if (side == Side::BUY) remove_bid(slot->price, slot->quantity);
            else                   remove_ask(slot->price, slot->quantity);
            slot->active = 0;
        }

        [[gnu::hot]] void execute_order(uint64_t ref, Quantity exec_qty) noexcept {
            auto* slot = orders.find(ref);
            if (!slot) return;
            Side side = static_cast<Side>(slot->side);
            Quantity actual = std::min(exec_qty, slot->quantity);
            if (side == Side::BUY) remove_bid(slot->price, actual);
            else                   remove_ask(slot->price, actual);
            slot->quantity -= actual;
            if (slot->quantity <= 0) slot->active = 0;
        }

        [[gnu::hot]] void replace_order(uint64_t ref, Price new_price, Quantity new_qty) noexcept {
            auto* slot = orders.find(ref);
            if (!slot) return;
            Side side = static_cast<Side>(slot->side);
            // Remove old level contribution
            if (side == Side::BUY) remove_bid(slot->price, slot->quantity);
            else                   remove_ask(slot->price, slot->quantity);
            // Update slot
            slot->price = new_price;
            slot->quantity = new_qty;
            // Add new level contribution
            if (side == Side::BUY) insert_bid(new_price, new_qty);
            else                   insert_ask(new_price, new_qty);
        }

        // ----- Apply a normalized MarketUpdate -----

        [[gnu::hot]] void apply(const MarketUpdate& update) noexcept {
            switch (update.type) {
                case UpdateType::ADD:
                    add_order(update.order_ref, update.side, update.price, update.quantity);
                    break;
                case UpdateType::DELETE:
                    cancel_order(update.order_ref);
                    break;
                case UpdateType::EXECUTE:
                    execute_order(update.order_ref, update.quantity);
                    break;
                case UpdateType::MODIFY:
                    replace_order(update.order_ref, update.price, update.quantity);
                    break;
                default:
                    break;
            }
        }

        // ----- Generate a compact BookSignal for the strategy -----

        [[nodiscard]] BookSignal to_signal(InstrumentId id, TimestampNs ts) const noexcept {
            BookSignal sig{};
            sig.instrument_id = id;
            sig.timestamp = ts;
            if (bid_depth > 0) {
                sig.best_bid   = bids[0].price;
                sig.bid_size   = static_cast<Quantity>(bids[0].quantity);
                sig.bid_orders = bids[0].order_count;
            }
            if (ask_depth > 0) {
                sig.best_ask   = asks[0].price;
                sig.ask_size   = static_cast<Quantity>(asks[0].quantity);
                sig.ask_orders = asks[0].order_count;
            }
            sig.mid_price = mid_price();
            sig.spread = spread();
            return sig;
        }

        void clear() noexcept {
            bids = {};
            asks = {};
            bid_depth = 0;
            ask_depth = 0;
            orders.clear();
        }

    private:
        // ----- Bid side: sorted descending (highest price at index 0) -----

        void insert_bid(Price price, Quantity qty) noexcept {
            // Check if level already exists
            for (int i = 0; i < bid_depth; ++i) {
                if (bids[i].price == price) {
                    bids[i].quantity += qty;
                    ++bids[i].order_count;
                    return;
                }
            }
            // Insert new level in sorted position
            if (bid_depth >= MAX_LEVELS) {
                // If new price is worse than worst existing level, drop it
                if (price <= bids[MAX_LEVELS - 1].price) return;
                --bid_depth;  // Drop the worst level to make room
            }
            int pos = bid_depth;
            // Find insertion point (descending order)
            while (pos > 0 && bids[pos - 1].price < price) {
                bids[pos] = bids[pos - 1];
                --pos;
            }
            bids[pos] = PriceLevel{price, qty, 1, 0, 0};
            ++bid_depth;
        }

        void remove_bid(Price price, Quantity qty) noexcept {
            for (int i = 0; i < bid_depth; ++i) {
                if (bids[i].price == price) {
                    bids[i].quantity -= qty;
                    if (bids[i].order_count > 0) --bids[i].order_count;
                    if (bids[i].quantity <= 0) {
                        // Remove the level by shifting
                        for (int j = i; j < bid_depth - 1; ++j)
                            bids[j] = bids[j + 1];
                        bids[bid_depth - 1] = PriceLevel{};
                        --bid_depth;
                    }
                    return;
                }
            }
        }

        // ----- Ask side: sorted ascending (lowest price at index 0) -----

        void insert_ask(Price price, Quantity qty) noexcept {
            for (int i = 0; i < ask_depth; ++i) {
                if (asks[i].price == price) {
                    asks[i].quantity += qty;
                    ++asks[i].order_count;
                    return;
                }
            }
            if (ask_depth >= MAX_LEVELS) {
                if (price >= asks[MAX_LEVELS - 1].price) return;
                --ask_depth;
            }
            int pos = ask_depth;
            while (pos > 0 && asks[pos - 1].price > price) {
                asks[pos] = asks[pos - 1];
                --pos;
            }
            asks[pos] = PriceLevel{price, qty, 1, 0, 0};
            ++ask_depth;
        }

        void remove_ask(Price price, Quantity qty) noexcept {
            for (int i = 0; i < ask_depth; ++i) {
                if (asks[i].price == price) {
                    asks[i].quantity -= qty;
                    if (asks[i].order_count > 0) --asks[i].order_count;
                    if (asks[i].quantity <= 0) {
                        for (int j = i; j < ask_depth - 1; ++j)
                            asks[j] = asks[j + 1];
                        asks[ask_depth - 1] = PriceLevel{};
                        --ask_depth;
                    }
                    return;
                }
            }
        }
    };

} // namespace hft::feed
