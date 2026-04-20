#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

#include "core/market_data.h"
#include "core/spsc_ring.h"
#include "feed/order_book.h"
#include "strategy/strategies/market_maker.h"
#include "risk/kill_switch.h"
#include "risk/pre_trade_risk.h"
#include "oms/order_manager.h"
#include "gateway/fix_encoder.h"

// =============================================================================
// End-to-end integration test: tick → book → strategy → risk → OMS → FIX wire
//
// Exercises the full hot path without hardware or network. Catches wiring
// regressions when individual modules change their contract.
// =============================================================================

using namespace hft::core;
using namespace hft::feed;
using namespace hft::strategy;
using namespace hft::risk;
using namespace hft::oms;
using namespace hft::gateway;

namespace {

    MarketUpdate make_add(uint64_t seq, Side side, Price px, Quantity qty,
                          OrderId ref, InstrumentId instr = 1) {
        MarketUpdate u{};
        u.timestamp     = 1'000'000 + seq;
        u.instrument_id = instr;
        u.type          = UpdateType::ADD;
        u.side          = side;
        u.price         = px;
        u.quantity      = qty;
        u.order_ref     = ref;
        u.sequence      = seq;
        return u;
    }

} // namespace

TEST(EndToEndTest, TickToFixOrder) {
    // --- Build the book from two incoming ticks ---
    OrderBook<> book{};
    book.apply(make_add(1, Side::BUY,  9990, 300, 101));
    book.apply(make_add(2, Side::SELL, 10010, 300, 102));

    // --- Strategy consumes a BookSignal and produces a Signal ---
    MarketMaker::Config cfg{};
    cfg.instrument_id    = 1;
    cfg.base_qty         = 100;
    cfg.max_position     = 1000;
    cfg.min_spread       = 10;
    cfg.max_spread       = 100;
    cfg.skew_factor      = 0.5;
    cfg.volatility_scale = 1.0;
    MarketMaker mm(cfg);
    mm.initialize();

    BookSignal bs{};
    bs.instrument_id = 1;
    bs.best_bid  = book.best_bid();
    bs.best_ask  = book.best_ask();
    bs.mid_price = (bs.best_bid + bs.best_ask) / 2;
    bs.spread    = bs.best_ask - bs.best_bid;
    bs.timestamp = 2'000'000;

    auto sig = mm.on_book_update(1, bs);
    ASSERT_NE(sig.action, Action::NONE) << "strategy should emit a quote";
    ASSERT_GT(sig.quantity, 0);

    // --- Pre-trade risk gate ---
    // PreTradeRisk is constructed with external global position table and
    // kill switch pointers — we provide in-test stand-ins here.
    auto global_positions = std::make_unique<
        std::array<GlobalPosition, MAX_INSTRUMENTS>>();
    KillSwitch kill_switch;
    PreTradeRisk risk(*global_positions, kill_switch, /*strategy_id=*/0);

    InstrumentLimits lim{};
    lim.max_position        = 100'000;
    lim.max_order_size      = 10'000;
    lim.max_notional        = 1'000'000'000;
    lim.fat_finger_price    = bs.mid_price;  // 100% band — permissive
    lim.max_orders_per_sec  = 1'000;
    risk.set_limits(1, lim);

    auto rr = risk.check(/*id=*/1, sig.side, sig.quantity, sig.price, bs.mid_price);
    ASSERT_EQ(rr, RiskResult::PASS);

    // --- OMS creates an Order with assigned ClOrdID ---
    OrderRequest req{};
    req.action        = sig.action;
    req.side          = sig.side;
    req.order_type    = OrderType::LIMIT;
    req.tif           = TimeInForce::DAY;
    req.instrument_id = sig.instrument_id;
    req.price         = sig.price;
    req.quantity      = sig.quantity;
    req.timestamp     = 3'000'000;

    OrderManagementSystem<> oms;
    auto order = oms.create_order(req);
    EXPECT_GT(order.client_order_id, 0U);

    // --- Encode to FIX wire ---
    FixEncoder encoder;
    encoder.set_sender("TRADER1");
    encoder.set_target("EXCHANGE");

    NewOrderSingle nos{};
    std::snprintf(nos.cl_ord_id, sizeof(nos.cl_ord_id), "%llu",
                  static_cast<unsigned long long>(order.client_order_id));
    std::memcpy(nos.symbol, "TEST", 4);
    nos.side      = req.side;
    nos.ord_type  = req.order_type;
    nos.tif       = req.tif;
    nos.price     = req.price;
    nos.qty       = req.quantity;
    nos.timestamp = req.timestamp;

    auto wire = encoder.encode_new_order(nos);
    ASSERT_GT(wire.size(), 0U);

    std::string s(wire.data(), wire.size());
    EXPECT_NE(s.find("35=D"), std::string::npos);       // MsgType=NewOrderSingle
    EXPECT_NE(s.find("49=TRADER1"), std::string::npos); // SenderCompID
    EXPECT_NE(s.find("56=EXCHANGE"), std::string::npos);
    EXPECT_NE(s.find("10="), std::string::npos);        // Checksum
}

TEST(EndToEndTest, RiskBlocksOversizedOrder) {
    auto global_positions = std::make_unique<
        std::array<GlobalPosition, MAX_INSTRUMENTS>>();
    KillSwitch kill_switch;
    PreTradeRisk risk(*global_positions, kill_switch, /*strategy_id=*/0);

    InstrumentLimits lim{};
    lim.max_position        = 10'000;
    lim.max_order_size      = 100;   // Tight cap
    lim.max_notional        = 1'000'000'000;
    lim.fat_finger_price    = 10'000;
    lim.max_orders_per_sec  = 1'000;
    risk.set_limits(1, lim);

    auto rr = risk.check(/*id=*/1, Side::BUY, /*qty=*/500, /*price=*/10000,
                         /*mid=*/10000);
    EXPECT_EQ(rr, RiskResult::SIZE_BREACH);
}

TEST(EndToEndTest, SpscRingFeedToStrategy) {
    SPSCRingBuffer<MarketUpdate, 64> ring;
    for (uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring.try_push(make_add(i, Side::BUY, 100 + i, 10, 1000 + i)));
    }
    std::vector<MarketUpdate> received;
    MarketUpdate u;
    while (ring.try_pop(u)) received.push_back(u);
    ASSERT_EQ(received.size(), 5U);
    for (uint64_t i = 0; i < 5; ++i) EXPECT_EQ(received[i].sequence, i);
}
