#!/usr/bin/env bash
# =============================================================================
# rollback.sh — revert a live engine to the previous binary.
#
# Implements the zero-downtime sequence from hft_system_design.md §12.4:
#
#   1. Promote STANDBY (already running previous binary) to primary.
#      Standby takes over trading with no gap (hot standby).
#   2. Roll back PRIMARY server at leisure: swap symlink, restart unit.
#   3. Primary rejoins as standby once healthy.
#
# Binary layout on each host (convention, enforced by deploy.sh):
#
#   /opt/hft/releases/<version>/hft_engine     # immutable per-version dir
#   /opt/hft/current   → symlink → releases/<active>
#   /opt/hft/previous  → symlink → releases/<previous>
#
# Target: rollback completes in < 30s per host. Whole cluster failover
# (primary+standby swap) should complete in < 5s — the 30s budget is for
# the un-hurried recovery of the now-standby former-primary.
#
# Usage:
#   rollback.sh --primary <host> --standby <host>
#   rollback.sh --primary <host> --standby <host> --reason "p99 regression"
# =============================================================================
set -euo pipefail

PRIMARY=""
STANDBY=""
REASON="manual"
REMOTE_USER=${REMOTE_USER:-trading}
REMOTE_PATH=${REMOTE_PATH:-/opt/hft}
UNIT_NAME=${UNIT_NAME:-hft-trader.service}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --primary)  PRIMARY="$2";  shift 2;;
        --standby)  STANDBY="$2";  shift 2;;
        --reason)   REASON="$2";   shift 2;;
        -h|--help)
            sed -n '2,/^# ====/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

die() { echo "[rollback] ERROR: $*" >&2; exit 1; }
log() { printf '[rollback] %s\n' "$*"; }

[[ -n "$PRIMARY" ]] || die "missing --primary"
[[ -n "$STANDBY" ]] || die "missing --standby"

log "START  primary=$PRIMARY  standby=$STANDBY  reason='$REASON'"
START_TS=$(date -u +%s)

# -----------------------------------------------------------------------------
# Step 0: verify standby is healthy BEFORE we touch anything.
# Refuse the rollback if the standby isn't ready — it's the whole point.
# -----------------------------------------------------------------------------
log "[0/4] verifying standby health on $STANDBY"
if ! ssh "$REMOTE_USER@$STANDBY" "systemctl is-active $UNIT_NAME" >/dev/null; then
    die "standby $STANDBY is not active — ABORTING rollback"
fi
ssh "$REMOTE_USER@$STANDBY" "systemctl show $UNIT_NAME --property=ActiveEnterTimestamp,MainPID --no-pager"

# -----------------------------------------------------------------------------
# Step 1: promote standby. The engine watches a role file and auto-arms
# the gateway when promoted. Writing the file is cheap; the effect is
# observed by the engine's role-watcher thread within one poll cycle (~10ms).
# -----------------------------------------------------------------------------
log "[1/4] promoting $STANDBY → PRIMARY"
ssh "$REMOTE_USER@$STANDBY" "echo primary | sudo tee $REMOTE_PATH/role >/dev/null"

# Fencing: bump the shared token so the old primary refuses to send orders
# even if it's briefly alive at the same time. Token lives in a mmap'd
# file that both hosts read under a lease from the failover manager.
ssh "$REMOTE_USER@$STANDBY" "sudo $REMOTE_PATH/bin/hft_fence bump" || \
    log "warn: fence bump failed (manual intervention may be needed)"

# -----------------------------------------------------------------------------
# Step 2: demote former primary. We do NOT stop it — it stays up as standby
# so its position table is usable for reconciliation in step 3.
# -----------------------------------------------------------------------------
log "[2/4] demoting $PRIMARY → STANDBY"
ssh "$REMOTE_USER@$PRIMARY" "echo standby | sudo tee $REMOTE_PATH/role >/dev/null"

# -----------------------------------------------------------------------------
# Step 3: swap the current→previous symlinks on the demoted host, then
# restart the unit to pick up the old binary.
# -----------------------------------------------------------------------------
log "[3/4] rolling back binary on $PRIMARY"
ssh "$REMOTE_USER@$PRIMARY" bash -se <<REMOTE
    set -euo pipefail
    cd $REMOTE_PATH
    if [[ ! -L previous ]]; then
        echo "no 'previous' symlink — cannot roll back" >&2
        exit 3
    fi
    # Atomic swap: new-prev → old-current, new-current → old-previous
    old_current=\$(readlink current)
    old_previous=\$(readlink previous)
    ln -sfn "\$old_current"  current.new  # scratch
    ln -sfn "\$old_previous" current
    ln -sfn "\$old_current"  previous
    rm -f current.new
    echo "current  -> \$(readlink current)"
    echo "previous -> \$(readlink previous)"
    sudo systemctl daemon-reload
    sudo systemctl restart $UNIT_NAME
REMOTE

# -----------------------------------------------------------------------------
# Step 4: wait for the rolled-back host to come back as healthy standby.
# -----------------------------------------------------------------------------
log "[4/4] waiting for $PRIMARY to rejoin as standby"
for i in $(seq 1 30); do
    if ssh "$REMOTE_USER@$PRIMARY" "systemctl is-active $UNIT_NAME" >/dev/null 2>&1; then
        log "rolled-back host active after ${i}s"
        break
    fi
    sleep 1
done

# Record the rollback for audit.
log "writing audit record"
ssh "$REMOTE_USER@$STANDBY" bash -se <<REMOTE
    sudo mkdir -p $REMOTE_PATH/audit
    ts=\$(date -u +%Y%m%dT%H%M%SZ)
    echo "\$ts rollback primary=$PRIMARY standby=$STANDBY reason='$REASON' operator=\$USER" \
        | sudo tee -a $REMOTE_PATH/audit/rollback.log >/dev/null
REMOTE

END_TS=$(date -u +%s)
log "DONE in $((END_TS - START_TS))s"
log "post-rollback: monitor dashboards for 15 minutes before declaring stable"
