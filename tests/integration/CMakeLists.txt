cmake_minimum_required(VERSION 3.20)

add_executable(hft_integration_tests
    test_end_to_end.cpp
)

target_link_libraries(hft_integration_tests
    PRIVATE
    core_lib
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(hft_integration_tests)
