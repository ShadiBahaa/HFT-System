#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include "core/types.h"

namespace hft::fpga {

    using namespace hft::core;

    // =========================================================================
    // FPGA Interface — host-side abstraction for a wire-to-wire pipeline
    // implemented on an FPGA NIC/card. In real hardware this would sit on top
    // of a PCIe BAR mapped via mmap()/MapViewOfFile; here we expose a portable
    // "mock MMIO" backing store that exercises the same code paths.
    //
    // Pipeline stages (from hft_system_design.md Figure 4):
    //   0: MAC_RX        — L2 packet reception
    //   1: UDP_PARSE     — UDP header extraction
    //   2: FEED_DECODE   — exchange-specific wire decode
    //   3: SIGNAL_EVAL   — minimal strategy evaluation
    //   4: RISK_CHECK    — pre-trade risk gate
    //   5: ORDER_BUILD   — order encoding
    //   6: MAC_TX        — L2 packet emission
    // =========================================================================

    enum class FPGAStage : uint8_t {
        MAC_RX         = 0,
        UDP_PARSE      = 1,
        FEED_DECODE    = 2,
        SIGNAL_EVAL    = 3,
        RISK_CHECK     = 4,
        ORDER_BUILD    = 5,
        MAC_TX         = 6,
        NUM_STAGES     = 7
    };

    // Register map (offsets in bytes within the BAR)
    namespace regs {
        constexpr size_t CONTROL          = 0x0000;   // bit0=enable, bit1=reset
        constexpr size_t STATUS           = 0x0008;   // bitfield: see FPGAStatusFlags
        constexpr size_t ERROR_FLAGS      = 0x0010;
        constexpr size_t PACKETS_IN       = 0x0018;
        constexpr size_t PACKETS_OUT      = 0x0020;
        constexpr size_t ORDERS_SENT      = 0x0028;
        constexpr size_t RISK_REJECTS     = 0x0030;
        constexpr size_t CYCLES_WIRE_RX   = 0x0038;
        constexpr size_t CYCLES_WIRE_TX   = 0x0040;
        constexpr size_t CONFIG_BASE      = 0x0100;   // Start of strategy config
        constexpr size_t SYMTAB_BASE      = 0x1000;   // Symbol table
        constexpr size_t STAGE_BASE       = 0x2000;   // Per-stage config area
        constexpr size_t STAGE_STRIDE     = 0x100;
        constexpr size_t BAR_SIZE         = 0x10000;  // 64KB BAR
    }

    namespace status_flags {
        constexpr uint64_t PIPELINE_ACTIVE = 1ULL << 0;
        constexpr uint64_t LINK_UP         = 1ULL << 1;
        constexpr uint64_t PLL_LOCKED      = 1ULL << 2;
        constexpr uint64_t BUFFER_FULL     = 1ULL << 3;
        constexpr uint64_t CHECKSUM_ERR    = 1ULL << 4;
    }

    // -------------------------------------------------------------------------
    // FPGAConfig — strategy parameters pushed to the FPGA
    // -------------------------------------------------------------------------
    struct FPGAStrategyParams {
        Price       fair_value{0};
        Price       spread_half{0};
        Quantity    max_order_qty{0};
        uint32_t    throttle_cycles{0};
    };

    struct FPGARiskLimits {
        int64_t     max_position_qty{0};
        int64_t     max_notional{0};
        Price       max_price_band{0};
        uint32_t    max_orders_per_sec{0};
    };

    struct FPGASymbol {
        InstrumentId    id{0};
        uint32_t        exchange_sym_id{0};
        Price           ref_price{0};
    };

    struct FPGAConfig {
        FPGAStrategyParams  params{};
        FPGARiskLimits      limits{};
        std::vector<FPGASymbol> symbols;
    };

