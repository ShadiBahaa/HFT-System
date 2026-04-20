# Production-Grade High-Frequency Trading System Architecture

> **Document Classification**: Technical Architecture Specification
> **Target Latency**: Sub-5μs tick-to-trade (glass-to-glass)
> **Language**: C++20/23 (hot path), Python (analytics/backtesting)
> **Last Updated**: April 2026

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [High-Level Architecture](#2-high-level-architecture)
3. [Low-Latency Design](#3-low-latency-design)
4. [Market Data Pipeline](#4-market-data-pipeline)
5. [Strategy Engine](#5-strategy-engine)
6. [Order Execution System](#6-order-execution-system)
7. [Risk Management](#7-risk-management)
8. [Data Storage & State](#8-data-storage--state)
9. [Fault Tolerance & Resilience](#9-fault-tolerance--resilience)
10. [Security](#10-security)
11. [Performance Engineering](#11-performance-engineering)
12. [CI/CD & Deployment](#12-cicd--deployment)
13. [Monitoring & Observability](#13-monitoring--observability)
14. [Testing Strategy](#14-testing-strategy)
15. [Advanced Topics](#15-advanced-topics)
16. [Design Review & Errata](#16-design-review--errata)

---

## 1. System Overview

### 1.1 System Goals

| Metric | Target | Measurement Point |
|---|---|---|
| **Tick-to-trade latency** | < 5μs p50, < 10μs p99, < 25μs p999 | NIC RX timestamp → NIC TX timestamp |
| **Order book update latency** | < 1μs | Feed handler output → book state update |
| **Throughput** | 10M+ messages/sec | Aggregate across all feed handlers |
| **Availability** | 99.999% (5.26 min/year downtime) | During exchange trading hours |
| **Jitter** | < 2μs standard deviation | Measured over 1-hour trading windows |
| **Recovery Time** | < 50ms | Failover to standby system |

### 1.2 Supported Trading Strategies

| Strategy | Latency Sensitivity | Description |
|---|---|---|
| **Market Making** | Critical (< 5μs) | Two-sided quoting with dynamic spread, inventory management, adverse selection avoidance |
| **Statistical Arbitrage** | High (< 50μs) | Cross-asset mean reversion, pairs trading, cointegrated baskets |
| **Index Arbitrage** | Critical (< 5μs) | ETF vs. constituent basket arbitrage, futures-spot basis trading |
| **Latency Arbitrage** | Extreme (< 2μs) | Cross-venue price discrepancy exploitation (typically FPGA-only) |
| **Momentum/Trend** | Moderate (< 500μs) | Short-term momentum signals, order flow imbalance |

### 1.3 System Constraints

**Hardware Constraints**:
- Co-located servers within exchange data centers (Equinix NY5/NY4, LD4, TY3)
- Dual-socket Intel Xeon Sapphire Rapids or AMD EPYC Genoa (≤ 32 cores, high clock)
- 512GB DDR5-4800 ECC RAM minimum; 1TB for full book depth
- Solarflare X2/XtremeScale NICs (kernel bypass via OpenOnload)
- PCIe Gen5 x16 for FPGA cards (Xilinx Alveo U55C or similar)
- NVMe SSDs for WAL (Intel Optane P5800X preferred for deterministic latency)

**Network Constraints**:
- Exchange multicast feeds: UDP, 10GbE/25GbE
- Exchange order entry: TCP, dedicated cross-connects
- Inter-datacenter: Microwave/millimeter-wave links (Chicago–NJ ~4.1ms one-way)
- Round-trip to exchange matching engine: ~1-5μs via cross-connect

**Exchange API Constraints**:
- FIX 4.2/4.4/5.0 (most equity exchanges)
- OUCH (NASDAQ), BOE (CBOE), PITCH (BATS/Cboe)
- CME iLink 3 (binary, SBE-encoded)
- Rate limits: 300-1000 orders/sec per port (exchange-dependent)

**Regulatory Constraints**:
- SEC Rule 15c3-5 (pre-trade risk checks, mandatory)
- MiFID II RTS 25 (clock synchronization ≤ 100μs, EU)
- Reg NMS (best execution obligation, US equities)
- Full audit trail with nanosecond timestamps
- Market Access Rule (broker-dealer sponsored access controls)

---

## 2. High-Level Architecture

### 2.1 Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        EXCHANGE DATA CENTER (Co-located)                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐     ┌──────────────────────────────────────────────────┐  │
│  │  Exchange     │     │              PRIMARY TRADING SERVER              │  │
│  │  Matching     │     │                                                  │  │
│  │  Engine       │     │  ┌──────────┐   ┌──────────┐   ┌────────────┐  │  │
│  │              ├─UDP─►│  │  Feed    │──►│ Order    │──►│ Strategy   │  │  │
│  │              │ mcast│  │  Handler │   │ Book     │   │ Engine     │  │  │
│  │              │      │  │  (DPDK)  │   │ Builder  │   │ (plugins)  │  │  │
│  │              │      │  └──────────┘   └──────────┘   └─────┬──────┘  │  │
│  │              │      │                                       │         │  │
│  │              │      │  ┌──────────┐   ┌──────────┐   ┌────▼──────┐  │  │
│  │              │◄─TCP─┤  │ Exchange │◄──│ OMS /    │◄──│ Pre-Trade │  │  │
│  │              │  FIX │  │ Gateway  │   │ Router   │   │ Risk      │  │  │
│  │              │      │  └──────────┘   └──────────┘   └───────────┘  │  │
│  └──────────────┘      │                                                  │  │
│                         │  ┌──────────────────────────────────────────┐   │  │
│                         │  │ Shared Memory Bus (lock-free SPSC rings) │   │  │
│                         │  └──────────────────────────────────────────┘   │  │
│                         │                                                  │  │
│                         │  ┌──────────┐   ┌──────────┐   ┌───────────┐  │  │
│                         │  │ WAL      │   │ Position │   │ Telemetry │  │  │
│                         │  │ Writer   │   │ Manager  │   │ (off-path)│  │  │
│                         │  └──────────┘   └──────────┘   └───────────┘  │  │
│                         └──────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │              STANDBY TRADING SERVER (hot standby)                     │   │
│  │  (Identical topology, receives same market data, shadow-trades)      │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  ┌──────────┐  ┌──────────────┐  ┌──────────────┐                          │
│  │ PTP/GPS  │  │ Management   │  │ FPGA Card    │                          │
│  │ Clock    │  │ Server       │  │ (optional)   │                          │
│  │ Source   │  │ (out-of-band)│  │              │                          │
│  └──────────┘  └──────────────┘  └──────────────┘                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                              │
                    Microwave / Fiber
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     REMOTE DATA CENTER (Analytics / DR)                     │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐  ┌───────────┐  ┌──────────┐  │
│  │ Market   │  │ Backtest │  │ Risk Mgmt │  │ Compliance│  │ Grafana  │  │
│  │ Data     │  │ Engine   │  │ Dashboard │  │ & Audit   │  │ & Alerts │  │
│  │ Store    │  │          │  │           │  │           │  │          │  │
│  └──────────┘  └──────────┘  └───────────┘  └───────────┘  └──────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Core Components

| Component | Responsibility | Latency Budget | Thread Model |
|---|---|---|---|
| **Feed Handler** | Receive, decode, normalize market data | < 500ns | Dedicated core, busy-poll |
| **Order Book Builder** | Maintain L3 book state per instrument | < 300ns | Same core as feed handler or adjacent |
| **Strategy Engine** | Signal generation, order decisions | < 2μs | Dedicated core per strategy |
| **Pre-Trade Risk** | Position/exposure checks | < 200ns (inline) | Same thread as strategy |
| **OMS / Smart Router** | Order lifecycle, venue selection | < 500ns | Dedicated core |
| **Exchange Gateway** | Protocol encode/transmit | < 300ns | Dedicated core, kernel bypass TX |
| **WAL Writer** | Persist decisions for recovery | Off critical path | Separate core, async |
| **Telemetry** | Metrics, timestamps, audit | Off critical path | Separate core, async |

### 2.3 End-to-End Data Flow

```
Exchange Match → [1] NIC RX (hardware timestamp)
              → [2] DPDK poll / OpenOnload
              → [3] Feed Handler: decode wire format (SBE/ITCH/PITCH)
              → [4] Normalize to internal MarketUpdate struct
              → [5] Order Book Builder: apply update (price/qty at level)
              → [6] Strategy Engine: evaluate signal
              → [7] IF signal triggered:
                    → [7a] Pre-Trade Risk: check limits (inline, < 200ns)
                    → [7b] OMS: create order, assign ClOrdID
                    → [7c] Smart Router: select venue
                    → [7d] Exchange Gateway: encode FIX/binary
                    → [7e] NIC TX (hardware timestamp)
              → [8] Async: WAL write, telemetry emit
```

**Critical path**: Steps [1]→[7e] must complete in < 5μs total.
**Off-path**: Step [8] runs on separate cores and does not block the hot path.

---

## 3. Low-Latency Design

### 3.1 Kernel Bypass Networking

The Linux kernel network stack adds 5-15μs of latency per packet. For HFT, this is unacceptable.

**Solarflare OpenOnload (Production Standard)**:
```cpp
// OpenOnload: user-space TCP/UDP stack, transparent to application
// Just set environment variables and run your existing socket code
// EF_POLL_USEC=100000       - busy-poll duration
// EF_SPIN_USEC=100000       - spin rather than block
// EF_UDP_RCVBUF=8388608     - large receive buffer
// EF_TIMESTAMPING_REPORTING=1 - hardware timestamps
// onload --profile=latency ./trading_engine

// For ef_vi (raw Solarflare API, lowest latency):
#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>

struct EfViReceiver {
    ef_driver_handle   dh;
    ef_pd              pd;
    ef_vi              vi;
    ef_memreg          memreg;
    ef_filter_spec     filter_spec;

    void init(const char* interface, int port) {
        ef_driver_open(&dh);
        ef_pd_alloc_by_name(&pd, dh, interface, EF_PD_DEFAULT);
        ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, -1, -1,
                            nullptr, -1, EF_VI_FLAGS_DEFAULT);

        // Register memory for zero-copy DMA
        void* buf = aligned_alloc(4096, RX_BUF_SIZE * NUM_BUFS);
        ef_memreg_alloc(&memreg, dh, &pd, dh, buf, RX_BUF_SIZE * NUM_BUFS);

        // Set up UDP filter for multicast market data
        ef_filter_spec_init(&filter_spec, EF_FILTER_FLAG_NONE);
        ef_filter_spec_set_ip4_local(&filter_spec, IPPROTO_UDP, 
                                      inet_addr("224.0.28.1"), htons(port));
        ef_vi_filter_add(&vi, dh, &filter_spec, nullptr);
    }

    // Hot loop — runs on dedicated core, never yields
    [[gnu::hot]] void poll_loop() {
        ef_event events[EF_VI_EVENT_POLL_MIN_EVS];
        while (running_) {
            int n = ef_eventq_poll(&vi, events, EF_VI_EVENT_POLL_MIN_EVS);
            for (int i = 0; i < n; ++i) {
                if (EF_EVENT_TYPE(events[i]) == EF_EVENT_TYPE_RX) {
                    auto* pkt = get_rx_buf(EF_EVENT_RX_RQ_ID(events[i]));
                    process_packet(pkt);      // → feed handler
                    ef_vi_receive_post(&vi, /*dma_addr*/, /*id*/);
                }
            }
        }
    }
};
```

**DPDK (Alternative for multi-vendor NICs)**:
```cpp
#include <rte_ethdev.h>
#include <rte_mbuf.h>

// DPDK gives ~200-500ns per-packet latency (vs 5-15μs kernel)
// Requires hugepages, core isolation, and dedicated NIC ports
[[gnu::hot]] void dpdk_rx_loop(uint16_t port_id, uint16_t queue_id) {
    constexpr uint16_t BURST_SIZE = 32;
    rte_mbuf* pkts[BURST_SIZE];

    while (running_) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts, BURST_SIZE);
        for (uint16_t i = 0; i < nb_rx; ++i) {
            auto* eth_hdr = rte_pktmbuf_mtod(pkts[i], rte_ether_hdr*);
            process_market_data(pkts[i]);       // zero-copy: pointer into DMA buffer
            rte_pktmbuf_free(pkts[i]);
        }
    }
}
```

### 3.2 Lock-Free Data Structures

> [!CAUTION]
> Lock contention is the #1 killer of deterministic latency. A single mutex can add 1-10μs of jitter. The hot path must be entirely lock-free.

**SPSC Ring Buffer (Inter-thread Communication)**:
```cpp
// Single-Producer, Single-Consumer lock-free ring
// Cache-line padded to prevent false sharing
template <typename T, size_t Capacity>
class alignas(64) SPSCRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    struct alignas(64) {                // Producer cache line
        std::atomic<size_t> head_{0};
    };
    struct alignas(64) {                // Consumer cache line
        std::atomic<size_t> tail_{0};
    };
    struct alignas(64) {                // Data
        std::array<T, Capacity> buffer_;
    };

public:
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire)) return false;  // full
        buffer_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& item) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;    // empty
        item = buffer_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return true;
    }
};
```

### 3.3 NUMA Awareness & CPU Pinning

```
# Boot parameters (GRUB_CMDLINE_LINUX):
# isolcpus=4-15 nohz_full=4-15 rcu_nocbs=4-15
# nosmt                          (disable hyperthreading — reduces jitter)
# default_hugepagesz=1G hugepagesz=1G hugepages=8
# intel_pstate=disable           (lock CPU frequency)
# processor.max_cstate=0         (disable C-states)
# idle=poll                      (never enter idle states)
# skew_tick=1                    (reduce timer interrupt contention)
# tsc=reliable                   (trust TSC for timestamps)

# BIOS settings:
# - Disable Turbo Boost (causes frequency transitions = jitter)
# - Disable C-states and P-states
# - Disable NUMA interleaving
# - Enable Performance mode
```

```cpp
#include <sched.h>
#include <numa.h>

void pin_thread_to_core(std::thread& t, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cpuset), &cpuset);
    if (rc != 0) throw std::runtime_error("Failed to pin thread to core");
}

// Allocate memory on the same NUMA node as the pinned core
void* numa_alloc_on_node(size_t size, int node) {
    void* ptr = numa_alloc_onnode(size, node);
    if (!ptr) throw std::bad_alloc();
    // Touch pages to ensure physical allocation (avoid page faults on hot path)
    memset(ptr, 0, size);
    return ptr;
}

// REVISED Core Assignment Plan:
//
// Key insight: Minimize SPSC ring hops on the critical path. Each ring hop
// costs 30-80ns plus a full cache-line migration between cores. Fuse
// sequential pipeline stages onto the same core whenever possible.
//
// Core 0:  OS + housekeeping (never use for trading)
// Core 1:  Management / telemetry / monitoring exporter
// Core 2:  WAL writer (io_uring async)
// Core 3:  Post-trade risk aggregation (off-path)
// Core 4:  Feed Handler + Order Book Builder (FUSED)     — ISOLATED
//          └── Decode + book update in same function call, no ring between them
//          └── Pushes compact BookSignal to strategy core via single SPSC ring
// Core 5:  Strategy #1 + Pre-Trade Risk + OMS + Gateway  — ISOLATED
//          └── Pre-trade risk is inlined (120-200ns, not worth a separate core)
//          └── OMS + gateway encode + NIC TX fused to eliminate 2 ring hops
//          └── Reduces critical path from 5 SPSC hops to 1 SPSC hop
// Core 6:  Strategy #2 (if needed, same fused topology)  — ISOLATED
// Core 7:  Standby / overflow
//
// Net savings: ~200-500ns from eliminated ring hops + cache migrations
// Tradeoff: Less modularity, harder to profile individual components
// Mitigation: Use rdtsc_start()/rdtsc_end() checkpoints within the fused loop
```

### 3.4 Cache Optimization

```cpp
// Prefetch the next expected cache line before processing
__builtin_prefetch(&order_book_levels[next_idx], 0 /* read */, 3 /* high locality */);

// Ensure hot data fits in L1d (32-48KB per core)
// Order book top-of-book: 2 sides × 10 levels × 16 bytes = 320 bytes (fits in 5 cache lines)
// Strategy state: < 1KB typical — fits entirely in L1

// Avoid pointer chasing: use arrays/structs of arrays, not linked lists
// BAD:  std::map<Price, Level>  — scattered memory, cache-hostile
// GOOD: flat sorted array of PriceLevel structs — contiguous, prefetchable
```

### 3.5 Language Comparison for Hot Path

| Criterion | C++20/23 | Rust | Java (+ Azul/GraalVM) |
|---|---|---|---|
| **Typical tick-to-trade** | 2-5μs | 3-7μs | 10-50μs |
| **Memory control** | Full (placement new, allocators) | Good (unsafe blocks for raw pointers) | Limited (GC pauses 50-500μs) |
| **Lock-free primitives** | `std::atomic`, inline asm | `AtomicCell`, crossbeam | `VarHandle`, mostly safe |
| **Kernel bypass** | Native (ef_vi, DPDK link C) | FFI overhead (~20-50ns) | JNI overhead (~100-300ns) |
| **Compile-time optimization** | Excellent (constexpr, templates) | Excellent (const generics, monomorphization) | JIT warmup (seconds to minutes) |
| **Jitter** | Lowest (no GC, no runtime) | Low (no GC, no runtime) | High (GC safepoints, JIT deopt) |
| **Industry adoption (HFT)** | Dominant (>80% of firms) | Growing (new systems) | Legacy + some shops (Jane Street uses OCaml) |
| **Verdict** | **Standard for hot path** | Good for new builds, but ecosystem gaps | Only for non-latency-critical paths |

> [!IMPORTANT]
> **C++ is the only viable choice for the hot path at sub-5μs targets.** Rust is acceptable if the team has expertise, but FFI friction with C exchange libraries and FPGA toolchains makes C++ pragmatically superior. Java is used only for backtesting, analytics, and GUI dashboards.

### 3.6 FPGA Acceleration

**When to use FPGAs:**
- Latency target < 1μs (tick-to-trade)
- Deterministic latency is more important than flexibility
- Strategy is simple and well-defined (e.g., pure arbitrage, simple market making)
- Regulatory changes to strategy logic are infrequent

**FPGA Pipeline Architecture:**
```
NIC PHY → MAC → IP/UDP Parse → Market Data Decode → Signal Logic → Order Encode → MAC → NIC PHY
                        |                                      |
                    ┌───▼───┐                            ┌─────▼─────┐
                    │ PCIe  │                            │  PCIe     │
                    │ to CPU│                            │  from CPU │
                    │ (slow │                            │  (config  │
                    │  path)│                            │   only)   │
                    └───────┘                            └───────────┘

Total wire-to-wire: ~800ns - 1.5μs
vs CPU path:        ~3-5μs
```

**Hardware**: Xilinx Alveo U55C, Intel Agilex, Cisco Nexus SmartNIC
**HDL**: Verilog/SystemVerilog (most firms), or HLS (C++ → RTL via Vitis HLS)
**Tradeoff**: 3-6 month development cycle per strategy change vs. hours in software

---

## 4. Market Data Pipeline

### 4.1 Feed Handler Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                       FEED HANDLER (Core 4)                      │
│                                                                  │
│  NIC ──── ef_vi/DPDK ────┬──── Decoder ────┬──── Normalizer ──► │
│        (zero-copy RX)    │   (ITCH 5.0)   │   (internal fmt)   │
│                           │   (PITCH)      │                     │
│                           │   (CME MDP3)   │                     │
│                           │   (OPRA)       │                     │
│                           └────────────────┘                     │
│                                                                  │
│  Gap Detection ──── Retransmit Request ──── Recovery             │
│  (sequence #s)      (TCP snapshot)          (rebuild book)       │
└──────────────────────────────────────────────────────────────────┘
```

### 4.2 Wire Protocol Handling

**NASDAQ ITCH 5.0 Decoder (example)**:
```cpp
// ITCH messages are binary, no field delimiters
// Parse directly from DMA buffer — zero-copy

#pragma pack(push, 1)
struct ITCHAddOrder {
    char     msg_type;        // 'A'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];    // 6-byte nanosecond timestamp
    uint64_t order_ref;
    char     side;            // 'B' or 'S'
    uint32_t shares;
    char     stock[8];
    uint32_t price;           // price × 10000
};

struct ITCHOrderExecuted {
    char     msg_type;        // 'E'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t  timestamp[6];
    uint64_t order_ref;
    uint32_t executed_shares;
    uint64_t match_number;
};
#pragma pack(pop)

// Compile-time dispatch table — no virtual calls, no branches
template <char MsgType>
struct ITCHHandler;

template <>
struct ITCHHandler<'A'> {
    static void handle(const void* data, OrderBook& book) {
        auto* msg = static_cast<const ITCHAddOrder*>(data);
        book.add_order(msg->order_ref, msg->side, 
                       ntohl(msg->price), ntohl(msg->shares));
    }
};

[[gnu::hot, gnu::flatten]]
void dispatch_itch(const uint8_t* buf, size_t len, OrderBook& book) {
    char msg_type = buf[0];
    // Use jump table — compiler optimizes switch to O(1) lookup
    switch (msg_type) {
        case 'A': ITCHHandler<'A'>::handle(buf, book); break;
        case 'E': ITCHHandler<'E'>::handle(buf, book); break;
        case 'X': ITCHHandler<'X'>::handle(buf, book); break;
        case 'D': ITCHHandler<'D'>::handle(buf, book); break;
        case 'U': ITCHHandler<'U'>::handle(buf, book); break;
        case 'P': ITCHHandler<'P'>::handle(buf, book); break;
        // ... ~20 message types total
        default: break; // unknown message — log off-path
    }
}
```

### 4.3 Gap Detection & Recovery

```cpp
class GapDetector {
    uint64_t expected_seq_ = 1;
    
    // Store gap ranges for bulk recovery
    struct Gap { uint64_t begin; uint64_t end; };
    boost::lockfree::spsc_queue<Gap, boost::lockfree::capacity<256>> gaps_;

public:
    [[gnu::hot]] bool check(uint64_t seq) noexcept {
        if (__builtin_expect(seq == expected_seq_, 1)) {
            ++expected_seq_;
            return true;     // normal case — no gap
        }
        if (seq > expected_seq_) {
            gaps_.push({expected_seq_, seq - 1});
            expected_seq_ = seq + 1;
            return true;     // process this packet, request gap fill async
        }
        return false;        // duplicate or old packet — discard
    }
};

// Recovery runs on a separate thread, NOT on the hot path
// Uses TCP retransmission channel to request missing messages
// Book is rebuilt from snapshot + incremental replay
```

### 4.4 Order Book Data Structure

```cpp
// Cache-optimized order book — flat arrays, no heap allocation on hot path

// IMPROVED: PriceLevel padded to exactly 32 bytes (2 per cache line, zero waste)
// Original was 24 bytes → 2.67 per cache line, wasting 16 bytes per line.
struct alignas(32) PriceLevel {
    int64_t  price;          // Fixed-point: actual_price × 1e8
    int64_t  quantity;       // Total shares at this level
    uint32_t order_count;    // Number of orders (useful for signal)
    uint32_t flags;          // Implied, RFQ, etc.
    uint64_t _padding;       // Explicit alignment — 2 levels per cache line
};  // 32 bytes exactly
static_assert(sizeof(PriceLevel) == 32);

// IMPROVED: Pre-allocated order pool with O(1) lookup, no hash map on hot path.
//
// WHY NOT robin_hood::unordered_flat_map?
// Hash maps — even flat ones — have unpredictable latency spikes during rehashing.
// A single rehash during a fast market (NFP release: 500K+ msgs/sec) causes a
// multi-microsecond stall. The pool below never rehashes because it's pre-sized.

struct alignas(64) OrderPool {
    static constexpr size_t CAPACITY = 1 << 20;  // ~1M orders, pre-allocated
    static constexpr size_t MASK = CAPACITY - 1;

    struct Slot {
        uint64_t order_ref;   // 0 = empty
        int64_t  price;
        int32_t  quantity;
        int8_t   side;
        int8_t   active;      // 0 = deleted/empty, 1 = live
        int16_t  padding;
    };
    static_assert(sizeof(Slot) == 24);

    Slot slots_[CAPACITY];  // Pre-allocated, pre-faulted at startup

    // Fibonacci hashing — much better distribution than modulo for sequential IDs
    [[gnu::hot]] size_t hash(uint64_t ref) const noexcept {
        return (ref * 11400714819323198485ULL) >> (64 - 20);  // top 20 bits
    }

    [[gnu::hot]] Slot* find(uint64_t ref) noexcept {
        size_t idx = hash(ref);
        for (int probe = 0; probe < 8; ++probe) {  // max 8 probes
            auto& s = slots_[(idx + probe) & MASK];
            if (s.order_ref == ref && s.active) return &s;
            if (s.order_ref == 0) return nullptr;   // empty = not found
        }
        return nullptr;
    }

    [[gnu::hot]] void insert(uint64_t ref, int64_t price, int32_t qty, int8_t side) noexcept {
        size_t idx = hash(ref);
        for (int probe = 0; probe < 8; ++probe) {
            auto& s = slots_[(idx + probe) & MASK];
            if (s.order_ref == 0 || !s.active) {
                s = {ref, price, qty, side, 1, 0};
                return;
            }
        }
        // Pool full — critical error. Log off-path, do NOT throw on hot path.
    }

    [[gnu::hot]] void remove(uint64_t ref) noexcept {
        if (auto* s = find(ref)) s->active = 0;
    }
};

struct alignas(64) OrderBook {
    static constexpr int MAX_LEVELS = 20;

    // Bids sorted descending, asks sorted ascending
    // Top-of-book at index 0
    std::array<PriceLevel, MAX_LEVELS> bids;
    std::array<PriceLevel, MAX_LEVELS> asks;
    int bid_depth = 0;
    int ask_depth = 0;

    // L3 order tracking — pre-allocated pool, no rehashing
    OrderPool orders;

    [[gnu::hot]] void add_order(uint64_t ref, char side, int64_t price, int64_t qty) {
        orders.insert(ref, price, qty, side);
        if (side == 'B') insert_bid(price, qty);
        else             insert_ask(price, qty);
    }

    [[gnu::hot]] void cancel_order(uint64_t ref) {
        if (auto* slot = orders.find(ref)) {
            if (slot->side == 'B') remove_bid(slot->price, slot->quantity);
            else                   remove_ask(slot->price, slot->quantity);
            slot->active = 0;
        }
    }

    // BBO (best bid/offer) — most accessed, always at index 0
    [[nodiscard]] int64_t best_bid()  const noexcept { return bids[0].price; }
    [[nodiscard]] int64_t best_ask()  const noexcept { return asks[0].price; }
    [[nodiscard]] int64_t spread()    const noexcept { return asks[0].price - bids[0].price; }
    [[nodiscard]] int64_t mid_price() const noexcept { return (bids[0].price + asks[0].price) / 2; }
};
```

### 4.5 Memory Layout Optimizations

```cpp
// Struct of Arrays (SoA) for multi-instrument scanning
// When strategy needs to scan BBO across 5000 instruments:
struct alignas(64) BBOSnapshot {
    // All best bids contiguous → vectorizable SIMD scan
    std::array<int64_t, 8192> best_bids;
    std::array<int64_t, 8192> best_asks;
    std::array<int64_t, 8192> bid_sizes;
    std::array<int64_t, 8192> ask_sizes;
    std::array<uint64_t, 8192> last_update_ns;  // nanosecond timestamp
};

// Pre-warm TLB entries to avoid latency spikes
void prewarm_pages(void* base, size_t size) {
    volatile char* p = static_cast<volatile char*>(base);
    for (size_t i = 0; i < size; i += 4096) {
        (void)p[i];   // Touch each page to fault it into TLB
    }
}
```

---

## 5. Strategy Engine

### 5.1 Execution Model

**Polling Model (Preferred for < 5μs Latency)**:
```cpp
// Strategy thread busy-polls the order book for changes
// No context switches, no syscalls, no sleeping
class StrategyEngine {
    SPSCRing<MarketUpdate, 65536>& update_ring_;
    StrategyPlugin* active_strategy_;
    SPSCRing<OrderRequest, 4096>& order_ring_;

public:
    [[gnu::hot, noreturn]] void run() {
        MarketUpdate update;
        while (true) {
            if (update_ring_.try_pop(update)) {
                // Update local book state (or reference shared book)
                auto signal = active_strategy_->on_update(update);
                if (signal.action != Action::NONE) {
                    order_ring_.try_push(signal.to_order_request());
                }
            }
            // No else/sleep — pure spin-wait
            // CPU core is 100% dedicated to this
        }
    }
};
```

**Event-Driven Model (For < 50μs targets, allows multiplexing)**:
```cpp
// Uses io_uring or epoll for multiplexed event handling
// Lower CPU usage but higher tail latency
class EventDrivenEngine {
    io_uring ring_;

    void run() {
        while (true) {
            io_uring_cqe* cqe;
            io_uring_wait_cqe(&ring_, &cqe);
            process_event(cqe);
            io_uring_cqe_seen(&ring_, cqe);
        }
    }
};
```

| Model | p50 Latency | p99 Latency | CPU Usage | Best For |
|---|---|---|---|---|
| **Busy-poll** | ~1μs | ~3μs | 100% per core | Market making, arb |
| **Event-driven** | ~5μs | ~20μs | 10-30% per core | Stat arb, slower strategies |
| **Hybrid** | ~2μs | ~8μs | 60-80% per core | Mixed workloads |

### 5.2 Plugin Architecture

```cpp
// === OPTION A: Virtual dispatch (simpler, ~2-5ns overhead per call) ===
// Use when: multiple strategies loaded at runtime, flexibility > performance

class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Called on every book update — MUST be fast (< 1μs)
    [[nodiscard]] virtual Signal on_book_update(
        InstrumentId id, const OrderBook& book) noexcept = 0;

    // Called on order fill/cancel acknowledgment
    virtual void on_execution_report(const ExecutionReport& report) noexcept = 0;

    // Called once at startup — can be slow
    virtual void initialize(const StrategyConfig& config) = 0;

    // Strategy metadata
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// === OPTION B: CRTP static dispatch (zero-overhead, compiler can inline) ===
// Use when: maximum performance, strategy type known at compile time.
// Tradeoff: can't swap strategies at runtime without recompiling the engine.
//           But since strategies are NOT hot-reloaded during trading anyway,
//           this is essentially free performance.
//
// The real cost of virtual dispatch isn't the 2-5ns indirect call — it's that
// the compiler CANNOT inline across a virtual call boundary. If on_book_update()
// is a tight 200ns function, lost inlining/optimization adds 10-20% overhead.

template <typename Derived>
class StrategyBase {
public:
    [[gnu::hot, gnu::flatten]]
    Signal on_book_update(InstrumentId id, const OrderBook& book) noexcept {
        return static_cast<Derived*>(this)->on_book_update_impl(id, book);
    }

    void on_execution_report(const ExecutionReport& report) noexcept {
        static_cast<Derived*>(this)->on_execution_report_impl(report);
    }
};

// Example: MarketMaker using CRTP — all calls are statically dispatched
class MarketMakerV3 : public StrategyBase<MarketMakerV3> {
    friend class StrategyBase<MarketMakerV3>;

    [[gnu::hot]]
    Signal on_book_update_impl(InstrumentId id, const OrderBook& book) noexcept {
        // Strategy logic — fully inlineable by compiler.
        // Compiler sees through StrategyBase::on_book_update → this function.
        // No vtable lookup, no indirect branch, no icache miss.
        return {};
    }

    void on_execution_report_impl(const ExecutionReport& report) noexcept {
        // ... handle fill
    }
};

// The strategy engine can be templated for CRTP dispatch:
template <typename Strategy>
class TypedStrategyEngine {
    Strategy strategy_;  // statically dispatched — zero overhead
    // ... engine loop, all calls to strategy are devirtualized + inlined
};

// Strategies are loaded as shared objects (.so) at startup
// NOT hot-reloaded during trading (too risky)
class StrategyLoader {
    std::vector<void*> handles_;
    
public:
    std::unique_ptr<IStrategy> load(const std::string& path) {
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) throw std::runtime_error(dlerror());
        handles_.push_back(handle);

        auto factory = reinterpret_cast<IStrategy*(*)()>(
            dlsym(handle, "create_strategy"));
        return std::unique_ptr<IStrategy>(factory());
    }
};
```

### 5.3 Deterministic Execution & Reproducibility

```cpp
// All strategies operate on a deterministic event stream:
// 1. Market data events (timestamped, sequenced)
// 2. Timer events (logical clock, not wall clock during replay)
// 3. Execution reports (timestamped, sequenced)

// NO system calls on the hot path (time via RDTSC, not clock_gettime)
//
// IMPORTANT: Plain `rdtsc` is NOT serializing on modern out-of-order CPUs.
// Instructions can be reordered around it, giving incorrect measurements.
// Use `lfence + rdtsc` for start timestamps and `rdtscp` for end timestamps.

// Start measurement: lfence ensures all prior instructions retire first
[[gnu::always_inline]] inline uint64_t rdtsc_start() noexcept {
    uint32_t lo, hi;
    __asm__ __volatile__(
        "lfence\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// End measurement: rdtscp waits for all prior instructions to complete
[[gnu::always_inline]] inline uint64_t rdtsc_end() noexcept {
    uint32_t lo, hi, aux;
    __asm__ __volatile__(
        "rdtscp"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :: "memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// Backward-compat wrapper for non-measurement use (e.g., generating unique IDs)
[[gnu::always_inline]] inline uint64_t rdtsc() noexcept {
    return rdtsc_end();  // serializing version by default
}

// Convert TSC to nanoseconds using calibrated frequency
// Calibrate at startup against CLOCK_MONOTONIC, then re-calibrate every
// 60 seconds on the telemetry thread to correct for thermal drift (~100ppm).
struct TSCCalibration {
    double tsc_to_ns_ratio;    // e.g., ~0.3 ns/tick for 3.5GHz CPU
    uint64_t base_tsc;
    uint64_t base_ns;
    std::atomic<double> correction_factor{1.0};  // updated by telemetry thread

    uint64_t tsc_to_ns(uint64_t tsc) const noexcept {
        return base_ns + static_cast<uint64_t>(
            (tsc - base_tsc) * tsc_to_ns_ratio 
            * correction_factor.load(std::memory_order_relaxed));
    }

    // Called every 60s by telemetry thread — not on hot path
    void recalibrate() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t wall_ns = ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
        uint64_t current_tsc = rdtsc_end();
        double actual_ratio = double(wall_ns - base_ns) / double(current_tsc - base_tsc);
        correction_factor.store(actual_ratio / tsc_to_ns_ratio, std::memory_order_relaxed);
    }
};

// For backtesting: replace rdtsc() with replayed timestamp
// Strategy code is identical in production and simulation
class Clock {
public:
    virtual uint64_t now_ns() noexcept = 0;
};

class WallClock : public Clock {
    uint64_t now_ns() noexcept override { /* rdtsc + calibration */ }
};

class SimulatedClock : public Clock {
    uint64_t current_ns_ = 0;
    uint64_t now_ns() noexcept override { return current_ns_; }
    void advance(uint64_t ns) { current_ns_ = ns; }
};
```

---

## 6. Order Execution System

### 6.1 Smart Order Router

```cpp
// Venue selection based on:
// 1. Current BBO at each venue (price improvement)
// 2. Historical latency to each venue (faster fills)
// 3. Fee schedule (maker/taker rebates)
// 4. Current position at venue (netting)
// 5. Regulatory requirements (Reg NMS / best execution)

struct VenueMetrics {
    int64_t  best_bid;
    int64_t  best_ask;
    int64_t  bid_size;
    int64_t  ask_size;
    uint32_t latency_p50_ns;
    uint32_t latency_p99_ns;
    int32_t  maker_fee_bps;     // negative = rebate
    int32_t  taker_fee_bps;
    bool     active;
};

class SmartRouter {
    std::array<VenueMetrics, 16> venues_;

public:
    [[gnu::hot]] VenueId select_venue(
        Side side, int64_t price, int64_t qty) noexcept
    {
        VenueId best = VenueId::NONE;
        int64_t best_score = INT64_MIN;

        for (int i = 0; i < num_venues_; ++i) {
            if (!venues_[i].active) continue;

            int64_t price_at_venue = (side == Side::BUY) 
                ? venues_[i].best_ask : venues_[i].best_bid;
            int64_t size_at_venue = (side == Side::BUY)
                ? venues_[i].ask_size : venues_[i].bid_size;

            // Score = price improvement - latency cost - fee
            int64_t score = (price - price_at_venue) * (side == Side::BUY ? 1 : -1)
                          - venues_[i].latency_p50_ns / 100    // latency penalty
                          - venues_[i].taker_fee_bps;          // fee cost

            if (score > best_score && size_at_venue >= qty) {
                best_score = score;
                best = static_cast<VenueId>(i);
            }
        }
        return best;
    }
};
```

### 6.2 Exchange Protocol Adapters

**FIX Protocol (via custom zero-alloc encoder)**:
```cpp
// FIX encoding — pre-computed static fields, zero allocation
// NOT using QuickFIX (adds 20-50μs of overhead)

class FixEncoder {
    char buffer_[2048];
    int  pos_ = 0;

    // Pre-computed: "8=FIX.4.2\x01" "9=..." "35=D\x01" "49=SENDER\x01" ...
    // The static prefix is memcpy'd once, then only variable fields are written

    void append_tag_value(int tag, std::string_view value) noexcept {
        // Write tag
        pos_ += itoa_fast(tag, buffer_ + pos_);
        buffer_[pos_++] = '=';
        // Write value
        std::memcpy(buffer_ + pos_, value.data(), value.size());
        pos_ += value.size();
        buffer_[pos_++] = '\x01';  // SOH delimiter
    }

    void append_tag_int(int tag, int64_t value) noexcept {
        pos_ += itoa_fast(tag, buffer_ + pos_);
        buffer_[pos_++] = '=';
        pos_ += itoa_fast(value, buffer_ + pos_);
        buffer_[pos_++] = '\x01';
    }

public:
    [[gnu::hot]] std::span<const char> encode_new_order(
        const NewOrderSingle& order) noexcept
    {
        pos_ = 0;
        // BeginString, BodyLength (placeholder), MsgType
        static constexpr char PREFIX[] = "8=FIX.4.2\x01" "9=000\x01" "35=D\x01";
        std::memcpy(buffer_, PREFIX, sizeof(PREFIX) - 1);
        pos_ = sizeof(PREFIX) - 1;

        append_tag_value(49, sender_comp_id_);      // SenderCompID
        append_tag_value(56, target_comp_id_);      // TargetCompID
        append_tag_int(34, ++msg_seq_num_);          // MsgSeqNum
        append_tag_value(52, format_timestamp());    // SendingTime
        append_tag_value(11, order.cl_ord_id);       // ClOrdID
        append_tag_value(55, order.symbol);          // Symbol
        append_tag_int(54, static_cast<int>(order.side)); // Side
        append_tag_int(38, order.qty);               // OrderQty
        append_tag_int(44, order.price);             // Price
        append_tag_int(40, static_cast<int>(order.ord_type)); // OrdType

        // Fix body length and compute checksum
        fixup_body_length();
        append_checksum();

        return {buffer_, static_cast<size_t>(pos_)};
    }
};
```

**Binary Protocol (CME iLink 3 / SBE)**:
```cpp
// SBE (Simple Binary Encoding) — fixed-layout, zero parsing overhead
// Message templates are generated at compile time from XML schema

// Example: CME iLink 3 NewOrderSingle
#pragma pack(push, 1)
struct SBENewOrderSingle {
    // SBE Message Header (8 bytes)
    uint16_t block_length;
    uint16_t template_id;      // 514 for NewOrderSingle
    uint16_t schema_id;
    uint16_t version;

    // Body
    int64_t  price;            // PRICENULL9 mantissa
    int32_t  order_qty;
    int32_t  security_id;
    uint8_t  side;             // 1=Buy, 2=Sell
    uint64_t order_request_id;
    uint64_t sending_time_epoch;
    char     cl_ord_id[20];
    // ... additional fields
};
#pragma pack(pop)

// Encoding: just fill the struct and send — zero transformation
// ~50ns encode time vs ~500ns for FIX text protocol
```

### 6.3 Failure Handling

```cpp
// Exchange connection state machine
enum class ConnState { 
    DISCONNECTED, CONNECTING, LOGGING_ON, ACTIVE, 
    RESENDING, DRAINING, DISCONNECTING 
};

class ExchangeConnection {
    ConnState state_ = ConnState::DISCONNECTED;
    uint32_t retry_count_ = 0;
    static constexpr uint32_t MAX_RETRIES = 3;
    static constexpr auto RETRY_DELAYS_MS = std::array{100, 500, 2000};

    // Sequence number tracking for FIX gap fill
    uint32_t expected_seq_in_ = 1;
    uint32_t next_seq_out_ = 1;

    void on_disconnect() {
        state_ = ConnState::DISCONNECTED;
        if (retry_count_ < MAX_RETRIES) {
            schedule_reconnect(RETRY_DELAYS_MS[retry_count_++]);
        } else {
            // Escalate: kill switch, alert on-call
            risk_manager_.emergency_flatten();
            alert_system_.fire(Severity::CRITICAL, 
                "Exchange connection lost after max retries");
        }
    }

    // Handle sequence gaps (FIX ResendRequest)
    void on_sequence_gap(uint32_t received, uint32_t expected) {
        state_ = ConnState::RESENDING;
        send_resend_request(expected, received - 1);
    }
};
```

### 6.4 Per-Exchange Latency Benchmarking

```cpp
// Every order gets hardware-timestamped at NIC TX and exchange ACK at NIC RX
// Maintained per-venue, per-message-type

struct LatencyTracker {
    // HdrHistogram — constant memory, O(1) record, O(1) percentile query
    // Logs latency in nanoseconds with 3 significant figures
    hdr_histogram* histogram_;

    void record(uint64_t tx_hw_ns, uint64_t rx_hw_ns) {
        int64_t rtt_ns = rx_hw_ns - tx_hw_ns;
        hdr_record_value(histogram_, rtt_ns);
    }

    void report() {
        printf("p50=%ldns  p99=%ldns  p999=%ldns  max=%ldns\n",
               hdr_value_at_percentile(histogram_, 50.0),
               hdr_value_at_percentile(histogram_, 99.0),
               hdr_value_at_percentile(histogram_, 99.9),
               hdr_max(histogram_));
    }
};
```

---

## 7. Risk Management

### 7.1 Pre-Trade Risk (Inline — On Hot Path)

> [!WARNING]
> Pre-trade risk checks are **regulatory mandated** (SEC Rule 15c3-5). They cannot be bypassed, even for latency. The implementation must be O(1) and < 200ns.

```cpp
class PreTradeRisk {
    // === TWO-TIER RISK ARCHITECTURE ===
    //
    // PROBLEM with single-tier: If multiple strategies trade the same instrument,
    // they share InstrumentRisk state. Concurrent non-atomic reads/writes from
    // different strategy threads cause data races (undefined behavior) and can
    // allow position limits to be exceeded.
    //
    // SOLUTION: Per-strategy LOCAL state (no contention) + GLOBAL atomic aggregation.
    // - Tier 1: Each strategy thread has its own LocalInstrumentRisk (no sharing)
    // - Tier 2: Global position tracked via atomics, updated after each decision
    // - Risk limits checked against GLOBAL position (catches cross-strategy exposure)

    // Per-strategy view — only one writer, zero contention
    struct alignas(64) LocalInstrumentRisk {
        int64_t net_position{0};
        int64_t gross_notional{0};
        int32_t orders_this_second{0};
        int32_t _padding{0};
    };

    // Global limits — read-only during trading, configured at startup
    struct InstrumentLimits {
        int64_t max_position;       // max absolute net position (all strategies)
        int64_t max_notional;       // max gross notional (all strategies)
        int32_t max_orders_per_sec; // rate limit per strategy
        int64_t max_order_size;     // single order size limit
        int64_t fat_finger_price;   // max deviation from mid price
    };

    // Aggregated position across all strategies — updated atomically
    struct alignas(64) GlobalPosition {
        std::atomic<int64_t> net_position{0};
        std::atomic<int64_t> gross_notional{0};
    };

    // Flat array indexed by InstrumentId (uint16_t) — no hash lookup
    std::array<LocalInstrumentRisk, 65536> local_state_;   // per-strategy, thread-local
    std::array<InstrumentLimits, 65536> limits_;            // read-only after startup
    std::array<GlobalPosition, 65536>& global_positions_;  // shared across all strategies

public:
    PreTradeRisk(std::array<GlobalPosition, 65536>& global)
        : global_positions_(global) {}

    // Returns PASS/REJECT — uses branch hints for the common (pass) case
    [[gnu::hot, nodiscard]]
    RiskResult check(InstrumentId id, Side side, int64_t qty,
                     int64_t price, int64_t mid_price) noexcept
    {
        auto& local = local_state_[id];
        const auto& lim = limits_[id];

        int64_t delta = (side == Side::BUY) ? qty : -qty;

        // 1. Position limit — checked against GLOBAL position (cross-strategy)
        int64_t current_global = global_positions_[id].net_position
            .load(std::memory_order_relaxed);
        int64_t new_global_pos = current_global + delta;
        if (__builtin_expect(std::abs(new_global_pos) > lim.max_position, 0))
            return RiskResult::POSITION_BREACH;

        // 2. Order size limit
        if (__builtin_expect(qty > lim.max_order_size, 0))
            return RiskResult::SIZE_BREACH;

        // 3. Fat finger check (price too far from mid)
        int64_t deviation = std::abs(price - mid_price);
        if (__builtin_expect(deviation > lim.fat_finger_price, 0))
            return RiskResult::PRICE_BREACH;

        // 4. Notional limit — checked against GLOBAL notional
        int64_t order_notional = price * qty;
        int64_t current_notional = global_positions_[id].gross_notional
            .load(std::memory_order_relaxed);
        if (__builtin_expect(current_notional + order_notional > lim.max_notional, 0))
            return RiskResult::NOTIONAL_BREACH;

        // 5. Rate limit — per-strategy (local), not global
        if (__builtin_expect(local.orders_this_second >= lim.max_orders_per_sec, 0))
            return RiskResult::RATE_BREACH;

        // All checks passed — update LOCAL state (zero contention)
        local.net_position += delta;
        local.gross_notional += order_notional;
        ++local.orders_this_second;

        // Update GLOBAL state (atomic, relaxed — slight staleness acceptable)
        // Other strategies will see the update within a few nanoseconds
        global_positions_[id].net_position.fetch_add(delta, std::memory_order_relaxed);
        global_positions_[id].gross_notional.fetch_add(
            order_notional, std::memory_order_relaxed);

        return RiskResult::PASS;
    }

    // Called once per second by timer on telemetry thread
    void reset_rate_counters() noexcept {
        for (auto& r : local_state_) {
            r.orders_this_second = 0;
        }
    }
};
```

### 7.2 Kill Switch & Circuit Breakers

```cpp
class KillSwitch {
    // Atomic flag — can be flipped from any thread (management, risk, etc.)
    std::atomic<bool> global_kill_{false};
    std::atomic<bool> per_strategy_kill_[MAX_STRATEGIES]{};

    // Hardware kill: separate management server can trigger via shared memory
    // or out-of-band TCP connection to exchange to cancel all orders

    // Exchange-level: most exchanges support "Cancel on Disconnect"
    // and "Mass Cancel" messages

public:
    void activate_global() noexcept {
        global_kill_.store(true, std::memory_order_release);
        // Simultaneously:
        // 1. Send mass cancel to all exchanges
        // 2. Begin position flattening (market orders to close)
        // 3. Alert on-call team
        // 4. Log to audit trail
    }

    [[gnu::hot, nodiscard]] 
    bool is_active() const noexcept {
        return global_kill_.load(std::memory_order_acquire);
    }
};

// Circuit breakers (automatic triggers):
// - P&L drawdown > $X in Y minutes → kill
// - Fill rate anomaly (too many/few fills) → kill
// - Message rate > threshold → throttle
// - Exchange disconnection → flatten positions
// - Clock skew > 1ms detected → halt trading
```

### 7.3 Post-Trade Risk (Off Hot Path)

```cpp
// Runs on separate core, aggregates position snapshots every 100μs
// Reports to risk dashboard and compliance systems

class PostTradeRisk {
    void run() {
        while (true) {
            // Read position snapshots from shared memory (lock-free)
            auto snapshot = position_manager_.get_snapshot();

            // Calculate portfolio-level metrics
            double total_pnl = calculate_pnl(snapshot);
            double var_95 = calculate_var(snapshot, 0.95);
            double portfolio_delta = calculate_delta(snapshot);
            double portfolio_gamma = calculate_gamma(snapshot);

            // Check portfolio-level limits
            if (total_pnl < max_drawdown_) kill_switch_.activate_global();
            if (var_95 > var_limit_) alert_("VaR breach");
            if (std::abs(portfolio_delta) > delta_limit_) alert_("Delta breach");

            // Publish to monitoring
            metrics_.publish("risk.pnl", total_pnl);
            metrics_.publish("risk.var95", var_95);

            // Regulatory: maintain real-time capital adequacy
            double capital_ratio = calculate_capital_ratio(snapshot);
            if (capital_ratio < min_capital_ratio_) {
                kill_switch_.activate_global();
                alert_("Capital adequacy breach — REGULATORY");
            }
        }
    }
};
```

---

## 8. Data Storage & State

### 8.1 Storage Tier Architecture

```
┌─────────────┐     ┌──────────────┐     ┌──────────────┐     ┌─────────────┐
│   L1/L2     │     │   Shared     │     │   NVMe SSD   │     │  Remote     │
│   Cache     │────►│   Memory     │────►│   (WAL)      │────►│  Storage    │
│             │     │   (hugepage) │     │              │     │  (TSDB)     │
│   < 10ns    │     │   < 100ns    │     │   < 10μs     │     │   < 1ms    │
│   ~48KB/core│     │   ~512GB     │     │   ~2TB       │     │   ~100TB   │
└─────────────┘     └──────────────┘     └──────────────┘     └─────────────┘

Hot Path Data         Working Set          Durability           Analytics
(book, signals)      (positions, orders)  (recovery)          (backtesting)
```

### 8.2 What Goes Where

| Data | Storage | Rationale |
|---|---|---|
| Order book state | L1/L2 cache | Updated every ~10μs, must be < 1ns access |
| Strategy state (signals, thresholds) | L1/L2 cache | Accessed every tick |
| Position state (per-instrument) | Shared memory (hugepage) | Accessed by multiple components |
| Open orders | Shared memory | OMS and risk need concurrent access |
| WAL entries | NVMe (O_DIRECT, no page cache) | Durability without adding latency to hot path |
| Historical ticks | QuestDB / TimescaleDB | Backtesting and analytics |
| Trade blotter | PostgreSQL | Compliance and reporting |
| Configuration | Flat files (TOML/YAML) | Loaded once at startup |

### 8.3 Write-Ahead Log

```cpp
// WAL writes are on the off-path — a separate core writes asynchronously
// The hot path pushes events into an SPSC ring; the WAL writer drains it

class WALWriter {
    int fd_;                      // O_DIRECT file descriptor
    char* aligned_buf_;           // 4KB-aligned buffer for O_DIRECT
    size_t buf_pos_ = 0;
    uint64_t file_offset_ = 0;

    static constexpr size_t BUF_SIZE = 4096;  // write in page-aligned chunks
    static constexpr size_t ENTRY_SIZE = 128; // fixed-size WAL entry
    static constexpr size_t SYNC_INTERVAL = 4; // fdatasync every N entries
    size_t entries_since_sync_ = 0;

    // io_uring for async writes — doesn't block the WAL writer thread
    io_uring ring_;

    struct WALEntry {
        uint64_t timestamp_ns;
        uint64_t sequence;
        uint8_t  type;            // ORDER_SENT, ORDER_FILLED, POSITION_CHANGE, etc.
        uint8_t  padding[7];
        char     payload[104];    // serialized event data
    };
    static_assert(sizeof(WALEntry) == ENTRY_SIZE);

public:
    WALWriter(const char* path) {
        fd_ = open(path, O_WRONLY | O_CREAT | O_DIRECT | O_DSYNC, 0644);
        aligned_buf_ = static_cast<char*>(aligned_alloc(4096, BUF_SIZE));

        // Initialize io_uring with 64 queue depth
        io_uring_queue_init(64, &ring_, 0);
    }

    ~WALWriter() {
        // Flush any remaining buffered entries before shutdown
        if (buf_pos_ > 0) flush_sync();
        io_uring_queue_exit(&ring_);
        free(aligned_buf_);
        close(fd_);
    }

    void write(const WALEntry& entry) {
        std::memcpy(aligned_buf_ + buf_pos_, &entry, ENTRY_SIZE);
        buf_pos_ += ENTRY_SIZE;

        if (buf_pos_ >= BUF_SIZE) {
            // Full page — submit async write via io_uring
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            io_uring_prep_write(sqe, fd_, aligned_buf_, BUF_SIZE, file_offset_);
            io_uring_submit(&ring_);
            file_offset_ += BUF_SIZE;
            buf_pos_ = 0;

            // Periodically sync to ensure durability
            // Without this, data survives process crash but not power loss
            if (++entries_since_sync_ >= SYNC_INTERVAL * (BUF_SIZE / ENTRY_SIZE)) {
                sqe = io_uring_get_sqe(&ring_);
                io_uring_prep_fsync(sqe, fd_, IORING_FSYNC_DATASYNC);
                io_uring_submit(&ring_);
                entries_since_sync_ = 0;
            }
        }
    }

    // Synchronous flush — used during shutdown and before failover
    void flush_sync() {
        if (buf_pos_ > 0) {
            // Pad remainder to page boundary for O_DIRECT
            memset(aligned_buf_ + buf_pos_, 0, BUF_SIZE - buf_pos_);
            ::write(fd_, aligned_buf_, BUF_SIZE);
            fdatasync(fd_);
            file_offset_ += BUF_SIZE;
            buf_pos_ = 0;
        }
    }

    // On startup: replay WAL to reconstruct state
    // IMPORTANT: Always cross-check against exchange drop-copy feed.
    // The drop-copy is the source of truth; WAL is the fast local backup.
    void replay(std::function<void(const WALEntry&)> handler) {
        WALEntry entry;
        lseek(fd_, 0, SEEK_SET);
        while (::read(fd_, &entry, ENTRY_SIZE) == ENTRY_SIZE) {
            if (entry.sequence == 0) break;  // hit zero-padded region
            handler(entry);
        }
    }
};
```

### 8.4 Replay System for Backtesting

```cpp
// Full L3 market data replay at arbitrary speed
// Uses the same strategy engine code as production

class MarketReplay {
    // Memory-mapped tick file (binary, ~50GB/day for full US equities)
    MappedFile tick_file_;
    SimulatedClock clock_;
    LatencyModel latency_model_;    // Simulate exchange RTT

public:
    struct ReplayConfig {
        uint64_t start_ns;
        uint64_t end_ns;
        double speed_multiplier;     // 1.0 = real-time, 0.0 = as fast as possible
        bool simulate_fills;         // Use order-level fill simulation
    };

    void run(IStrategy& strategy, const ReplayConfig& config) {
        TickIterator it(tick_file_, config.start_ns);

        while (it.valid() && it.timestamp() <= config.end_ns) {
            clock_.set(it.timestamp());

            MarketUpdate update = it.decode();
            book_builder_.apply(update);

            Signal signal = strategy.on_book_update(
                update.instrument_id, book_builder_.book(update.instrument_id));

            if (signal.action != Action::NONE && config.simulate_fills) {
                auto fill = latency_model_.simulate_fill(signal, book_builder_);
                if (fill.has_value()) {
                    strategy.on_execution_report(fill.value());
                    position_tracker_.update(fill.value());
                }
            }

            ++it;
        }
    }
};
```

---

## 9. Fault Tolerance & Resilience

### 9.1 Failover Architecture

```
┌──────────────────────────────────┐     ┌──────────────────────────────────┐
│      PRIMARY SERVER               │     │      STANDBY SERVER              │
│                                    │     │                                  │
│  Feed Handler ──► Strategy ──► OMS│     │  Feed Handler ──► Strategy (shadow)
│       │                       │   │     │       │                          │
│       │         State Sync    │   │     │       │                          │
│       │ ◄─────────────────────┤   │     │       │                          │
│       │    (shared memory /   │   │     │       │                          │
│       │     RDMA heartbeat)   │   │     │       │                          │
│       ▼                       ▼   │     │       ▼                          │
│  [WAL Writer]            [TX NIC] │     │  [WAL Writer]        [TX NIC]   │
│                                    │     │  (TX disabled until promotion)  │
└──────────────────────────────────┘     └──────────────────────────────────┘

Failover trigger:
1. Heartbeat timeout (< 10ms)
2. Standby detects primary WAL staleness
3. Manual operator trigger
4. Exchange disconnect detected by standby

Failover sequence (< 50ms total):
1. Standby stops shadow mode, enables TX NIC
2. Reconnects to exchange (if needed) — uses pre-authenticated session
3. Reconciles position state from WAL + exchange drop-copy
4. Resumes quoting
```

> [!WARNING]
> **Position reconciliation is the hardest problem in HFT failover.** The bullet
> point above ("reconciles position state") is where 80% of production incidents
> occur. The following state machine makes this explicit and safe.

**Failover Reconciliation State Machine:**
```
STATE 1: DETECT  (< 10ms)
└── Standby detects heartbeat timeout from primary
└── OR: Primary NIC TX error count spikes
└── OR: Operator manual trigger

STATE 2: VERIFY  (< 50ms)
├── Send order status requests for all known open orders
├── Subscribe to drop-copy feed (if not already receiving)
├── Query exchange for current FIX session sequence number
└── Determine if primary TCP session is still alive at exchange
    └── If alive: exchange Cancel-on-Disconnect will NOT fire
    └── If dead: exchange has already cancelled all orders (CoD)

STATE 3: QUARANTINE  (< 100ms)  *** CRITICAL ***
├── Send MASS CANCEL to all exchanges for all instruments
│   └── Do NOT skip this even if you think CoD already fired
├── DO NOT send any new orders during this state
├── Wait for all cancel acknowledgments (or timeout at 200ms)
└── This eliminates the "in-flight order" problem:
    orders sent by primary but not yet acked are now cancelled

STATE 4: RECONCILE  (< 500ms)
├── Replay drop-copy fills received since last position snapshot
├── Reconstruct position from: WAL + drop-copy + cancel-acks
├── Compare against last known position snapshot
├── If discrepancy > threshold (e.g., > 100 shares per instrument):
│   ├── ALERT on-call team IMMEDIATELY (PagerDuty critical)
│   ├── Use exchange-reported position as source of truth
│   └── Log discrepancy details for post-incident analysis
└── Mark each instrument individually as RECONCILED or UNRECONCILED

STATE 5: RESUME
├── Only trade instruments marked RECONCILED
├── Start in REDUCED_SIZE degradation mode for 60 seconds
├── Monitor fill quality and P&L closely
└── Promote to NORMAL mode after confirmation period

SPLIT-BRAIN PREVENTION:
├── Primary and standby share a "fencing token" via shared memory / RDMA
├── Only the holder of the token can send orders
├── Token transfer is atomic (CAS on shared memory)
└── If both think they hold the token: HALT BOTH, manual intervention
```

### 9.2 Active-Active vs Active-Passive

| Approach | Pros | Cons | When to Use |
|---|---|---|---|
| **Active-Passive (Hot Standby)** | Simple, no split-brain risk | 10-50ms failover gap | Single-venue strategies |
| **Active-Active (Partitioned)** | Zero failover for healthy partitions | Complexity, position reconciliation | Multi-venue, multi-strategy |
| **Active-Active (Leader Election)** | Fast failover | Consensus overhead (~1ms) | Cross-venue arbitrage |

**Recommended**: Active-passive with hot standby for most strategies. Active-active only for multi-venue arbitrage where each server owns a venue partition.

### 9.3 Graceful Degradation

```cpp
// Degradation levels based on system health
enum class DegradationLevel {
    NORMAL,           // Full trading
    REDUCED_SIZE,     // Halve position sizes
    PASSIVE_ONLY,     // Only take liquidity, no quoting
    FLATTEN_ONLY,     // Only reduce positions
    HALT              // Kill switch — cancel all, no new orders
};

class HealthMonitor {
    DegradationLevel current_level_ = DegradationLevel::NORMAL;

    void evaluate() {
        auto metrics = collect_metrics();

        if (metrics.latency_p99 > 50'000)          // > 50μs p99
            degrade_to(DegradationLevel::REDUCED_SIZE);
        if (metrics.packet_loss_rate > 0.001)       // > 0.1% packet loss
            degrade_to(DegradationLevel::PASSIVE_ONLY);
        if (metrics.exchange_disconnect)
            degrade_to(DegradationLevel::FLATTEN_ONLY);
        if (metrics.pnl_drawdown > max_drawdown_)
            degrade_to(DegradationLevel::HALT);
    }
};
```

### 9.4 Exchange Throttling & Self-Rate-Limiting

> [!WARNING]
> Exchange rate limits (300-1000 orders/sec per port) are hard limits.
> Exceeding them results in **port disconnection** (all orders die),
> **punitive fines**, or **port suspension**. The pre-trade risk rate counter
> alone is insufficient — exchanges use sliding windows, not calendar seconds.

```cpp
// Token bucket rate limiter matching actual exchange behavior
class ExchangeThrottle {
    struct TokenBucket {
        int64_t tokens;
        int64_t max_tokens;         // burst limit
        int64_t refill_per_us;      // tokens added per microsecond
        uint64_t last_refill_tsc;

        [[gnu::hot]] bool try_consume(uint64_t now_tsc, int64_t cost = 1) noexcept {
            // Refill tokens based on elapsed time
            uint64_t elapsed_us = tsc_to_us(now_tsc - last_refill_tsc);
            tokens = std::min(max_tokens, tokens + int64_t(elapsed_us) * refill_per_us);
            last_refill_tsc = now_tsc;

            if (__builtin_expect(tokens >= cost, 1)) {
                tokens -= cost;
                return true;
            }
            return false;  // would exceed rate limit
        }
    };

    TokenBucket new_orders_;       // e.g., NASDAQ: 300 new orders/sec
    TokenBucket cancel_replace_;   // separate counter for cancels/replaces
    TokenBucket messages_total_;   // some exchanges have total message cap

public:
    // Configure per-exchange limits (loaded from venues.toml)
    void configure(int new_per_sec, int cancel_per_sec, int total_per_sec,
                   int burst_limit) {
        new_orders_ = {burst_limit, burst_limit,
                       new_per_sec / 1'000'000, rdtsc()};
        cancel_replace_ = {burst_limit, burst_limit,
                           cancel_per_sec / 1'000'000, rdtsc()};
        messages_total_ = {burst_limit * 2, burst_limit * 2,
                           total_per_sec / 1'000'000, rdtsc()};
    }

    [[gnu::hot, nodiscard]]
    bool allow_new_order() noexcept {
        auto now = rdtsc();
        return new_orders_.try_consume(now) && messages_total_.try_consume(now);
    }

    [[gnu::hot, nodiscard]]
    bool allow_cancel() noexcept {
        auto now = rdtsc();
        return cancel_replace_.try_consume(now) && messages_total_.try_consume(now);
    }
};
```

### 9.5 Disaster Recovery

| Scenario | RTO | RPO | Recovery Method |
|---|---|---|---|
| Server hardware failure | < 50ms | 0 (hot standby) | Auto-failover to standby |
| NIC failure | < 100ms | 0 | Bonded NIC failover |
| Exchange outage | N/A | N/A | Halt strategy, maintain positions |
| Data center power loss | < 5 min | Last WAL flush | DR site activation |
| Network partition | < 10ms | 0 | Flatten positions, halt |

---

## 10. Security

### 10.1 TLS vs Performance Tradeoffs

```
Exchange connections (order entry):
├── Most exchanges REQUIRE TLS 1.2+ for order entry
├── TLS handshake: ~1ms one-time cost (acceptable — done once at startup)
├── Per-message overhead: ~1-3μs (AES-NI hardware acceleration)
├── Use TLS 1.3 with 0-RTT resumption where supported
└── Pin certificates to prevent MITM

Market data (multicast):
├── Typically UNENCRYPTED (UDP multicast within exchange DC)
├── Secured by physical network isolation (dedicated VLANs)
└── No TLS overhead on the hottest path

Internal communication:
├── Within same server: shared memory (no encryption needed)
├── Between servers in same rack: RDMA or raw Ethernet (encrypted at NIC if needed)
└── To remote DC: WireGuard or IPsec (adds ~5-10μs)
```

### 10.2 Authentication

```cpp
// Exchange authentication — typically FIX Logon with credentials
// or certificate-based mutual TLS

// API keys / passwords stored encrypted, loaded once at startup
// Never in source code, never in config files on disk

class ExchangeAuth {
    // Credentials loaded from HSM or Vault at startup
    std::string username_;
    SecureString password_;     // zeroized on destruction
    X509* client_cert_;
    EVP_PKEY* private_key_;

    // Session token (some exchanges use HMAC-signed session tokens)
    std::array<uint8_t, 32> session_token_;
    uint64_t token_expiry_ns_;

    void authenticate() {
        // FIX Logon: 35=A, 553=Username, 554=Password
        // Some exchanges: HMAC-SHA256 signing of timestamp + sequence
        auto signature = hmac_sha256(
            private_key_, 
            format("{}|{}|{}", username_, timestamp_, seq_num_));
        send_logon(username_, signature);
    }
};

// SecureString: IMPROVED — fixed-size, non-copyable, non-movable
//
// WHY NOT std::string?
// 1. std::string uses Small String Optimization (SSO). For passwords < ~22 chars,
//    data lives inside the object itself. If SecureString is moved/copied, the old
//    location retains the password in stack/register memory that never gets wiped.
// 2. std::string may create copies during reserve()/operator= that are never wiped.
// 3. The compiler may optimize away the cleanse as a "dead store" (writing to memory
//    that's never read again). OPENSSL_cleanse avoids this, but std::string internals
//    may have already created uncontrolled copies.

class SecureString {
    static constexpr size_t MAX_LEN = 256;
    alignas(64) char data_[MAX_LEN];
    size_t len_ = 0;

    // Non-copyable, non-movable — prevent secret duplication
    SecureString(const SecureString&) = delete;
    SecureString& operator=(const SecureString&) = delete;
    SecureString(SecureString&&) = delete;
    SecureString& operator=(SecureString&&) = delete;

public:
    SecureString() { OPENSSL_cleanse(data_, MAX_LEN); }

    ~SecureString() {
        OPENSSL_cleanse(data_, MAX_LEN);  // wipe ENTIRE buffer, not just used portion
        len_ = 0;
    }

    void set(const char* src, size_t len) noexcept {
        assert(len < MAX_LEN);
        OPENSSL_cleanse(data_, MAX_LEN);  // wipe old data first
        std::memcpy(data_, src, len);
        data_[len] = '\0';
        len_ = len;
    }

    [[nodiscard]] std::string_view view() const noexcept { return {data_, len_}; }

    // Prevent this memory from being swapped to disk (where it could be recovered)
    void lock_memory() {
        mlock(data_, MAX_LEN);
    }
};
```

### 10.3 Threat Protection

```
┌──────────────────────────────────────────────────────────────┐
│                    SECURITY CONTROLS                         │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  Replay Attacks:                                             │
│  ├── Sequence numbers on all messages (reject duplicates)    │
│  ├── Timestamp validation (reject messages > 5s old)         │
│  └── HMAC signatures prevent forgery                         │
│                                                              │
│  Order Injection:                                            │
│  ├── Source IP whitelisting (exchange + management only)      │
│  ├── All orders pass pre-trade risk (can't bypass)           │
│  ├── Order rate limiting per session                         │
│  └── Anomaly detection on order patterns                     │
│                                                              │
│  Insider Threats:                                            │
│  ├── Role-based access control (RBAC) for config changes     │
│  ├── 4-eyes principle for risk limit changes                 │
│  ├── Full audit trail of all config/code changes             │
│  ├── Separate management network (out-of-band)               │
│  └── Code review required for strategy changes               │
│                                                              │
│  Infrastructure:                                             │
│  ├── No SSH keys on trading servers (BMC/IPMI access only)   │
│  ├── Immutable OS images (read-only root filesystem)         │
│  ├── Network segmentation (trading VLAN isolated)            │
│  └── Regular penetration testing                             │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 10.4 Secrets Management

```
Secrets Flow:
                  ┌─────────────┐
                  │  HashiCorp  │
                  │  Vault      │
                  │  (or AWS    │
                  │   Secrets   │
                  │   Manager)  │
                  └──────┬──────┘
                         │ mTLS
                         ▼
                  ┌──────────────┐
                  │  Startup     │
                  │  Script      │ ─── Fetches secrets, writes to tmpfs
                  └──────┬───────┘
                         │
                         ▼
                  ┌──────────────┐
                  │  Trading     │
                  │  Engine      │ ─── Reads from tmpfs, loads into SecureString
                  └──────────────┘     tmpfs file deleted after read

- Secrets never touch persistent storage
- Vault token is single-use, fetched via instance metadata
- Secrets rotation: automated, with zero-downtime handoff
- Emergency key revocation: Vault can invalidate all sessions
```

---

## 11. Performance Engineering

### 11.1 Latency Measurement

```cpp
// Hardware timestamps from NIC — most accurate measurement
// Solarflare: onload_timestamping_request()
// DPDK: rte_eth_timesync_read_rx_timestamp()

// Software timestamps: use RDTSC (calibrated) — ~20ns resolution
// AVOID: clock_gettime(CLOCK_MONOTONIC) — syscall overhead ~50-100ns up

// Measurement points (instrumented via RDTSC):
// T0: NIC RX hardware timestamp (packet arrival)
// T1: Feed handler decode complete
// T2: Order book update complete
// T3: Strategy signal generated
// T4: Pre-trade risk passed
// T5: Order encoded
// T6: NIC TX hardware timestamp (packet departure)
//
// Tick-to-trade = T6 - T0
// Per-component: T(n) - T(n-1)

// Use HdrHistogram for percentile tracking
// - O(1) record, O(1) percentile query
// - Configurable precision (3 significant figures)
// - Constant memory (~55KB for 1ns-10s range)

#include "hdr/hdr_histogram.h"

class LatencyProfiler {
    struct Component {
        const char* name;
        hdr_histogram* hist;
    };

    std::array<Component, 8> components_;

    struct TimestampChain {
        uint64_t stamps[8];
        int count = 0;

        void mark() { stamps[count++] = rdtsc(); }
    };

    // Per-core timestamp chains — no sharing
    thread_local static TimestampChain chain_;

public:
    void start() { chain_.count = 0; chain_.mark(); }
    void checkpoint() { chain_.mark(); }

    void finish() {
        chain_.mark();
        // Record per-component deltas
        for (int i = 1; i < chain_.count; ++i) {
            uint64_t delta_ns = tsc_to_ns(chain_.stamps[i] - chain_.stamps[i-1]);
            hdr_record_value(components_[i-1].hist, delta_ns);
        }
        // Record total
        uint64_t total = tsc_to_ns(chain_.stamps[chain_.count-1] - chain_.stamps[0]);
        hdr_record_value(total_hist_, total);
    }

    void report() {
        for (auto& c : components_) {
            printf("%-20s  p50=%6ldns  p99=%6ldns  p999=%6ldns\n",
                   c.name,
                   hdr_value_at_percentile(c.hist, 50.0),
                   hdr_value_at_percentile(c.hist, 99.0),
                   hdr_value_at_percentile(c.hist, 99.9));
        }
    }
};
```

### 11.2 Profiling Tools

| Tool | Purpose | When to Use |
|---|---|---|
| **perf stat** | PMU counters (cache misses, branch mispredicts) | Steady-state profiling |
| **perf record + flamegraph** | CPU sampling | Identifying hot functions |
| **Intel VTune** | Deep microarchitecture analysis | Cache analysis, port contention |
| **rdtsc instrumentation** | Per-component nanosecond timing | Always-on in production |
| **ftrace / trace-cmd** | Kernel event tracing | Debugging kernel interactions |
| **pcm (Intel PCM)** | Core/uncore counters, memory bandwidth | NUMA analysis |
| **cyclictest** | OS scheduling jitter measurement | System tuning validation |
| **mlc (Intel MLC)** | Memory latency characterization | NUMA topology profiling |

### 11.3 Benchmarking Strategy

```bash
# Micro-benchmark: per-component latency (run daily)
./bench_feed_handler --duration=60s --protocol=ITCH --output=csv

# Macro-benchmark: full tick-to-trade (run daily + before each deploy)
./bench_e2e --replay=data/2026-04-15.pcap --strategy=mm_v3 --output=histogram

# Regression detection: compare p99 against baseline
python3 scripts/latency_regression.py \
    --baseline=results/baseline.hdr \
    --current=results/current.hdr \
    --threshold-pct=10 \
    --fail-on-regression

# Sustained load test: 10M+ messages/sec for 1 hour
./bench_throughput --rate=10000000 --duration=3600s --report-interval=1s
```

### 11.4 Backpressure Handling

```cpp
// If the strategy engine can't keep up with market data:
// 1. Drop stale updates (keep only latest per instrument)
// 2. Never block the feed handler
// 3. Measure drop rate as a health metric

class BackpressurePolicy {
    std::atomic<uint64_t> drops_{0};
    std::atomic<uint64_t> processed_{0};

    // === DOUBLE-BUFFER CONFLATION (fixes torn-read bug) ===
    //
    // PROBLEM: Writing a large MarketUpdate struct non-atomically while
    // a consumer reads it concurrently causes torn reads — the consumer
    // sees half-old, half-new data (e.g., bid from tick N, ask from tick N+1).
    //
    // SOLUTION: Double-buffer with atomic index flip.
    // Producer writes to inactive buffer, then atomically increments sequence.
    // Consumer reads from the buffer indicated by the NEW sequence.
    // Since producer always writes to the OTHER buffer, reads are safe.
    //
    // Required invariant: exactly ONE producer and ONE consumer per instrument.

    struct alignas(128) ConflatedUpdate {
        MarketUpdate buffers[2];             // double-buffer
        std::atomic<uint64_t> sequence{0};   // low bit = active buffer index

        void publish(const MarketUpdate& update) noexcept {
            uint64_t seq = sequence.load(std::memory_order_relaxed);
            int write_idx = (seq & 1) ^ 1;   // write to INACTIVE buffer
            buffers[write_idx] = update;       // non-atomic, safe: consumer reads OTHER buffer
            sequence.store(seq + 1, std::memory_order_release);  // flip active index
        }

        bool consume(MarketUpdate& out, uint64_t& last_seq) noexcept {
            uint64_t seq = sequence.load(std::memory_order_acquire);
            if (seq == last_seq) return false;  // no new data
            int read_idx = seq & 1;             // read from NEWLY active buffer
            out = buffers[read_idx];             // safe: producer writes to OTHER buffer
            uint64_t skipped = seq - last_seq - 1;
            if (skipped > 0) drops_.fetch_add(skipped, std::memory_order_relaxed);
            last_seq = seq;
            return true;
        }

        // Note: drops_ referenced above would need to be passed in or be a member.
        // Shown inline for clarity.
        std::atomic<uint64_t> local_drops_{0};
    };

    std::array<ConflatedUpdate, 65536> latest_;  // indexed by InstrumentId

public:
    void publish(InstrumentId id, const MarketUpdate& update) noexcept {
        latest_[id].publish(update);
    }

    bool consume(InstrumentId id, MarketUpdate& out, uint64_t& last_seq) noexcept {
        auto& entry = latest_[id];
        uint64_t seq = entry.sequence.load(std::memory_order_acquire);
        if (seq == last_seq) return false;
        int read_idx = seq & 1;
        out = entry.buffers[read_idx];
        uint64_t skipped = seq - last_seq - 1;
        if (skipped > 0) drops_.fetch_add(skipped, std::memory_order_relaxed);
        last_seq = seq;
        ++processed_;
        return true;
    }

    double drop_rate() const noexcept {
        uint64_t d = drops_.load(std::memory_order_relaxed);
        uint64_t p = processed_.load(std::memory_order_relaxed);
        return (p + d) > 0 ? double(d) / double(p + d) : 0.0;
    }
};
```

---

## 12. CI/CD & Deployment

### 12.1 Build Pipeline

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│  Git     │───►│  Build   │───►│  Test    │───►│  Staging │───►│  Prod    │
│  Push    │    │  (Bazel) │    │  Suite   │    │  Deploy  │    │  Deploy  │
└──────────┘    └──────────┘    └──────────┘    └──────────┘    └──────────┘
                     │               │               │               │
               ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐
               │ Determin-  │  │ Unit      │  │ Paper     │  │ Canary    │
               │ istic      │  │ Integ     │  │ trading   │  │ (shadow   │
               │ Hermetic   │  │ Sim       │  │ replay    │  │  mode)    │
               │ Build      │  │ Latency   │  │ overnight │  │ then      │
               │ (toolchain │  │ Regression│  │           │  │ promote   │
               │  pinned)   │  │           │  │           │  │           │
               └────────────┘  └───────────┘  └───────────┘  └───────────┘
```

### 12.2 Deterministic Builds

```python
# BUILD system: Bazel (hermetic, reproducible)
# - Pinned toolchain (GCC 13.2, exact version)
# - Pinned libc version
# - All dependencies vendored or pinned by hash
# - Cross-compile from dev machines to prod matching kernel/glibc

# bazel build //trading:engine --config=prod
# Produces identical binary regardless of build machine

# Compiler flags for production:
# -O3 -march=native -mtune=native
# -flto=thin                    (link-time optimization)
# -fno-exceptions               (exceptions add ~5% overhead and jitter)
# -fno-rtti                     (RTTI unused on hot path)
# -fomit-frame-pointer          (one more register)
# -falign-functions=64          (cache line alignment)
# -falign-loops=64              (loop alignment)
# -funroll-loops                (reduce branch overhead)
# -DNDEBUG                      (disable asserts)
# Profile-guided optimization (PGO):
# 1. Build with -fprofile-generate
# 2. Run with production-like market replay
# 3. Rebuild with -fprofile-use
# Result: 5-15% latency improvement from better branch prediction & inlining
```

### 12.3 Canary Deployment

```
Canary deployment for trading systems:

Phase 1: Shadow Mode (1-3 days)
├── New binary runs alongside production
├── Receives same market data
├── Generates orders but does NOT send to exchange
├── Compares decisions against production system
├── Validates: latency, P&L simulation, risk behavior
└── Gate: p99 latency within 10% of baseline, no P&L anomalies

Phase 2: Reduced Size (1 day)
├── New binary sends real orders at 10% position size
├── Full monitoring and alerting active
├── Automatic rollback if any risk threshold breached
└── Gate: Fill quality matches expectations, no unexpected fills

Phase 3: Full Promotion
├── Swap primary to new binary
├── Old binary becomes standby
└── 15-minute monitoring window before decommissioning old binary
```

### 12.4 Rollback

```bash
# Binary rollback: < 30 seconds
# Both old and new binaries are on disk
# Rollback = restart process with old binary + restore config
# State is reconstructed from WAL + exchange drop-copy

# Zero-downtime: standby server promoted first, then primary rolls back
# Sequence:
# 1. Promote standby (running old binary) to primary
# 2. Standby takes over trading with no gap (hot standby)
# 3. Roll back primary server at leisure
# 4. Primary rejoins as standby once healthy
```

---

## 13. Monitoring & Observability

### 13.1 Key Metrics

```
Trading Metrics (real-time):
├── tick_to_trade_p99_ns          (< 10,000)
├── tick_to_trade_p999_ns         (< 25,000)
├── orders_sent_per_sec           (0-1000)
├── fill_rate_pct                 (expected: 5-30% for market making)
├── realized_pnl_usd              (rolling 1min, 5min, 1hr)
├── unrealized_pnl_usd            (mark-to-market)
├── position_net_shares            (per instrument)
├── position_gross_notional_usd    (total exposure)
├── spread_captured_bps            (maker edge)
├── adverse_selection_bps          (negative fill signal)

Infrastructure Metrics (real-time):
├── packet_loss_rate               (< 0.0001%)
├── feed_handler_latency_ns        (< 500)
├── book_update_latency_ns         (< 300)
├── cpu_utilization_pct            (per core — expect 100% on hot cores)
├── l1_cache_miss_rate             (< 1%)
├── context_switch_count           (should be 0 on isolated cores)
├── page_fault_count               (should be 0 after startup)
├── nic_rx_drops                   (should be 0)
├── nic_tx_errors                  (should be 0)
├── exchange_rtt_ns                (per venue)
├── wal_write_latency_us           (< 100)

System Health:
├── heartbeat_status               (binary: alive/dead)
├── clock_sync_offset_ns           (PTP: < 100ns)
├── memory_used_bytes              (should be stable)
├── disk_usage_pct                 (WAL rotation)
└── temperature_celsius            (CPU/NIC — throttling alert)
```

### 13.2 Monitoring Stack

```
Hot Path (nanosecond resolution):
├── RDTSC timestamps at each component boundary
├── Written to SPSC ring → consumed by telemetry thread
├── Aggregated into HdrHistograms per 1-second window
└── Published via shared memory to monitoring daemon

Near-Real-Time (millisecond resolution):
├── Prometheus / VictoriaMetrics for time-series storage
├── Custom exporter that reads from shared memory
├── Grafana dashboards with 1s refresh
└── PagerDuty for critical alerts

Trade Audit (complete, durable):
├── Every order, fill, cancel logged with nanosecond timestamps
├── Written to WAL and replicated to remote DC
├── Queryable via specialized GUI (internal tool)
└── Retained for 7+ years (SEC Rule 17a-4)
```

### 13.3 Alerting Strategy

```yaml
# Alert tiers:
critical:  # PagerDuty: wake up on-call
  - tick_to_trade_p99 > 50us for 10s
  - pnl_drawdown > $100K in 5min
  - exchange_disconnect any venue
  - kill_switch_activated
  - packet_loss > 0.1% for 5s
  - clock_sync_offset > 1ms

warning:   # Slack notification
  - tick_to_trade_p99 > 20us for 30s
  - fill_rate deviation > 2 stddev
  - position_near_limit (> 80% of max)
  - wal_write_latency > 1ms
  - context_switches > 0 on isolated core

info:      # Dashboard only
  - strategy_pnl crosses thresholds
  - exchange_maintenance_window approaching
  - certificate expiry < 30 days
```

### 13.4 Distributed Tracing

> [!NOTE]
> Traditional distributed tracing (Jaeger, Zipkin) adds microseconds of overhead and is **unacceptable on the hot path**. Instead, we use a lightweight approach:

```cpp
// Each event carries a monotonic sequence number and RDTSC timestamp
// Correlation is done offline by joining on sequence number across components
// No span creation, no context propagation — just timestamps and sequence IDs

struct TracePoint {
    uint64_t sequence_id;      // monotonic per trading day
    uint64_t tsc_timestamp;    // RDTSC value
    uint16_t component_id;     // FEED_HANDLER=0, BOOK_BUILDER=1, ...
    uint16_t event_type;       // ENTER=0, EXIT=1, CUSTOM=2+
    uint32_t extra;            // instrument_id or order_id, context-dependent
};

// Written to per-core ring buffer, drained by telemetry thread
// Offline analysis reconstructs the full trace per event chain
```

---

## 14. Testing Strategy

### 14.1 Test Pyramid

```
                    ┌────────────┐
                    │  E2E Sim   │   1-2 per strategy
                    │  (hours)   │   Full market replay + exchange sim
                    ├────────────┤
                 ┌──┤ Integration │   20-50 tests
                 │  │  (minutes) │   Component interaction, multi-thread
                 │  ├────────────┤
              ┌──┤  │   Unit     │   500+ tests
              │  │  │  (seconds) │   Pure logic, deterministic
              │  │  └────────────┘
              │  │
              │  │  ┌────────────┐
              │  └──┤ Latency    │   Run daily + pre-deploy
              │     │ Regression │   p99 comparison against baseline
              │     └────────────┘
              │
              │  ┌────────────┐
              └──┤  Chaos     │   Monthly
                 │  Tests     │   Hardware/network failure injection
                 └────────────┘
```

### 14.2 Unit Tests

```cpp
// Test framework: Google Test + Google Benchmark
// All strategy logic is tested with deterministic inputs

TEST(OrderBookTest, AddBidUpdatesTopOfBook) {
    OrderBook book;
    book.add_order(1, 'B', 10050, 100);  // bid at 100.50
    book.add_order(2, 'B', 10060, 200);  // bid at 100.60

    EXPECT_EQ(book.best_bid(), 10060);
    EXPECT_EQ(book.bids[0].quantity, 200);
    EXPECT_EQ(book.bid_depth, 2);
}

TEST(PreTradeRiskTest, RejectsPositionBreach) {
    PreTradeRisk risk;
    risk.set_limit(AAPL, {.max_position = 1000});

    // Fill up to limit
    EXPECT_EQ(risk.check(AAPL, Side::BUY, 1000, 15000, 15000), RiskResult::PASS);
    // Next order should breach
    EXPECT_EQ(risk.check(AAPL, Side::BUY, 1, 15000, 15000), RiskResult::POSITION_BREACH);
}

TEST(FixEncoderTest, EncodesValidMessage) {
    FixEncoder encoder("SENDER", "TARGET");
    NewOrderSingle order{.cl_ord_id = "ORD001", .symbol = "AAPL", 
                         .side = Side::BUY, .qty = 100, .price = 15000};
    auto msg = encoder.encode_new_order(order);
    EXPECT_TRUE(validate_fix_checksum(msg));
    EXPECT_TRUE(contains_tag(msg, 35, "D"));
    EXPECT_TRUE(contains_tag(msg, 55, "AAPL"));
}
```

### 14.3 Market Replay Testing

```cpp
// Replay historical market data through the strategy
// Compare decisions against recorded production decisions

class ReplayTest {
    void test_strategy_matches_production() {
        auto replay = MarketReplay("data/2026-04-15.bin");
        auto strategy = StrategyLoader::load("strategies/mm_v3.so");
        auto prod_log = ProductionLog::load("logs/2026-04-15.wal");

        replay.run(*strategy, {.simulate_fills = false});

        // Compare every decision
        auto sim_decisions = strategy->get_decision_log();
        auto prod_decisions = prod_log.get_decisions();

        for (size_t i = 0; i < sim_decisions.size(); ++i) {
            EXPECT_EQ(sim_decisions[i].action, prod_decisions[i].action)
                << "Divergence at event " << i 
                << " timestamp " << sim_decisions[i].timestamp_ns;
            EXPECT_EQ(sim_decisions[i].price, prod_decisions[i].price);
            EXPECT_EQ(sim_decisions[i].qty, prod_decisions[i].qty);
        }
    }
};
```

### 14.4 Latency Regression Tests

```bash
#!/bin/bash
# Run as part of CI — fails the build if latency regresses

BASELINE="benchmarks/baseline_$(git log -1 --format=%H HEAD~1).hdr"
CURRENT="benchmarks/current.hdr"

# Run benchmark with production-like replay
./bench_e2e --replay=data/benchmark_corpus.pcap \
            --strategy=mm_v3 \
            --warmup=10s \
            --duration=60s \
            --output=$CURRENT

# Compare p99 latency
python3 scripts/compare_latency.py \
    --baseline=$BASELINE \
    --current=$CURRENT \
    --metric=p99 \
    --max-regression-pct=10 \
    --max-regression-abs-ns=500

# Exit code 0 = pass, 1 = regression detected
```

### 14.5 Chaos Testing

```
Monthly chaos test scenarios:
1. NIC failure simulation        → Verify bonded NIC failover (< 100ms)
2. Exchange disconnect           → Verify kill switch activation
3. Feed handler crash            → Verify standby promotion
4. Clock desync injection        → Verify clock monitoring alerts
5. Memory pressure (cgroups)     → Verify graceful degradation
6. CPU throttling simulation     → Verify latency alerting fires
7. Packet loss injection (tc)    → Verify gap detection and recovery
8. Disk full simulation          → Verify WAL rotation continues
9. Kill -9 trading process       → Verify WAL recovery on restart
10. Rogue strategy (infinite loop) → Verify watchdog kills it
```

---

## 15. Advanced Topics

### 15.1 Co-Location Strategy

```
Optimal co-location topology:

Exchange DC (e.g., Equinix NY5):
├── Cabinet: 1/4 rack rental ($5K-15K/month)
├── Cross-connect: Direct fiber to exchange matching engine
│   ├── Physical distance: ideally < 10m of fiber
│   └── RTT: ~1-5μs (varies by exchange)
├── Power: Redundant A+B feeds, UPS, generator backup
├── Cooling: Liquid cooling for high-density compute
└── Multiple cabinets for different exchange cross-connects

Network optimization:
├── Shortest possible fiber runs (negotiate cabinet placement)
├── No switches in the path if possible (direct cross-connect)
├── PTP grandmaster clock in cabinet (GPS-disciplined)
│   └── Antenna on datacenter roof with coax run
├── NIC tuning: interrupt coalescing disabled, jumbo frames if supported
└── Switch ASIC tuning: cut-through mode (vs store-and-forward)

Inter-site connectivity (for cross-venue arb):
├── Chicago ↔ NJ: ~4.1ms microwave (vs 6.5ms fiber)
│   └── Providers: McKay Brothers, Anova Technologies
├── London ↔ Frankfurt: ~2.1ms microwave
├── Cost: $50K-200K/month per microwave link
└── Latency advantage: 2-3ms over fiber = massive edge for arb
```

### 15.2 FPGA-Based Trading Pipeline

```
Full FPGA pipeline (wire-to-wire):

                    ┌─────────────────────────────────────────┐
                    │              FPGA (Alveo U55C)          │
                    │                                         │
NIC ──10GbE──►     │  MAC ──► UDP ──► ITCH   ──► Signal ──► │
              │     │  Parse   Parse   Decode     Logic      │ ──►  NIC ──10GbE──► Exchange
              │     │  (2 clk) (3 clk) (5 clk)   (10 clk)   │
              │     │                                         │
              │     │  ──► Order ──► FIX/SBE ──► UDP ──► MAC │
              │     │     Build     Encode      Build   Build│
              │     │     (3 clk)  (5 clk)     (3 clk) (2clk)│
              │     │                                         │
              │     │  Total pipeline: ~33 clock cycles       │
              │     │  @ 300MHz = ~110ns wire-to-wire         │
              │     │                                         │
              │     │  ┌─────────┐                            │
              │     │  │ PCIe    │ Config from CPU            │
              │     │  │ Bridge  │ (strategy params,          │
              │     │  │         │  risk limits, symbols)     │
              │     │  └─────────┘                            │
              │     └─────────────────────────────────────────┘
              │
              └──► CPU (software path for complex strategies,
                   monitoring, logging)

FPGA development stack:
├── HDL: SystemVerilog (most teams), Chisel (functional hardware)
├── Simulation: Vivado xsim / Verilator
├── Synthesis: Vivado for Xilinx, Quartus for Intel
├── Testing: cocotb (Python-based verification)
├── Build time: 2-8 hours for full synthesis (vs seconds for software)
└── Strategy change cycle: days-weeks (vs hours for software)
```

### 15.3 AI/ML in HFT

> [!IMPORTANT]
> AI/ML is used in HFT, but almost never on the hot path. Inference latency for even small neural networks (~10-100μs on GPU, ~5-50μs on CPU with ONNX) is too high for sub-5μs systems.

| Use Case | Latency Tolerance | Technology | Location |
|---|---|---|---|
| **Alpha signal generation** | Offline (minutes-hours) | XGBoost, LightGBM, deep learning | Research cluster |
| **Parameter optimization** | Offline (overnight) | Bayesian optimization, genetic algorithms | Cloud |
| **Feature engineering** | Near-real-time (seconds) | Streaming feature stores | Separate server |
| **Regime detection** | Near-real-time (100ms) | HMM, online clustering | Co-lo, off-path |
| **Execution quality prediction** | Pre-trade (~1ms allowed) | Small gradient-boosted model | CPU, off-hot-path |
| **Anomaly detection** | Post-trade | Autoencoders, isolation forest | Remote DC |

```cpp
// Example: ML-derived signal used as strategy parameter
// Model runs offline, publishes parameters to shared memory
// Strategy reads parameters with zero overhead

struct MLSignalParams {
    double spread_multiplier;      // How wide to quote (from ML model)
    double inventory_bias;         // Skew based on regime detection
    double adverse_selection_adj;  // Tighten/widen based on toxicity model
    uint64_t update_timestamp_ns;
};

// ML inference server writes to shared memory every N seconds
// Strategy hot loop reads atomically — no syscalls, no IPC overhead
auto* params = static_cast<const MLSignalParams*>(
    mmap(nullptr, sizeof(MLSignalParams), PROT_READ, MAP_SHARED, shm_fd, 0));

// In strategy:
double compute_spread() {
    return base_spread_ * params->spread_multiplier + params->adverse_selection_adj;
}
```

### 15.4 Regulatory Compliance

```
Regulatory Requirements by Jurisdiction:

United States (SEC/FINRA):
├── Rule 15c3-5: Pre-trade risk controls (mandatory)
│   ├── Capital thresholds
│   ├── Credit limits per counterparty
│   └── Erroneous order prevention
├── Reg NMS: Best execution obligation
│   └── Must route to venue with best price
├── Rule 17a-4: Record retention (7 years)
│   └── All orders, modifications, cancellations, fills
├── Consolidated Audit Trail (CAT):
│   └── Report all orders with microsecond timestamps
├── Market Access Rule:
│   └── Broker-dealer must have risk controls on sponsored access
└── Regulation SCI: Systems compliance (if registered)

European Union (ESMA/MiFID II):
├── RTS 25: Clock synchronization ≤ 100μs (gateway level)
│   └── PTP or GPS required, documented
├── RTS 6: Algorithmic trading requirements
│   ├── Kill switches mandatory
│   ├── Pre-trade risk checks mandatory
│   └── Strategy testing documentation
├── Transaction reporting (EMIR, MiFIR)
│   └── T+1 reporting to ARM/APA
├── Algo tagging: Each algo must have unique ID
│   └── Tag in FIX field 9999 (exchange-specific)
└── Market making obligations (if registered MM)
    └── Minimum quoting time, max spread, min size

Implementation checklist:
☑ All orders timestamped with nanosecond precision
☑ Complete audit trail persisted and immutable
☑ Kill switch tested monthly and documented
☑ Pre-trade risk checks cannot be bypassed
☑ Clock synchronization monitored and alerted
☑ Annual algo risk assessment documented
☑ Change management process for strategy changes
☑ Disaster recovery tested quarterly
```

---

## Appendix A: Technology Stack Summary

| Component | Technology | Rationale |
|---|---|---|
| **Language (hot path)** | C++20/23 | Lowest latency, full hardware control |
| **Language (analytics)** | Python 3.11+ | NumPy/Pandas ecosystem, rapid prototyping |
| **Build system** | Bazel | Hermetic, reproducible, fast incremental |
| **Kernel bypass** | Solarflare OpenOnload / ef_vi | Lowest NIC latency, production-proven |
| **DPDK** | DPDK 23.x | Multi-vendor NIC support |
| **Serialization** | SBE (hot path), FlatBuffers (warm), Protobuf (cold) | Zero-copy → schema evolution → flexibility |
| **Time-series DB** | QuestDB or ClickHouse | Columnar, fast ingestion, SQL interface |
| **RDBMS** | PostgreSQL 16 | Trade blotter, config, compliance |
| **Monitoring** | Prometheus + Grafana | Industry standard, extensible |
| **Alerting** | PagerDuty + Slack | Tiered alerting |
| **CI/CD** | Bazel + custom scripts | Deterministic builds, latency regression gates |
| **Secrets** | HashiCorp Vault | Dynamic secrets, audit trail |
| **Container** | None (bare metal) | Containers add ~1-3μs overhead |
| **OS** | Ubuntu 22.04 LTS (PREEMPT_RT kernel) | Stable, well-supported, low-latency patches |
| **Clock sync** | PTP (IEEE 1588) + GPS | Sub-100ns accuracy required by regulation |
| **FPGA** | Xilinx Alveo U55C / Intel Agilex | Sub-microsecond strategies |
| **Hash map** | robin_hood::unordered_flat_map | Flat, cache-friendly, fast |
| **Ring buffer** | Custom SPSC (shown above) | Zero-contention inter-thread comm |
| **Histogram** | HdrHistogram | O(1) percentile queries, constant memory |

---

## Appendix B: Latency Budget Breakdown

> [!WARNING]
> **Latency budgets must be based on measurements from target hardware, not datasheets.**
> The numbers below reflect realistic measurements on Intel Xeon Sapphire Rapids
> with Solarflare X2 NIC, isolated cores, and a market-making strategy with
> inventory management. Your mileage will vary — always benchmark on YOUR hardware.

```
Total tick-to-trade budget: 5,000ns (5μs) p50 target

┌────────────────────────────────┬──────────┬──────────┬──────────┐
│ Component                      │ p50 (ns) │ p99 (ns) │p999 (ns) │
├────────────────────────────────┼──────────┼──────────┼──────────┤
│ NIC RX → userspace (ef_vi)    │      280 │      500 │    1,200 │
│ Feed decode + normalize        │      200 │      350 │      800 │
│ Order book update (L3, hash)   │      350 │      700 │    1,500 │
│ SPSC ring hop (×1, fused arch) │       40 │       70 │      150 │
│ Strategy signal evaluation     │    2,000 │    5,000 │   12,000 │
│ Pre-trade risk (inlined)       │      120 │      200 │      400 │
│ OMS + routing + FIX/SBE encode │      400 │      700 │    1,200 │
│ NIC TX → wire (ef_vi)         │      200 │      400 │      800 │
├────────────────────────────────┼──────────┼──────────┼──────────┤
│ SUBTOTAL (measured)            │    3,590 │    7,920 │   18,050 │
│ 20% engineering margin         │      718 │    1,584 │    3,610 │
│ TOTAL (budgeted)               │    4,308 │    9,504 │   21,660 │
│ vs. target                     │  ✅ <5μs │  ✅<10μs │  ✅<25μs │
└────────────────────────────────┴──────────┴──────────┴──────────┘

Key observations:
- Strategy evaluation dominates the budget (55% of p50). Optimizing the
  strategy is the highest-leverage improvement.
- The fused core architecture (Section 3.3) reduces ring hops from 4 to 1,
  saving ~120-320ns vs. the original 6-core pipeline.
- The 20% margin absorbs sporadic L2/L3 cache misses, TLB misses, and
  timer interrupts on imperfectly-isolated cores.
- p999 target (25μs) allows for rare but unavoidable events: NUMA cross-
  node access, SMI interrupts, NIC coalescing delays.
- To hit <2μs (FPGA territory), the strategy must be trivial (<200ns)
  and the software path must be eliminated entirely.
```

---

## Appendix C: Directory Structure

```
hft-system/
├── BUILD                           # Bazel root build file
├── WORKSPACE                       # Bazel workspace config
├── .bazelrc                        # Compiler flags, configs
├── toolchain/                      # Pinned GCC, glibc, sysroot
│
├── core/                           # Shared low-latency primitives
│   ├── spsc_ring.h                 # Lock-free SPSC ring buffer
│   ├── clock.h                     # RDTSC clock, calibration
│   ├── allocator.h                 # Pool allocator, hugepage allocator
│   ├── hash_map.h                  # robin_hood wrapper
│   └── types.h                     # Price, Qty, InstrumentId, Side
│
├── feed/                           # Market data pipeline
│   ├── feed_handler.cpp            # DPDK/ef_vi packet receive loop
│   ├── itch_decoder.h              # NASDAQ ITCH 5.0 parser
│   ├── pitch_decoder.h             # CBOE PITCH parser
│   ├── mdp3_decoder.h              # CME MDP 3.0 parser
│   ├── normalizer.h                # Unified MarketUpdate format
│   ├── gap_detector.h              # Sequence gap detection
│   └── order_book.h                # L2/L3 order book
│
├── strategy/                       # Strategy engine
│   ├── engine.cpp                  # Main strategy loop (busy-poll)
│   ├── strategy_interface.h        # IStrategy plugin interface
│   ├── strategy_loader.cpp         # dlopen-based plugin loading
│   └── strategies/
│       ├── market_maker_v3.cpp     # Market making strategy
│       ├── stat_arb_pairs.cpp      # Statistical arbitrage
│       └── index_arb.cpp           # ETF/index arbitrage
│
├── oms/                            # Order management
│   ├── order_manager.cpp           # Order lifecycle tracking
│   ├── smart_router.cpp            # Venue selection
│   └── order_types.h               # NewOrder, Cancel, Replace
│
├── gateway/                        # Exchange connectivity
│   ├── fix_encoder.h               # Zero-alloc FIX encoder
│   ├── fix_decoder.h               # FIX message parser
│   ├── sbe_encoder.h               # SBE encoder (CME, etc.)
│   ├── connection_manager.cpp      # TCP/TLS session management
│   └── adapters/
│       ├── nasdaq_ouch.cpp         # NASDAQ OUCH adapter
│       ├── cme_ilink3.cpp          # CME iLink 3 adapter
│       └── cboe_boe.cpp            # CBOE BOE adapter
│
├── risk/                           # Risk management
│   ├── pre_trade_risk.h            # Inline pre-trade checks
│   ├── post_trade_risk.cpp         # Aggregated risk monitoring
│   ├── kill_switch.h               # Emergency halt
│   └── position_manager.cpp        # Position tracking
│
├── persistence/                    # Storage
│   ├── wal_writer.cpp              # Write-ahead log
│   ├── wal_reader.cpp              # WAL replay for recovery
│   └── market_data_recorder.cpp    # Tick data capture
│
├── telemetry/                      # Monitoring
│   ├── latency_profiler.h          # RDTSC-based profiling
│   ├── metrics_publisher.cpp       # Prometheus exporter
│   └── trade_logger.cpp            # Audit trail
│
├── config/                         # Configuration
│   ├── instruments.toml            # Symbol universe, tick sizes
│   ├── risk_limits.toml            # Position/notional limits
│   ├── venues.toml                 # Exchange connection params
│   └── strategies.toml             # Strategy parameters
│
├── scripts/                        # Operational scripts
│   ├── deploy.sh                   # Deployment automation
│   ├── latency_regression.py       # CI latency comparison
│   ├── pnl_report.py               # Daily P&L report
│   └── tune_system.sh              # OS/BIOS tuning script
│
├── tests/                          # Test suite
│   ├── unit/                       # Google Test unit tests
│   ├── integration/                # Multi-component tests
│   ├── simulation/                 # Full system simulation
│   ├── replay/                     # Market replay tests
│   └── benchmarks/                 # Google Benchmark latency tests
│
└── docs/                           # Documentation
    ├── architecture.md             # This document
    ├── runbook.md                  # Operational procedures
    ├── incident_response.md        # Incident playbook
    └── regulatory_compliance.md    # Compliance documentation
```

---

> [!TIP]
> **Key Engineering Tradeoffs to Remember**:
> 1. **Latency vs. Flexibility**: FPGA = lowest latency but weeks to change; C++ = slightly higher latency but hours to change
> 2. **Throughput vs. Latency**: Batching improves throughput but kills latency; always optimize for latency first
> 3. **Safety vs. Speed**: Pre-trade risk is mandatory and cannot be optimized away; design it to be O(1) and branchless
> 4. **Determinism vs. Performance**: Disable Turbo Boost, C-states, and hyperthreading — you lose peak throughput but gain predictable latency
> 5. **Complexity vs. Reliability**: Every line of code on the hot path is a potential bug; minimize hot path complexity ruthlessly

---

## 16. Design Review & Errata

> [!CAUTION]
> **April 2026 Addendum**: A subsequent principal-level engineering review of this architecture document identified several critical gaps and aspirational assumptions that have since been rectified in the text above. 

Substantial design updates incorporated in the latest revision:

1. **Latency Budgets**: Hardware-validated latency numbers have replaced the original aspirational targets. While the goal remains <5μs, achieving this required reducing internal queues (SPSC ring hops).
2. **Fused Architecture**: The pipeline was flattened to eliminate cross-core cache migrations. The `Feed Handler` and `Order Book Builder` now run synchronously on the same isolated core, as does the `Strategy`, `Risk`, and `Gateway` chain.
3. **Data Structure Safety**: 
   - `robin_hood::unordered_flat_map` was removed from the L3 book (rehashing is fatal to latency bounds). A pre-allocated Fibonacci-hashed pool now handles order tracking.
   - The Backpressure conflator was redesigned with a double-buffered atomic index to prevent torn reads.
4. **Resilience & Risk**:
   - The original pre-trade risk implementation contained a fatal multi-threading race condition when multiple strategies traded the same instrument. It now uses a two-tier lock-free mechanism.
   - The WAL writer was transitioned to `io_uring` to prevent data loss on process crash.
   - The failover plan was expanded with a rigorous 5-state reconciliation machine, acknowledging that simple drop-copy replays are insufficient without strict quarantining.

*Future engineers should prioritize measuring absolute latencies on target production hardware rather than optimizing cycles on unmeasured microbenchmarks.*
