cmake_minimum_required(VERSION 3.20)

add_executable(hft_simulation_tests
    test_market_replay_divergence.cpp
)

target_link_libraries(hft_simulation_tests
    PRIVATE
    core_lib
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(hft_simulation_tests)
