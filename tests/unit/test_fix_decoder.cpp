#include <gtest/gtest.h>
#include <string>
#include "gateway/fix_decoder.h"

using namespace hft::core;
using namespace hft::gateway;

namespace {
    // Build a FIX-style buffer with correct checksum
    std::string build_fix(const std::string& body_no_cs) {
        // body_no_cs is already a sequence of tag=value<SOH> fields excluding tag 10
        // Checksum over everything prior to "10=..."
        uint32_t sum = 0;
        for (char c : body_no_cs) sum += static_cast<uint8_t>(c);
        char tail[16];
        int n = std::snprintf(tail, sizeof(tail), "10=%03u\x01", sum % 256);
        return body_no_cs + std::string(tail, static_cast<size_t>(n));
    }
}

TEST(FixDecoderTest, ParseSimpleMessage) {
    std::string msg = build_fix("8=FIX.4.4\x01" "9=12\x01" "35=D\x01" "49=ME\x01");

    FixDecoder::Fields f;
    ASSERT_TRUE(FixDecoder::parse(msg.data(), msg.size(), f));
    EXPECT_EQ(f.get(8),  "FIX.4.4");
    EXPECT_EQ(f.get(35), "D");
    EXPECT_EQ(f.get(49), "ME");
    EXPECT_TRUE(f.has(10));
}

TEST(FixDecoderTest, UnknownTagReturnsEmpty) {
    std::string msg = build_fix("35=D\x01" "49=ME\x01");
    FixDecoder::Fields f;
    ASSERT_TRUE(FixDecoder::parse(msg.data(), msg.size(), f));
    EXPECT_TRUE(f.get(9999).empty());
    EXPECT_FALSE(f.has(9999));
}

TEST(FixDecoderTest, BadChecksumFails) {
    std::string msg = "35=D\x01" "49=ME\x01" "10=000\x01";  // bogus cs
    FixDecoder::Fields f;
    EXPECT_FALSE(FixDecoder::parse(msg.data(), msg.size(), f));
}

TEST(FixDecoderTest, MalformedRejected) {
    // Missing '=' between tag and value
    std::string msg = "35D\x01";
    FixDecoder::Fields f;
    EXPECT_FALSE(FixDecoder::parse(msg.data(), msg.size(), f));
}

TEST(FixDecoderTest, Checksum) {
    const char data[] = "abc";
    EXPECT_EQ(FixDecoder::checksum(data, 3), ('a' + 'b' + 'c') % 256);
}

TEST(FixDecoderTest, ExecutionReportExtraction) {
    std::string msg = build_fix(
        "8=FIX.4.4\x01" "9=60\x01"
        "35=8\x01"
        "11=12345\x01"
        "37=99\x01"
        "54=1\x01"
        "31=150.25\x01"
        "32=100\x01"
        "151=0\x01"
        "150=F\x01"
    );
    FixDecoder::Fields f;
    ASSERT_TRUE(FixDecoder::parse(msg.data(), msg.size(), f));

    ExecutionReport er{};
    ASSERT_TRUE(FixDecoder::to_execution_report(f, er));
    EXPECT_EQ(er.cl_ord_id, 12345U);
    EXPECT_EQ(er.order_id,  99U);
    EXPECT_EQ(er.side, Side::BUY);
    EXPECT_EQ(er.price, 1502500);            // 150.25 * 10000
    EXPECT_EQ(er.filled_qty, 100);
    EXPECT_EQ(er.leaves_qty, 0);
    EXPECT_EQ(er.exec_type, ExecType::FILL);
}

TEST(FixDecoderTest, FindTagOffset) {
    std::string msg = "8=FIX.4.4\x01" "35=D\x01" "49=ME\x01";
    size_t off = FixDecoder::find_tag_offset(msg.data(), msg.size(), 35);
    ASSERT_NE(off, std::string_view::npos);
    EXPECT_EQ(msg.substr(off, 4), "35=D");
}
