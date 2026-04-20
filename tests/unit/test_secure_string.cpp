#include <gtest/gtest.h>
#include <type_traits>
#include "security/secure_string.h"

using namespace hft::security;

using SS = SecureString<64>;

TEST(SecureStringTest, NotCopyableNotMovable) {
    static_assert(!std::is_copy_constructible_v<SS>);
    static_assert(!std::is_move_constructible_v<SS>);
    static_assert(!std::is_copy_assignable_v<SS>);
    static_assert(!std::is_move_assignable_v<SS>);
}

TEST(SecureStringTest, DefaultIsEmpty) {
    SS s;
    EXPECT_EQ(s.size(), 0U);
    EXPECT_TRUE(s.empty());
}

TEST(SecureStringTest, SetAndView) {
    SS s;
    EXPECT_TRUE(s.set("hunter2", 7));
    EXPECT_EQ(s.size(), 7U);
    EXPECT_EQ(s.view(), "hunter2");
}

TEST(SecureStringTest, SetStringView) {
    SS s;
    EXPECT_TRUE(s.set(std::string_view{"secret"}));
    EXPECT_EQ(s.view(), "secret");
}

TEST(SecureStringTest, SetTooLongFails) {
    SS s;
    std::string big(100, 'x');
    EXPECT_FALSE(s.set(big));
    EXPECT_TRUE(s.empty());
}

TEST(SecureStringTest, ClearWipes) {
    SS s;
    s.set("abc");
    s.clear();
    EXPECT_EQ(s.size(), 0U);
    EXPECT_TRUE(s.view().empty());
}

TEST(SecureStringTest, SetReplaces) {
    SS s;
    s.set("first");
    s.set("second");
    EXPECT_EQ(s.view(), "second");
}

TEST(SecureStringTest, LockUnlock) {
    SS s;
    s.set("locked");
    // Locking may fail without privileges — just ensure no crash
    s.lock_memory();
    s.unlock_memory();
    EXPECT_FALSE(s.is_locked());
}
