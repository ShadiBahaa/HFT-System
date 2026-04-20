cmake_minimum_required(VERSION 3.20)

add_executable(hft_benchmarks
    bench_hot_path.cpp
)

target_link_libraries(hft_benchmarks PRIVATE core_lib)
