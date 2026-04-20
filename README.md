# HFT System — C++ Low-Latency Trading Platform

A header-only, production-style reference implementation of a high-frequency
trading system. The code traces the design in
[`hft_system_design.md`](hft_system_design.md) end-to-end: market data ingest,
order book, strategies, pre/post-trade risk, OMS, FIX/OUCH/BOE/iLink3 gateways,
WAL persistence, replay, failover, telemetry, and operational tooling.

Targeted at Windows (MSYS2/MinGW-w64) and Linux. Compiles with GCC 13+ or
MSVC 19.40+ at `-std=c++20`, `-Werror`, `-O3 -march=native`.

## Status

- **257** unit tests across **56** suites
- **3** integration tests (`tick → book → strategy → risk → OMS → FIX`)
- **5** chaos tests (WAL crash recovery, failover state transitions,
  rogue-strategy watchdog, feed-gap detection)
- **2** simulation tests (bit-exact market-replay divergence, strategy
  determinism across runs)
- **1** microbenchmark (`OrderBook::apply`, `MarketMaker::on_book_update`,
  `PreTradeRisk::check` — all **p999 ≤ 25 ns** on a modern desktop)
- **1** runnable orchestrator binary — `src/main.cpp` → `trading_engine.exe`,
  wires feed → book → strategy → risk → OMS → gateway → WAL → metrics
- Zero warnings at `-Wall -Wextra -Werror -Wpedantic`
- Portable across MinGW-w64 and glibc — platform-specific code is feature-gated
- CI on GitHub Actions: Linux GCC 13/14 × Debug/Release, Windows MinGW,
  ASan+UBSan, daily latency regression gate

## Directory layout

| Path | Contents |
|---|---|
| `core/` | Clock, TSC calibration, SPSC ring, backpressure, platform (CPU pinning, NUMA), market data types |
| `feed/` | Order book, ITCH/PITCH/MDP3 decoders, BBO snapshot (SoA), network I/O abstractions |
| `strategy/` | `IStrategy`, CRTP `StrategyBase`, market maker, stat-arb, index arb, ML signal bridge, dynamic loader |
| `risk/` | Pre-trade gate, post-trade aggregation, position manager, kill switch |
| `oms/` | Order manager, state machine, FIFO queue |
| `gateway/` | FIX encoder/decoder, connection state machine, NASDAQ OUCH / CBOE BOE / CME iLink3 adapters |
| `persistence/` | WAL writer, replay, market-data recorder |
| `telemetry/` | Latency profiler (HdrHistogram), health monitor, Prometheus metrics, trade logger |
| `resilience/` | Failover manager (5-state reconciliation, fencing tokens) |
| `security/` | `SecureString`, exchange auth (HMAC-SHA256 logon) |
| `fpga/` | Register-map abstraction, MMIO backend, pipeline configuration |
| `src/` | `main.cpp` — top-level `trading_engine` binary wiring all components |
| `tests/unit/` | GoogleTest unit coverage for every module |
| `tests/integration/` | End-to-end wire test |
| `tests/chaos/` | Fault-injection (crash, gap, heartbeat timeout, runaway strategy) |
| `tests/simulation/` | Market-replay bit-exactness & strategy determinism |
| `tests/benchmarks/` | Hot-path microbenchmarks (p50/p99/p999/max) |
| `scripts/` | System tuning, regression comparison, deployment, rollback, P&L report |
| `monitoring/` | Prometheus alert rules and Grafana dashboards (latency + PnL) |
| `docs/` | Runbook, incident response, regulatory compliance, metrics catalog |
| `.github/workflows/` | CI pipelines (`ci.yml`, `latency.yml`) |
| `Dockerfile` | Hermetic Ubuntu 24.04 + GCC 13.3 build, reproducible via `SOURCE_DATE_EPOCH` |

## Build

```bash
# Windows (MSYS2 / MinGW-w64)
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -B build -G Ninja
cmake --build build

# Linux
cmake -B build -G Ninja
cmake --build build
```

