#include <gtest/gtest.h>
#include "gateway/fix_encoder.h"
#include "gateway/sbe_encoder.h"
#include <string>
#include <cstring>

using namespace hft::core;
using namespace hft::gateway;

// ---- itoa_fast Tests ----

TEST(ItoaFastTest, Zero) {
    char buf[20];
    int len = itoa_fast(0, buf);
    EXPECT_EQ(len, 1);
    EXPECT_EQ(buf[0], '0');
}

TEST(ItoaFastTest, PositiveNumber) {
    char buf[20];
    int len = itoa_fast(12345, buf);
    EXPECT_EQ(std::string(buf, len), "12345");
}

TEST(ItoaFastTest, NegativeNumber) {
    char buf[20];
    int len = itoa_fast(-42, buf);
    EXPECT_EQ(std::string(buf, len), "-42");
}

TEST(ItoaFastTest, SingleDigit) {
    char buf[20];
    int len = itoa_fast(7, buf);
    EXPECT_EQ(len, 1);
    EXPECT_EQ(buf[0], '7');
}

TEST(ItoaFastTest, LargeNumber) {
    char buf[20];
    int len = itoa_fast(1000000000LL, buf);
    EXPECT_EQ(std::string(buf, len), "1000000000");
}

// ---- FixEncoder Tests ----

class FixEncoderTest : public ::testing::Test {
protected:
    FixEncoder encoder;

    void SetUp() override {
        encoder.set_sender("TRADER1");
        encoder.set_target("EXCHANGE");
    }

    // Helper: check that a FIX message contains a tag=value pair
    static bool has_tag_value(std::span<const char> msg, const std::string& tag_val) {
        std::string s(msg.data(), msg.size());
        // FIX uses \x01 as delimiter
        return s.find(tag_val) != std::string::npos;
    }

    // Helper: extract value for a FIX tag
    static std::string get_tag_value(std::span<const char> msg, int tag) {
        std::string s(msg.data(), msg.size());
        std::string prefix = std::to_string(tag) + "=";
        auto pos = s.find(prefix);
        if (pos == std::string::npos) return "";
        pos += prefix.size();
        auto end = s.find('\x01', pos);
        if (end == std::string::npos) return "";
        return s.substr(pos, end - pos);
    }
};

TEST_F(FixEncoderTest, EncodeNewOrder) {
    NewOrderSingle order{};
    std::strncpy(order.cl_ord_id, "ORD001", sizeof(order.cl_ord_id) - 1);
    std::strncpy(order.symbol, "AAPL", sizeof(order.symbol) - 1);
    order.side = Side::BUY;
    order.ord_type = OrderType::LIMIT;
    order.tif = TimeInForce::DAY;
    order.price = 15000;
    order.qty = 100;

    auto msg = encoder.encode_new_order(order);

    EXPECT_GT(msg.size(), 0U);

    // Verify BeginString
    EXPECT_TRUE(has_tag_value(msg, "8=FIX.4.2"));

    // Verify MsgType = D (NewOrderSingle)
    EXPECT_TRUE(has_tag_value(msg, "35=D"));

    // Verify SenderCompID
    EXPECT_TRUE(has_tag_value(msg, "49=TRADER1"));

    // Verify TargetCompID
    EXPECT_TRUE(has_tag_value(msg, "56=EXCHANGE"));

    // Verify ClOrdID
    EXPECT_TRUE(has_tag_value(msg, "11=ORD001"));

    // Verify Symbol
    EXPECT_TRUE(has_tag_value(msg, "55=AAPL"));

    // Verify Side = 1 (Buy)
    EXPECT_TRUE(has_tag_value(msg, "54=1"));

    // Verify OrderQty
    EXPECT_TRUE(has_tag_value(msg, "38=100"));

    // Verify Price
    EXPECT_TRUE(has_tag_value(msg, "44=15000"));

    // Verify OrdType = 2 (Limit)
    EXPECT_TRUE(has_tag_value(msg, "40=2"));

    // Verify TimeInForce = 0 (Day)
    EXPECT_TRUE(has_tag_value(msg, "59=0"));
}

TEST_F(FixEncoderTest, EncodeSellOrder) {
    NewOrderSingle order{};
    std::strncpy(order.cl_ord_id, "ORD002", sizeof(order.cl_ord_id) - 1);
    std::strncpy(order.symbol, "MSFT", sizeof(order.symbol) - 1);
    order.side = Side::SELL;
    order.ord_type = OrderType::MARKET;
    order.tif = TimeInForce::IOC;
    order.price = 0;
    order.qty = 200;

    auto msg = encoder.encode_new_order(order);

    EXPECT_TRUE(has_tag_value(msg, "54=2"));   // Side = Sell
    EXPECT_TRUE(has_tag_value(msg, "40=1"));   // OrdType = Market
    EXPECT_TRUE(has_tag_value(msg, "59=3"));   // TIF = IOC
    EXPECT_TRUE(has_tag_value(msg, "55=MSFT"));
}

