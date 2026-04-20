# Metrics Catalog

Canonical metric names exported by `telemetry/metrics_publisher.h`. These names
are referenced by `monitoring/prometheus_alerts.yml` and the Grafana dashboards
under `monitoring/grafana_dashboards/`.

All metrics are prefixed `hft_`. Labels are lowercase snake_case. Values are
base units (nanoseconds, not microseconds; USD with 4 decimal places encoded
as integer `*10000` for counters, plain `double` for gauges).

## Latency (gauges, updated per 1s window by telemetry thread)

| Metric | Unit | Labels | Source |
|---|---|---|---|
| `hft_tick_to_trade_p50_ns` | ns | `instance` | `LatencyProfiler::percentile(0.50)` |
| `hft_tick_to_trade_p99_ns` | ns | `instance` | same |
| `hft_tick_to_trade_p999_ns` | ns | `instance` | same |
| `hft_feed_handler_latency_ns` | ns | `venue` | per-component chain |
| `hft_book_update_latency_ns` | ns | `instance` | per-component chain |
| `hft_wal_write_latency_us` | µs | `instance` | WAL flush path |

## Throughput (counters)

| Metric | Labels |
|---|---|
| `hft_feed_packets_total` | `venue` |
| `hft_feed_packet_loss_total` | `venue` |
| `hft_orders_sent_total` | `venue`, `strategy` |
| `hft_fills_total` | `venue`, `strategy` |
| `hft_cancels_total` | `venue`, `strategy` |

## PnL (gauges)

| Metric | Unit | Labels |
|---|---|---|
| `hft_pnl_realized_usd` | USD | `strategy` |
| `hft_pnl_unrealized_usd` | USD | `strategy` |
| `hft_strategy_pnl_usd` | USD | `strategy` |
| `hft_fill_rate_pct` | % | `strategy` |

## Position (gauges)

| Metric | Unit | Labels |
|---|---|---|
| `hft_position_net` | shares/contracts | `instrument` |
| `hft_position_limit` | same | `instrument` |
| `hft_position_gross_notional_usd` | USD | `instance` |

## Venue health

| Metric | Type | Labels |
|---|---|---|
| `hft_exchange_connected` | 0/1 | `venue` |
| `hft_exchange_rtt_ns` | gauge | `venue` |
| `hft_exchange_maintenance_ts` | gauge (unix) | `venue` |

## System health

| Metric | Type | Labels |
|---|---|---|
| `hft_kill_switch_active` | 0/1 | `instance`, `reason` |
| `hft_clock_sync_offset_ns` | gauge | `instance` |
| `hft_context_switches_total` | counter | `instance`, `core`, `core_kind` |
| `hft_certificate_expiry_ts` | gauge (unix) | `cert` |

## Registration

Metric handles are created at startup via
`MetricsPublisher::register_metric(name, help, type)`. Emitting an unregistered
name returns a sentinel handle and is a no-op — this is intentional so missing
wiring does not crash the hot path, but it means alerts will silently stop
firing if a metric name is dropped. Keep this catalog and the exporter in sync.