    struct FPGAStatus {
        bool        pipeline_active{false};
        bool        link_up{false};
        bool        pll_locked{false};
        bool        buffer_full{false};
        bool        checksum_error{false};
        uint64_t    packets_in{0};
        uint64_t    packets_out{0};
        uint64_t    orders_sent{0};
        uint64_t    risk_rejects{0};
        uint32_t    wire_rx_cycles{0};
        uint32_t    wire_tx_cycles{0};
    };

    // -------------------------------------------------------------------------
    // MMIOBackend — abstracts the PCIe BAR. On real hardware this wraps the
    // mmap'd mapping; here we keep a backing array so tests can exercise the
    // register protocol without hardware.
    // -------------------------------------------------------------------------
    class MMIOBackend {
    public:
        MMIOBackend() { std::memset(bar_.data(), 0, bar_.size()); }

        void write32(size_t off, uint32_t v) noexcept {
            if (off + 4 > bar_.size()) return;
            std::memcpy(bar_.data() + off, &v, 4);
        }
        void write64(size_t off, uint64_t v) noexcept {
            if (off + 8 > bar_.size()) return;
            std::memcpy(bar_.data() + off, &v, 8);
        }
        [[nodiscard]] uint32_t read32(size_t off) const noexcept {
            if (off + 4 > bar_.size()) return 0;
            uint32_t v;
            std::memcpy(&v, bar_.data() + off, 4);
            return v;
        }
        [[nodiscard]] uint64_t read64(size_t off) const noexcept {
            if (off + 8 > bar_.size()) return 0;
            uint64_t v;
            std::memcpy(&v, bar_.data() + off, 8);
            return v;
        }
        void write_block(size_t off, const void* src, size_t n) noexcept {
            if (off + n > bar_.size()) return;
            std::memcpy(bar_.data() + off, src, n);
        }
        void read_block(size_t off, void* dst, size_t n) const noexcept {
            if (off + n > bar_.size()) return;
            std::memcpy(dst, bar_.data() + off, n);
        }

        // Test helpers: simulate FPGA-side state updates
        void simulate_status(uint64_t flags) noexcept { write64(regs::STATUS, flags); }
        void simulate_counters(uint64_t pkt_in, uint64_t pkt_out, uint64_t ord) noexcept {
            write64(regs::PACKETS_IN, pkt_in);
            write64(regs::PACKETS_OUT, pkt_out);
            write64(regs::ORDERS_SENT, ord);
        }

    private:
        std::array<uint8_t, regs::BAR_SIZE> bar_{};
    };

    // -------------------------------------------------------------------------
    // FPGABridge — host-side driver
    // -------------------------------------------------------------------------
    class FPGABridge {
    public:
        explicit FPGABridge(MMIOBackend& bar) : bar_(bar) {}

        void enable() noexcept {
            uint32_t ctrl = bar_.read32(regs::CONTROL);
            bar_.write32(regs::CONTROL, ctrl | 0x1);
        }

        void disable() noexcept {
            uint32_t ctrl = bar_.read32(regs::CONTROL);
            bar_.write32(regs::CONTROL, ctrl & ~0x1u);
        }

        void reset_pipeline() noexcept {
            uint32_t ctrl = bar_.read32(regs::CONTROL);
            bar_.write32(regs::CONTROL, ctrl | 0x2);
            // In real HW we'd wait for the reset bit to clear; here we clear immediately
            bar_.write32(regs::CONTROL, ctrl & ~0x2u);
        }

        [[nodiscard]] bool is_enabled() const noexcept {
            return (bar_.read32(regs::CONTROL) & 0x1) != 0;
        }

