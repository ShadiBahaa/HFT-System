cmake_minimum_required(VERSION 3.20)
project(HFTSystem VERSION 0.1.0 LANGUAGES CXX)

# Ensure C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS False)

# -----------------------------------------------------------------------------
# Build mode options
#
#   HFT_BUILD_PROD       ON   apply §12.2 hot-path flags: LTO, -fno-exceptions,
#                              -fno-rtti, alignment, loop unrolling. Off by
#                              default so unit tests (which use gtest and
#                              therefore exceptions) keep compiling.
#   HFT_ENABLE_PGO       OFF  two-stage PGO: configure once with
#                              -DHFT_ENABLE_PGO=generate, run the benchmark to
#                              populate /tmp/hft_pgo, reconfigure with
#                              -DHFT_ENABLE_PGO=use for the final build.
# -----------------------------------------------------------------------------
option(HFT_BUILD_PROD  "Apply production hot-path compile flags (§12.2)" OFF)
option(HFT_BUILD_TESTS "Build unit/integration/chaos/simulation tests (requires googletest)" ON)
set(HFT_ENABLE_PGO "" CACHE STRING "PGO mode: empty | generate | use")

# Set strict warning levels depending on compiler
if(MSVC)
    add_compile_options(/W4 /WX /arch:AVX2 /fp:fast)
    # Exclude some noisy MSVC warnings if necessary
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror -Wpedantic -O3 -march=native -ffast-math)

    if(HFT_BUILD_PROD)
        message(STATUS "HFT_BUILD_PROD=ON: enabling hot-path flags")
        # Link-time optimization + alignment + loop unrolling.
        # Exceptions/RTTI disabled — gtest cannot build with these flags, so
        # production mode is for the engine binary only.
        add_compile_options(
            -flto=thin
            -fno-exceptions
            -fno-rtti
            -fomit-frame-pointer
            -falign-functions=64
            -falign-loops=64
            -funroll-loops
        )
        add_link_options(-flto=thin)
        add_compile_definitions(NDEBUG)
    endif()

    if(HFT_ENABLE_PGO STREQUAL "generate")
        message(STATUS "PGO: instrumented build (run benchmark to populate /tmp/hft_pgo)")
        add_compile_options(-fprofile-generate=/tmp/hft_pgo)
        add_link_options(-fprofile-generate=/tmp/hft_pgo)
    elseif(HFT_ENABLE_PGO STREQUAL "use")
        message(STATUS "PGO: optimized build using /tmp/hft_pgo")
        add_compile_options(-fprofile-use=/tmp/hft_pgo -fprofile-correction)
        add_link_options(-fprofile-use=/tmp/hft_pgo)
    endif()
endif()

# Core library — header-only for best inlining
add_library(core_lib INTERFACE)

# Target include directories
target_include_directories(core_lib INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Trading engine executable — wires the whole stack together for deployment.
# Always built; it only depends on core_lib (no gtest).
add_subdirectory(src)

# Microbenchmarks — also do not depend on gtest, so always available.
add_subdirectory(tests/benchmarks)

if(HFT_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    # Fetch googletest via shallow git clone rather than a URL zip. Zip downloads
    # go through codeload.github.com which periodically returns 404 / resets
    # connections during CI; `git clone --depth=1` hits github.com directly
    # and is retried by git itself.
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
      GIT_SHALLOW    TRUE
      GIT_PROGRESS   TRUE
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests/unit)
    add_subdirectory(tests/integration)
    add_subdirectory(tests/chaos)
    add_subdirectory(tests/simulation)
endif()
