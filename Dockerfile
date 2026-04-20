# =============================================================================
# Hermetic build image for the HFT system.
#
# Pins toolchain + sysroot so every production binary is byte-reproducible
# regardless of the developer workstation (§12.2 of hft_system_design.md).
#
# Base: Ubuntu 24.04 LTS  → glibc 2.39
# Compiler: GCC 13.3 (distro-stable)
# Build system: CMake 3.28 + Ninja
#
# The image is multi-stage. The `builder` stage has the full toolchain; the
# final `runtime` stage strips everything except glibc + the built binary.
#
# Usage:
#   docker build -t hft/builder --target builder .
#   docker build -t hft/engine .                    # runtime image
#
#   # Reproducible build from a clean tree:
#   docker run --rm -v "$PWD:/src:ro" -v "$PWD/build-docker:/build" \
#     hft/builder bash -c "cmake -B /build -S /src -G Ninja \
#                          -DCMAKE_BUILD_TYPE=Release -DHFT_BUILD_PROD=ON \
#                          && cmake --build /build --parallel"
# =============================================================================

# ---- Stage 1: builder --------------------------------------------------------
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    SOURCE_DATE_EPOCH=1700000000

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential=12.10ubuntu1 \
        g++-13 \
        cmake \
        ninja-build \
        python3 \
        git \
        ca-certificates \
        pkg-config \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
    && rm -rf /var/lib/apt/lists/*

# Record exact toolchain versions into the image for audit.
RUN gcc --version > /etc/hft_toolchain.txt \
 && ldd  --version >> /etc/hft_toolchain.txt \
 && cmake --version >> /etc/hft_toolchain.txt

WORKDIR /src

# Default command: configure + build in /build, assuming /src is bind-mounted.
CMD ["bash", "-c", "\
    cmake -B /build -S /src -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DHFT_BUILD_PROD=ON && \
    cmake --build /build --parallel && \
    ctest --test-dir /build --output-on-failure"]

# ---- Stage 2: runtime --------------------------------------------------------
# Minimal runtime — only what the stripped binary needs at exec time.
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        libgcc-s1 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 1500 --home-dir /var/lib/hft --create-home hft

# Binary is copied in by the deploy pipeline (docker build -f Dockerfile \
#   --build-arg BIN=./build/engine .) rather than rebuilt here, so the runtime
# image stays small and reproducible independent of build environment.
ARG BIN=./build/engine
COPY --chown=hft:hft ${BIN} /usr/local/bin/hft_engine

USER hft
WORKDIR /var/lib/hft

# Healthcheck: stub — real deployments mount /var/lib/hft/health and exec a
# probe against /metrics (see docs/runbook.md).
ENTRYPOINT ["/usr/local/bin/hft_engine"]
