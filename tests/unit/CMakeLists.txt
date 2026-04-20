cmake_minimum_required(VERSION 3.20)

add_executable(hft_tests
    test_clock.cpp
    test_spsc_ring.cpp
    test_order_book.cpp
    test_feed_handler.cpp
    test_strategy.cpp
    test_risk.cpp
    test_oms.cpp
    test_gateway.cpp
    # Phase 1: Core Infrastructure
    test_tsc_calibration.cpp
    test_latency_profiler.cpp
    test_connection_manager.cpp
    test_post_trade_risk.cpp
    test_backpressure.cpp
    # Phase 2: Platform Features
    test_platform.cpp
    test_wal_writer.cpp
    test_replay.cpp
    # Phase 3: Strategies, Decoders, BBO, Loader
    test_stat_arb.cpp
    test_index_arb.cpp
    test_pitch_decoder.cpp
    test_mdp3_decoder.cpp
    test_bbo_snapshot.cpp
    # Phase 4: Security
    test_secure_string.cpp
    test_exchange_auth.cpp
    test_fix_decoder.cpp
    # Phase 5: Monitoring & Observability
    test_health_monitor.cpp
    test_metrics_publisher.cpp
    test_trade_logger.cpp
    # Phase 6: Failover & Resilience
    test_failover.cpp
    test_position_manager.cpp
    # Phase 7: FPGA Interface
    test_fpga_interface.cpp
    # Phase 8: ML Signal Integration
    test_ml_signal.cpp
    # Phase 9: Gateway Adapters
    test_adapters.cpp
    # Tick capture for offline replay
    test_market_data_recorder.cpp
)

target_link_libraries(hft_tests
    PRIVATE
    core_lib
    gtest_main
)

include(GoogleTest)
gtest_discover_tests(hft_tests)