### Build options

| Option | Default | Effect |
|---|---|---|
| `HFT_BUILD_PROD` | `OFF` | Applies §12.2 hot-path flags: `-flto=thin -fno-exceptions -fno-rtti -falign-functions=64 -funroll-loops`, `NDEBUG` |
| `HFT_ENABLE_PGO` | *empty* | `generate` or `use` — two-phase profile-guided optimization |

```bash
# Production build
cmake -B build-prod -DCMAKE_BUILD_TYPE=Release -DHFT_BUILD_PROD=ON
cmake --build build-prod

# Hermetic Docker build (reproducible)
docker build -t hft-engine .
```

## Run

```bash
# Unit tests (257 tests, 56 suites)
./build/tests/unit/hft_tests

# Integration test (end-to-end tick → FIX wire)
./build/tests/integration/hft_integration_tests

# Chaos tests (fault injection)
./build/tests/chaos/hft_chaos_tests

# Simulation / replay determinism
./build/tests/simulation/hft_simulation_tests

# Microbenchmark (not wired into ctest — run manually)
./build/tests/benchmarks/hft_benchmarks

# Trading engine — synthetic demo (no market-data file required)
./build/src/trading_engine --demo --duration 5 --symbol AAPL

# Trading engine — replay a recorded .mdr tape
./build/src/trading_engine --ticks run.mdr --symbol AAPL --wal /tmp/audit.wal
```

The `trading_engine` binary spawns four threads — market-data producer,
book builder, strategy engine, and gateway — communicating via SPSC rings,
then prints a summary of orders sent, throttled, and WAL bytes written
when shutdown is signalled (SIGINT/SIGTERM) or the tape ends.

## Design highlights

- **Zero-allocation hot path.** Order book, risk check, and OMS `create_order`
  use fixed arrays and intrusive free lists. The only dynamic allocation is
  at initialization.
- **Template dispatch over virtuals on the hot path.** Strategies use CRTP
  (`StrategyBase<Derived>`) so `on_book_update` inlines. Virtual `IStrategy`
  remains for dynamic loading only.
- **Cache-line aligned data.** Risk state, positions, and book levels are
  `alignas(64)` with padding to prevent false sharing.
- **Seqlock / CAS for cross-thread reads.** `PositionManager`,
  `MLSignalReader`, and `FencingToken` use lock-free patterns so the hot
  thread never waits.
- **TSC-based clocking with drift correction.** `WallClock` reads `rdtsc` and
  converts to nanoseconds via a calibration factor refreshed every 60 s by
  a telemetry thread.
- **Kernel-bypass-ready feed.** `feed/network_io.h` abstracts receivers so
  `ef_vi` or DPDK backends drop in without touching decode/book code.
- **Full audit & compliance surface.** `TradeLogger` writes 128-byte binary
  records; `scripts/pnl_report.py` parses them into daily P&L; `docs/`
  covers SEC 15c3-5 and MiFID II RTS 6/25.

## Further reading

- [`hft_system_design.md`](hft_system_design.md) — full architecture spec
- [`docs/runbook.md`](docs/runbook.md) — daily operations
- [`docs/incident_response.md`](docs/incident_response.md) — SEV1–3 playbooks
- [`docs/regulatory_compliance.md`](docs/regulatory_compliance.md) — control matrix
- [`docs/metrics_catalog.md`](docs/metrics_catalog.md) — canonical `hft_*` metric names
  referenced by Prometheus alerts and Grafana dashboards
- [`monitoring/prometheus_alerts.yml`](monitoring/prometheus_alerts.yml) —
  14 alert rules across critical / warning / info tiers
- [`monitoring/grafana_dashboards/`](monitoring/grafana_dashboards/) —
  latency (tick→trade percentiles) and PnL (realized / per-strategy / positions) dashboards

## License

Educational / reference code. Not for deployment against a production trading
venue without the additional certification, capacity, and operational controls
described in the design document and runbook.
