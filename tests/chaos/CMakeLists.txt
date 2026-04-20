cmake_minimum_required(VERSION 3.20)

add_executable(hft_chaos_tests
    test_chaos.cpp
)

target_link_libraries(hft_chaos_tests
    PRIVATE
    core_lib
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(hft_chaos_tests)