        // Push configuration to FPGA registers
        bool write_config(const FPGAConfig& cfg) noexcept {
            // Strategy params
            bar_.write64(regs::CONFIG_BASE + 0x00, static_cast<uint64_t>(cfg.params.fair_value));
            bar_.write64(regs::CONFIG_BASE + 0x08, static_cast<uint64_t>(cfg.params.spread_half));
            bar_.write64(regs::CONFIG_BASE + 0x10, static_cast<uint64_t>(cfg.params.max_order_qty));
            bar_.write32(regs::CONFIG_BASE + 0x18, cfg.params.throttle_cycles);

            // Risk limits
            bar_.write64(regs::CONFIG_BASE + 0x20, static_cast<uint64_t>(cfg.limits.max_position_qty));
            bar_.write64(regs::CONFIG_BASE + 0x28, static_cast<uint64_t>(cfg.limits.max_notional));
            bar_.write64(regs::CONFIG_BASE + 0x30, static_cast<uint64_t>(cfg.limits.max_price_band));
            bar_.write32(regs::CONFIG_BASE + 0x38, cfg.limits.max_orders_per_sec);

            // Symbol table — stored packed
            constexpr size_t SYM_STRIDE = 16;
            if (cfg.symbols.size() * SYM_STRIDE > (regs::STAGE_BASE - regs::SYMTAB_BASE)) {
                return false;  // Too many symbols
            }
            for (size_t i = 0; i < cfg.symbols.size(); ++i) {
                size_t off = regs::SYMTAB_BASE + i * SYM_STRIDE;
                bar_.write32(off + 0, cfg.symbols[i].id);
                bar_.write32(off + 4, cfg.symbols[i].exchange_sym_id);
                bar_.write64(off + 8, static_cast<uint64_t>(cfg.symbols[i].ref_price));
            }
            return true;
        }

        // Configure a single pipeline stage (opaque 256 bytes of per-stage config)
        bool configure_stage(FPGAStage stage, const void* cfg, size_t len) noexcept {
            if (len > regs::STAGE_STRIDE) return false;
            size_t off = regs::STAGE_BASE + static_cast<size_t>(stage) * regs::STAGE_STRIDE;
            bar_.write_block(off, cfg, len);
            return true;
        }

        // Read pipeline status
        [[nodiscard]] FPGAStatus read_status() const noexcept {
            FPGAStatus s;
            uint64_t flags = bar_.read64(regs::STATUS);
            s.pipeline_active = (flags & status_flags::PIPELINE_ACTIVE) != 0;
            s.link_up         = (flags & status_flags::LINK_UP) != 0;
            s.pll_locked      = (flags & status_flags::PLL_LOCKED) != 0;
            s.buffer_full     = (flags & status_flags::BUFFER_FULL) != 0;
            s.checksum_error  = (flags & status_flags::CHECKSUM_ERR) != 0;
            s.packets_in      = bar_.read64(regs::PACKETS_IN);
            s.packets_out     = bar_.read64(regs::PACKETS_OUT);
            s.orders_sent     = bar_.read64(regs::ORDERS_SENT);
            s.risk_rejects    = bar_.read64(regs::RISK_REJECTS);
            s.wire_rx_cycles  = bar_.read32(regs::CYCLES_WIRE_RX);
            s.wire_tx_cycles  = bar_.read32(regs::CYCLES_WIRE_TX);
            return s;
        }

    private:
        MMIOBackend& bar_;
    };

    // -------------------------------------------------------------------------
    // FPGAPipeline — high-level controller orchestrating stage configuration
    // -------------------------------------------------------------------------
    class FPGAPipeline {
    public:
        explicit FPGAPipeline(FPGABridge& bridge) : bridge_(bridge) {}

        bool deploy(const FPGAConfig& cfg) noexcept {
            bridge_.disable();
            bridge_.reset_pipeline();
            if (!bridge_.write_config(cfg)) return false;
            bridge_.enable();
            return true;
        }

        template <typename StageCfg>
        bool configure_stage(FPGAStage stage, const StageCfg& cfg) noexcept {
            return bridge_.configure_stage(stage, &cfg, sizeof(StageCfg));
        }

        [[nodiscard]] FPGAStatus status() const noexcept {
            return bridge_.read_status();
        }

    private:
        FPGABridge& bridge_;
    };

} // namespace hft::fpga
