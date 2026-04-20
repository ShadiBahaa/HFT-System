cmake_minimum_required(VERSION 3.20)

# -----------------------------------------------------------------------------
# trading_engine — production binary that orchestrates the full hot path.
# Depends only on the header-only core_lib (no gtest).
# -----------------------------------------------------------------------------
add_executable(trading_engine
    main.cpp
)

target_link_libraries(trading_engine
    PRIVATE
    core_lib
)

# Threads — std::thread needs pthread on Linux, WinAPI on Windows.
find_package(Threads REQUIRED)
target_link_libraries(trading_engine PRIVATE Threads::Threads)

# Statically link libstdc++/libgcc/winpthread so the produced .exe does not
# depend on MSYS2's mingw64 runtime DLLs being on PATH at runtime. This makes
# the binary portable enough for a Docker image or a bare Windows host.
if(MINGW)
    target_link_options(trading_engine PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
        # Reserve a generous thread-0 stack. The default MinGW PE reserve is
        # 2 MiB, which is not enough for our stack-constructed large objects
        # (OrderBook with a 1 M-slot OrderPool is ~24 MiB on its own). The
        # hot-path code heap-allocates these, but making the reserved stack
        # comfortably larger avoids surprises in helper threads too.
        "LINKER:--stack,67108864"   # 64 MiB reserved
    )
endif()
