# Regulatory Compliance Checklist

This document maps the platform's controls to the regulations that govern
algorithmic and high-frequency trading in the jurisdictions we operate in.
Ownership, evidence, and review cadence are captured per control.

## 1. Regulatory scope

| Jurisdiction | Primary regime | Key obligations |
|---|---|---|
| US (equities) | SEC Rule 15c3-5 (Market Access), Reg NMS, Reg SCI | Pre-trade risk checks, audit trail, capacity & resiliency |
| US (equities) | FINRA Rule 3110, 4511 | Supervision + recordkeeping (7 years) |
| US (futures) | CFTC Reg AT (withdrawn but best-practice) | Pre-trade risk, source-code repository |
| EU | MiFID II RTS 6 | Algo testing, kill functionality, audit trail, governance |
| EU | MiFID II RTS 25 | Clock sync to UTC within 100 µs |

## 2. Control matrix

### 2.1 Pre-trade risk (SEC 15c3-5 / MiFID II RTS 6)

| Control | Implementation | Evidence |
|---|---|---|
| Fat-finger price check | `risk/pre_trade_risk.h` — `PRICE_BREACH` | Unit test: PriceBreach |
| Max order size | `PreTradeRisk::check()` → `SIZE_BREACH` | Unit test |
| Max position | `PositionManager` + `PostTradeRisk` | Integration test |
| Notional cap | `PostTradeRisk::calculate_portfolio_delta` | Integration test |
| Message rate throttle | `ConnectionManager` backpressure + `MessageRateLimiter` | Backpressure test |
| Kill switch | `risk/kill_switch.h` + `HealthMonitor::CRITICAL` path | End-to-end test |

### 2.2 Audit trail & recordkeeping (FINRA 4511, MiFID II)

| Control | Implementation | Retention |
|---|---|---|
| Every order, fill, cancel logged with ns timestamp | `telemetry/trade_logger.h` → `TradeEvent` | 7 years |
| Write-ahead log of every state transition | `persistence/wal_writer.h` | 7 years |
| Order decisions reconstructable from inputs | `persistence/replay.h` + feed replay | 7 years |
| Source code under version control | Git + tagged releases | 7 years |

### 2.3 Clock synchronization (MiFID II RTS 25)

| Control | Implementation | Evidence |
|---|---|---|
| UTC traceable time source | PTP grandmaster → NIC HW timestamp | `chronyc tracking` logs |
| Host-local drift compensation | `core/clock.h` → `TSCCalibration::recalibrate()` | Calibration test |
| Divergence alerting | `HealthMonitor` + Prometheus alert | Alert rule |

### 2.4 Governance & algorithm testing (MiFID II RTS 6)

| Control | Implementation |
|---|---|
| Pre-deployment testing | CI: unit tests (251+) + replay regression + latency regression |
| Non-production environment | Dedicated replay harness using `MarketReplay` with historical tick files |
| Conformance test | Exchange-provided certification suite replayed through the gateway |
| Change management | PR review + two-person sign-off; deploy via `scripts/deploy.sh` |
| Annual self-assessment | Documented per RTS 6; reviewed by Compliance |

### 2.5 Resilience (Reg SCI, MiFID II)

| Control | Implementation |
|---|---|
| Primary/secondary failover | `resilience/failover_manager.h` with fencing token |
| Heartbeat monitoring | `HeartbeatMonitor` — 10 ms timeout |
| Disaster recovery drill | Quarterly; documented in `runbook.md` §2.4 |
| Capacity testing | Benchmarks run against 2× expected message rate |

### 2.6 Security (SEC 15c3-5, internal)

| Control | Implementation |
|---|---|
| Credentials at rest | `security/secure_string.h` — wiped on destruction, lockable |
| Exchange authentication | `security/exchange_auth.h` — FIX Logon with signed token |
| Audit of privileged actions | systemd journal + separate audit log |
| Network segregation | Trading VLAN, mgmt VLAN, out-of-band access |

## 3. Obligations we do **not** yet fulfil

- **CAT reporting** (US equities): downstream integration pending.
- **MiFID II transaction reporting** (RTS 22): sent via the broker of record;
  we retain the raw trade log and reconcile.
- **CFTC Reg AT source-code repository** (currently withdrawn): we maintain
  the equivalent via tagged source releases + build reproducibility.

## 4. Review cadence

- Daily: kill-switch arm/disarm, risk-limit snapshot, clock offset check.
- Weekly: drop-copy reconciliation report.
- Monthly: incident postmortem review; retention audit of WAL + trade logs.
- Quarterly: DR drill; penetration test of management surfaces.
- Annually: RTS 6 self-assessment; algorithm inventory and owner list.

## 5. Evidence locations

| Artifact | Path |
|---|---|
| Unit test run (latest CI) | `ci://runs/<date>/` |
| Latency baseline histograms | `s3://hft-lat/<date>.json` |
| Daily P&L reconciliation | `s3://hft-pnl/<date>.csv` (via `pnl_report.py`) |
| Trade log (raw) | `s3://hft-audit/<date>.tl` |
| WAL (raw) | `s3://hft-wal/<date>.wal` |
| Incident postmortems | `docs/postmortems/<incident-id>.md` |
