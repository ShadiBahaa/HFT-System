# Incident Response Playbook

Short, actionable playbooks for the most common failure modes. Each section is
a state machine: **Detect → Contain → Diagnose → Recover → Postmortem**.

## 1. Severity classification

| Level | Definition | Response time | Who |
|---|---|---|---|
| SEV1 | Runaway orders, outages, position/audit mismatch | < 5 min | On-call + trading lead |
| SEV2 | Latency or P&L degradation outside normal | < 30 min | On-call |
| SEV3 | Non-halting anomalies (occasional reject, warning logs) | Next business day | On-call |

All SEV1 incidents require the kill switch engaged and positions flattened
before investigation begins.

## 2. Playbooks

### 2.1 Runaway strategy / exploding order rate

**Detect**: `orders_sent_rate > 5 * baseline` OR `risk_rejects_rate > 1%`.

**Contain**:
1. `POST /admin/kill_switch` — immediately blocks new orders.
2. Send cancel-all: `gateway_cli cancel-all --venue=ALL`.
3. Drop drop-copy session connectivity only after fills confirmed.

**Diagnose**:
- Inspect the latency profiler for where the loop tightened.
- Walk backward through the trade log: which strategy emitted the first
  anomalous batch?
- Check ML signal shared memory — stuck timestamp? stale params?

**Recover**: disable the offending strategy in config, redeploy, re-arm risk,
verify normal behavior on replay before enabling live.

### 2.2 Exchange connection lost

**Detect**: `exchange_disconnect == 1` for > 5 s, OR FIX heartbeat miss.

**Contain**:
1. Reconnect is automatic (ConnectionManager retry with backoff 100/500/2000ms).
2. After 3 failed retries, ConnectionManager escalates to the kill switch.

**Diagnose**:
- Network: `tcpdump` on the NIC, confirm no packets.
- Exchange side: check the exchange's status page / OCG.
- Our side: was there a host tuning regression (see `scripts/tune_system.sh --check`)?

**Recover**:
1. Once reconnected, verify sequence numbers. If a gap exists, send Resend
   Request; otherwise normal flow resumes.
2. Reconcile positions against drop copy.

### 2.3 Position / audit mismatch

**Detect**: `PositionManager::total_gross_notional()` diverges from drop-copy
totals by > 1 contract.

**Contain**:
1. Kill switch on.
2. Freeze WAL writes (pause the OMS thread).

**Diagnose**:
- Replay the WAL via `persistence::MarketReplay` and recompute positions.
- Diff against the drop-copy ledger.
- Look for `UNKNOWN` side values or dropped messages in feed handler logs.

**Recover**:
- If the mismatch is our bug: correct in-memory positions from drop-copy
  (authoritative) and file a SEV1 postmortem.
- If the mismatch is an exchange bug: open an ECN ticket with the drop-copy
  snapshot attached; do **not** adjust local positions until acknowledged.

### 2.4 Latency regression

**Detect**: `latency_p99_ns` breaches budget for > 30 s.

**Contain**: automatic → strategies move to passive on DEGRADED.

**Diagnose**:
- `scripts/latency_regression.py baseline.json current.json --threshold 0.10`
- Check for background noise: TLB shootdowns, IRQ migration, GC in other
  processes. `perf sched record` for a few seconds.
- TSC calibration drift? `TSCCalibration::recalibrate()` logs drift in
  `telemetry/latency_profiler.h`.

**Recover**: disable the latest change, rerun baseline. File a postmortem if
the cause is non-obvious.

### 2.5 Split-brain during failover

**Detect**: both primary and secondary report `ACTIVE_PRIMARY`.

**Contain**: the fencing token CAS in `FailoverManager::FencingToken` prevents
this at the ledger level. Verify `FencingToken::current()` values; the higher
token wins.

**Diagnose**:
- Was one host partitioned long enough for the other to fence? Check
  heartbeat history.

**Recover**: demote the stale primary to `ACTIVE_SECONDARY`. Reconcile any
orders it emitted against drop copy; they should all be rejected due to stale
fencing token.

## 3. Kill switch decision tree

```
        Anomaly detected
               │
               ▼
     ┌──────────────────┐
     │ Positions safe?  │─── No ──► Kill switch on. Flatten via IOC.
     └─────────┬────────┘
               │ Yes
               ▼
     ┌──────────────────┐
     │ Latency > 2× p99?│─── Yes ──► Passive mode, monitor.
     └─────────┬────────┘
               │ No
               ▼
     Degraded mode (strategies tighten risk)
```

## 4. Postmortem template

1. **Summary** (one paragraph)
2. **Timeline** (UTC, second precision)
3. **Root cause**
4. **What went well**
5. **What went wrong**
6. **Action items** (owner + due date)
