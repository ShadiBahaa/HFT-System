#include <gtest/gtest.h>
#include "feed/order_book.h"

using namespace hft::core;
using namespace hft::feed;

// ---- PriceLevel ----

TEST(PriceLevelTest, SizeAndAlignment) {
    EXPECT_EQ(sizeof(PriceLevel), 32);
    EXPECT_EQ(alignof(PriceLevel), 32);
}

// ---- OrderPool ----

TEST(OrderPoolTest, InsertAndFind) {
    OrderPool<10> pool;  // 1024 slots

    EXPECT_TRUE(pool.insert(1001, 15000, 100, 1));
    EXPECT_TRUE(pool.insert(1002, 15010, 200, 2));

    auto* s1 = pool.find(1001);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1->price, 15000);
    EXPECT_EQ(s1->quantity, 100);
    EXPECT_EQ(s1->side, 1);

    auto* s2 = pool.find(1002);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->price, 15010);

    // Not found
    EXPECT_EQ(pool.find(9999), nullptr);
}

TEST(OrderPoolTest, Remove) {
    OrderPool<10> pool;

    pool.insert(500, 10000, 50, 1);
    ASSERT_NE(pool.find(500), nullptr);

    EXPECT_TRUE(pool.remove(500));
    EXPECT_EQ(pool.find(500), nullptr);

    // Remove non-existent
    EXPECT_FALSE(pool.remove(999));
}

TEST(OrderPoolTest, Clear) {
    OrderPool<10> pool;

    pool.insert(1, 100, 10, 1);
    pool.insert(2, 200, 20, 2);
    pool.clear();

    EXPECT_EQ(pool.find(1), nullptr);
    EXPECT_EQ(pool.find(2), nullptr);
}

// ---- OrderBook ----

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook<10> book;
};

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_EQ(book.best_ask(), 0);
    EXPECT_EQ(book.bid_depth, 0);
    EXPECT_EQ(book.ask_depth, 0);
}

TEST_F(OrderBookTest, AddBidOrders) {
    book.add_order(1, Side::BUY, 10000, 100);
    book.add_order(2, Side::BUY, 10010, 200);
    book.add_order(3, Side::BUY, 9990, 50);

    EXPECT_EQ(book.bid_depth, 3);
    EXPECT_EQ(book.best_bid(), 10010);  // Highest bid first
    EXPECT_EQ(book.bids[0].quantity, 200);
    EXPECT_EQ(book.bids[1].price, 10000);
    EXPECT_EQ(book.bids[2].price, 9990);
}

TEST_F(OrderBookTest, AddAskOrders) {
    book.add_order(1, Side::SELL, 10020, 100);
    book.add_order(2, Side::SELL, 10010, 200);
    book.add_order(3, Side::SELL, 10030, 50);

    EXPECT_EQ(book.ask_depth, 3);
    EXPECT_EQ(book.best_ask(), 10010);  // Lowest ask first
    EXPECT_EQ(book.asks[0].quantity, 200);
    EXPECT_EQ(book.asks[1].price, 10020);
    EXPECT_EQ(book.asks[2].price, 10030);
}

TEST_F(OrderBookTest, SpreadAndMid) {
    book.add_order(1, Side::BUY, 10000, 100);
    book.add_order(2, Side::SELL, 10020, 100);

    EXPECT_EQ(book.spread(), 20);
    EXPECT_EQ(book.mid_price(), 10010);
}

TEST_F(OrderBookTest, CancelOrder) {
    book.add_order(1, Side::BUY, 10000, 100);
    book.add_order(2, Side::BUY, 10010, 200);
    EXPECT_EQ(book.bid_depth, 2);

    book.cancel_order(2);
    EXPECT_EQ(book.bid_depth, 1);
    EXPECT_EQ(book.best_bid(), 10000);
}

TEST_F(OrderBookTest, ExecuteOrder) {
    book.add_order(1, Side::BUY, 10000, 100);
    book.add_order(2, Side::SELL, 10020, 200);

    // Partial execution
    book.execute_order(2, 50);
    EXPECT_EQ(book.asks[0].quantity, 150);

    // Full execution
    book.execute_order(2, 150);
    EXPECT_EQ(book.ask_depth, 0);
}

TEST_F(OrderBookTest, ReplaceOrder) {
    book.add_order(1, Side::BUY, 10000, 100);
    EXPECT_EQ(book.best_bid(), 10000);

    book.replace_order(1, 10050, 200);
    EXPECT_EQ(book.best_bid(), 10050);
    EXPECT_EQ(book.bids[0].quantity, 200);
}

TEST_F(OrderBookTest, AggregateAtSamePrice) {
    book.add_order(1, Side::BUY, 10000, 100);
    book.add_order(2, Side::BUY, 10000, 200);

    EXPECT_EQ(book.bid_depth, 1);            // Same price = same level
    EXPECT_EQ(book.bids[0].quantity, 300);    // Aggregated
    EXPECT_EQ(book.bids[0].order_count, 2);
}

TEST_F(OrderBookTest, ApplyMarketUpdate) {
    MarketUpdate update{};
    update.type = UpdateType::ADD;
    update.order_ref = 1;
    update.side = Side::BUY;
    update.price = 10000;
    update.quantity = 100;

    book.apply(update);
    EXPECT_EQ(book.best_bid(), 10000);

    update.type = UpdateType::DELETE;
    book.apply(update);
    EXPECT_EQ(book.bid_depth, 0);
}

TEST_F(OrderBookTest, ToSignal) {
    book.add_order(1, Side::BUY, 10000, 100);
    book.add_order(2, Side::SELL, 10020, 200);

    auto sig = book.to_signal(42, 1234567890);
    EXPECT_EQ(sig.instrument_id, 42);
    EXPECT_EQ(sig.best_bid, 10000);
    EXPECT_EQ(sig.best_ask, 10020);
    EXPECT_EQ(sig.bid_size, 100);
    EXPECT_EQ(sig.ask_size, 200);
    EXPECT_EQ(sig.spread, 20);
    EXPECT_EQ(sig.timestamp, 1234567890U);
}
