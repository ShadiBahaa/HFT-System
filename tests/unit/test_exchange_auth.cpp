#include <gtest/gtest.h>
#include <string_view>
#include "security/exchange_auth.h"

using namespace hft::security;

TEST(ExchangeAuthTest, SetIdentity) {
    ExchangeAuth auth;
    auth.set_identity("CLIENT1", "EXCH");
    EXPECT_EQ(auth.sender_comp_id(), "CLIENT1");
    EXPECT_EQ(auth.target_comp_id(), "EXCH");
}

TEST(ExchangeAuthTest, BuildLogonHasRequiredTags) {
    ExchangeAuth auth;
    auth.set_identity("CLIENT", "EXCH");
    auth.set_credentials("user1", "pass1");

    char buf[1024];
    size_t n = auth.build_logon_message(buf, sizeof(buf), 1, 1000);
    ASSERT_GT(n, 0U);

    std::string_view msg(buf, n);
    EXPECT_NE(msg.find("8=FIX.4.4"), std::string_view::npos);
    EXPECT_NE(msg.find("35=A"), std::string_view::npos);
    EXPECT_NE(msg.find("49=CLIENT"), std::string_view::npos);
    EXPECT_NE(msg.find("56=EXCH"), std::string_view::npos);
    EXPECT_NE(msg.find("553=user1"), std::string_view::npos);
    EXPECT_NE(msg.find("554=pass1"), std::string_view::npos);
    EXPECT_NE(msg.find("10="), std::string_view::npos);
}

TEST(ExchangeAuthTest, BufferTooSmall) {
    ExchangeAuth auth;
    auth.set_identity("A", "B");
    auth.set_credentials("u", "p");

    char buf[16];
    size_t n = auth.build_logon_message(buf, sizeof(buf), 1, 1);
    EXPECT_EQ(n, 0U);
}

TEST(ExchangeAuthTest, TokenLifecycle) {
    ExchangeAuth auth;
    auth.set_token_ttl_ns(1000);

    EXPECT_FALSE(auth.token_valid(100));

    auth.issue_token("TOK123", 100);
    EXPECT_TRUE(auth.token_valid(500));
    EXPECT_EQ(auth.token(), "TOK123");

    // Expire
    EXPECT_FALSE(auth.token_valid(2000));

    auth.clear_token();
    EXPECT_FALSE(auth.token_valid(100));
}
