#include <gtest/gtest.h>
#include "fpga/fpga_interface.h"

using namespace hft::fpga;

TEST(FPGABridgeTest, EnableDisable) {
    MMIOBackend bar;
    FPGABridge br(bar);
    EXPECT_FALSE(br.is_enabled());
    br.enable();
    EXPECT_TRUE(br.is_enabled());
    br.disable();
    EXPECT_FALSE(br.is_enabled());
}

TEST(FPGABridgeTest, WriteConfigStoresParams) {
    MMIOBackend bar;
    FPGABridge br(bar);

    FPGAConfig cfg{};
    cfg.params.fair_value = 10000;
    cfg.params.spread_half = 50;
    cfg.params.max_order_qty = 1000;
    cfg.params.throttle_cycles = 250;
    cfg.limits.max_position_qty = 100000;
    cfg.limits.max_notional = 1'000'000'000;
    cfg.limits.max_orders_per_sec = 500;
    ASSERT_TRUE(br.write_config(cfg));

    // Readback from BAR
    EXPECT_EQ(bar.read64(regs::CONFIG_BASE + 0x00), 10000U);
    EXPECT_EQ(bar.read64(regs::CONFIG_BASE + 0x08), 50U);
    EXPECT_EQ(bar.read32(regs::CONFIG_BASE + 0x18), 250U);
    EXPECT_EQ(bar.read32(regs::CONFIG_BASE + 0x38), 500U);
}

TEST(FPGABridgeTest, SymbolTable) {
    MMIOBackend bar;
    FPGABridge br(bar);
    FPGAConfig cfg{};
    cfg.symbols.push_back({1, 1001, 10000});
    cfg.symbols.push_back({2, 1002, 20000});
    ASSERT_TRUE(br.write_config(cfg));
    EXPECT_EQ(bar.read32(regs::SYMTAB_BASE + 0), 1U);
    EXPECT_EQ(bar.read32(regs::SYMTAB_BASE + 4), 1001U);
    EXPECT_EQ(bar.read64(regs::SYMTAB_BASE + 8), 10000U);
    EXPECT_EQ(bar.read32(regs::SYMTAB_BASE + 16), 2U);
}

TEST(FPGABridgeTest, StatusReadout) {
    MMIOBackend bar;
    FPGABridge br(bar);
    bar.simulate_status(status_flags::PIPELINE_ACTIVE | status_flags::LINK_UP | status_flags::PLL_LOCKED);
    bar.simulate_counters(1000, 950, 42);

    auto s = br.read_status();
    EXPECT_TRUE(s.pipeline_active);
    EXPECT_TRUE(s.link_up);
    EXPECT_TRUE(s.pll_locked);
    EXPECT_FALSE(s.buffer_full);
    EXPECT_EQ(s.packets_in, 1000U);
    EXPECT_EQ(s.packets_out, 950U);
    EXPECT_EQ(s.orders_sent, 42U);
}

TEST(FPGABridgeTest, ConfigureStage) {
    MMIOBackend bar;
    FPGABridge br(bar);
    struct SigCfg { uint32_t threshold; uint32_t weight; };
    SigCfg c{42, 7};
    ASSERT_TRUE(br.configure_stage(FPGAStage::SIGNAL_EVAL, &c, sizeof(c)));

    size_t off = regs::STAGE_BASE + static_cast<size_t>(FPGAStage::SIGNAL_EVAL) * regs::STAGE_STRIDE;
    EXPECT_EQ(bar.read32(off), 42U);
    EXPECT_EQ(bar.read32(off + 4), 7U);
}

TEST(FPGAPipelineTest, DeployEnablesBridge) {
    MMIOBackend bar;
    FPGABridge br(bar);
    FPGAPipeline pipe(br);

    FPGAConfig cfg{};
    cfg.params.fair_value = 500;
    ASSERT_TRUE(pipe.deploy(cfg));
    EXPECT_TRUE(br.is_enabled());
}

TEST(FPGAPipelineTest, ConfigureTypedStage) {
    MMIOBackend bar;
    FPGABridge br(bar);
    FPGAPipeline pipe(br);

    struct RiskStageCfg { int64_t pos_max; int64_t notional_max; };
    RiskStageCfg rc{10000, 1'000'000};
    ASSERT_TRUE(pipe.configure_stage(FPGAStage::RISK_CHECK, rc));
    size_t off = regs::STAGE_BASE + static_cast<size_t>(FPGAStage::RISK_CHECK) * regs::STAGE_STRIDE;
    EXPECT_EQ(bar.read64(off), 10000U);
    EXPECT_EQ(bar.read64(off + 8), 1'000'000U);
}