TEST_F(FixEncoderTest, SequenceNumberIncrements) {
    NewOrderSingle order{};
    std::strncpy(order.cl_ord_id, "A", sizeof(order.cl_ord_id) - 1);
    std::strncpy(order.symbol, "X", sizeof(order.symbol) - 1);
    order.side = Side::BUY;
    order.qty = 1;

    encoder.encode_new_order(order);
    EXPECT_EQ(encoder.seq_num(), 1U);

    encoder.encode_new_order(order);
    EXPECT_EQ(encoder.seq_num(), 2U);

    encoder.encode_new_order(order);
    EXPECT_EQ(encoder.seq_num(), 3U);
}

TEST_F(FixEncoderTest, HasChecksum) {
    NewOrderSingle order{};
    std::strncpy(order.cl_ord_id, "T1", sizeof(order.cl_ord_id) - 1);
    std::strncpy(order.symbol, "IBM", sizeof(order.symbol) - 1);
    order.side = Side::BUY;
    order.qty = 50;
    order.price = 14000;

    auto msg = encoder.encode_new_order(order);

    // Last field should be checksum: 10=NNN\x01
    std::string s(msg.data(), msg.size());
    auto pos = s.rfind("10=");
    EXPECT_NE(pos, std::string::npos);

    // Verify checksum is correct
    int expected_sum = 0;
    for (size_t i = 0; i < pos; ++i)
        expected_sum += static_cast<unsigned char>(s[i]);
    int expected_checksum = expected_sum % 256;

    std::string cs_str = get_tag_value(msg, 10);
    EXPECT_EQ(cs_str.size(), 3U);  // Always 3 digits
    int actual_checksum = std::stoi(cs_str);
    EXPECT_EQ(actual_checksum, expected_checksum);
}

TEST_F(FixEncoderTest, EncodeCancelRequest) {
    auto msg = encoder.encode_cancel(100, 99, "GOOG", Side::SELL);

    EXPECT_GT(msg.size(), 0U);
    EXPECT_TRUE(has_tag_value(msg, "35=F"));        // Cancel request
    EXPECT_TRUE(has_tag_value(msg, "11=100"));       // ClOrdID
    EXPECT_TRUE(has_tag_value(msg, "41=99"));        // OrigClOrdID
    EXPECT_TRUE(has_tag_value(msg, "55=GOOG"));      // Symbol
    EXPECT_TRUE(has_tag_value(msg, "54=2"));         // Sell
}

// ---- SBEEncoder Tests ----

TEST(SBEEncoderTest, EncodeNewOrder) {
    SBEEncoder encoder;

    NewOrderSingle order{};
    std::strncpy(order.cl_ord_id, "SBE001", sizeof(order.cl_ord_id) - 1);
    order.side = Side::BUY;
    order.price = 15000;
    order.qty = 100;
    order.timestamp = 1234567890;

    auto msg = encoder.encode_new_order(order, 42, 1);

    EXPECT_EQ(msg.size(), sizeof(SBENewOrderSingle));

    // Decode and verify
    const auto* decoded = reinterpret_cast<const SBENewOrderSingle*>(msg.data());
    EXPECT_EQ(decoded->header.template_id, 514);
    EXPECT_EQ(decoded->price, 15000);
    EXPECT_EQ(decoded->order_qty, 100);
    EXPECT_EQ(decoded->security_id, 42);
    EXPECT_EQ(decoded->side, 1);  // Buy
    EXPECT_EQ(decoded->order_request_id, 1U);
    EXPECT_EQ(decoded->sending_time_epoch, 1234567890U);
    EXPECT_EQ(std::string(decoded->cl_ord_id, 6), "SBE001");
}

TEST(SBEEncoderTest, EncodeSellOrder) {
    SBEEncoder encoder;

    NewOrderSingle order{};
    order.side = Side::SELL;
    order.price = 20000;
    order.qty = 500;

    auto msg = encoder.encode_new_order(order, 99, 7);

    const auto* decoded = reinterpret_cast<const SBENewOrderSingle*>(msg.data());
    EXPECT_EQ(decoded->side, 2);  // Sell
    EXPECT_EQ(decoded->price, 20000);
    EXPECT_EQ(decoded->order_qty, 500);
}
