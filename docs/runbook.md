# HFT Platform — Runbook

This runbook covers day-to-day operation of the trading platform in production.
Incident handling lives in `incident_response.md`; regulatory obligations live
in `regulatory_compliance.md`.

## 1. Daily lifecycle

### 1.1 Pre-open (T − 60 min)

1. **Time sync** — confirm PTP/chrony offset < 100 µs:
   ```
   chronyc tracking | head -n5
   ```
2. **Host tuning** — `sudo ./scripts/tune_system.sh --check` should report:
   - governor = `performance`
   - SMT = `off`
   - hugepages > 0
   - isolated CPUs match the trading topology
3. **Market-data feeds** — snapshot channels subscribed, gap counters at 0.
4. **Exchange sessions** — FIX / OUCH / iLink logons acknowledged. Verify the
   last-received sequence number matches the drop copy.
5. **Risk limits** — kill switch armed (`/metrics` → `kill_switch_active 0`).

### 1.2 Open

- Strategies marked `PASSIVE` until market calms (configurable; default
  disengage at open + 30 s).
- Post-trade risk aggregator running on dedicated CPU; verify P&L heartbeat
  in the telemetry dashboard.

### 1.3 Close

1. Cancel all resting orders.
2. Snapshot positions: `PositionManager::snapshot()` → persist to WAL.
3. Drain OMS / gateway sessions gracefully (send logout).
4. Roll trade logs: `scripts/pnl_report.py log.bin --summary`.

## 2. Common operational tasks

### 2.1 Deploy a new build

```
./scripts/deploy.sh <host>
```

The script refuses to deploy if `ctest` fails locally. Always deploy to a
staging host first, let it run against the replay driver for ≥ 15 min, then
promote.

### 2.2 Roll strategy parameters

Strategy params are hot-reloadable via `MLSignalWriter`. The ML server writes
to a shared-memory page and the trading strategy picks up the new values on
the next tick without a restart.

### 2.3 Rotate credentials

`SecureString` wipes on destruction. To rotate:
1. Bring up the new credential set in the config.
2. `SIGUSR1` → triggers the gateway to re-logon using the new creds.
3. Confirm successful logon, then wipe the old keys from the config store.

### 2.4 Failover

Manual failover (secondary → primary):

```
# On secondary
curl -X POST http://localhost:9100/admin/force_failover
```

The failover manager transitions `ACTIVE_SECONDARY → DETECT → VERIFY →
QUARANTINE → RECONCILE → RESUME → ACTIVE_PRIMARY`. Reconciliation drains the
WAL and drop-copy before RESUME; total expected duration: < 2 s.

## 3. Monitoring

- Prometheus exposition: `http://<host>:9100/metrics`
- Key alerts:
  - `latency_p99_ns > 10ms` for ≥ 30 s → page
  - `exchange_disconnect == 1` → page
  - `risk_reject_rate > 1%` for ≥ 60 s → page
  - `health_degradation_level >= DEGRADED` → page

## 4. Logs and observability

| Artifact | Location | Retention |
|---|---|---|
| WAL | `/var/hft/wal/YYYYMMDD.wal` | 30 d local, 7 y S3 |
| Trade log | `/var/hft/trades/YYYYMMDD.tl` | 7 y S3 |
| Latency histogram | `/var/hft/lat/YYYYMMDD.json` | 90 d |
| Stdout/stderr | journald | 14 d |

## 5. Recovery checkpoints

See `incident_response.md` for failure-mode-specific playbooks. Every incident
ends with:

1. WAL + drop-copy reconciliation.
2. Position and P&L verification against the custody statement.
3. Postmortem filed within 24 h.
