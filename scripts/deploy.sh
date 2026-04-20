#!/usr/bin/env bash
# =============================================================================
# deploy.sh — deploy a built trading binary to a target host
#
# Pipeline:
#   1. Sanity-check local build artifacts
#   2. Copy binaries + config to target via rsync
#   3. Pre-flight checks on target (tuning, NIC, NTP)
#   4. Systemd unit restart under a maintenance window
#
# Intentionally a smoke-test scaffold: production deploys go through the
# change-management system referenced in docs/runbook.md.
# =============================================================================
set -euo pipefail

TARGET=${1:-}
BUILD_DIR=${BUILD_DIR:-build}
CONFIG_DIR=${CONFIG_DIR:-config}
REMOTE_USER=${REMOTE_USER:-trading}
REMOTE_PATH=${REMOTE_PATH:-/opt/hft}
UNIT_NAME=${UNIT_NAME:-hft-trader.service}

die() { echo "error: $*" >&2; exit 1; }
log() { printf '[deploy] %s\n' "$*"; }

[[ -n "$TARGET" ]] || die "usage: deploy.sh <target-host>"

log "target: $TARGET"
log "build:  $BUILD_DIR"

# 1. Local sanity
[[ -d "$BUILD_DIR" ]] || die "build dir $BUILD_DIR missing"
[[ -d "$CONFIG_DIR" ]] || die "config dir $CONFIG_DIR missing"

log "running unit tests before deploy..."
(cd "$BUILD_DIR" && ctest --output-on-failure) || die "tests failed — refusing to deploy"

# 2. Sync
log "syncing binaries..."
rsync -az --delete "$BUILD_DIR"/tests/unit/hft_tests \
    "$REMOTE_USER@$TARGET:$REMOTE_PATH/bin/" || die "rsync (bin) failed"

log "syncing configs..."
rsync -az --delete "$CONFIG_DIR/" \
    "$REMOTE_USER@$TARGET:$REMOTE_PATH/config/" || die "rsync (config) failed"

# 3. Pre-flight
log "running pre-flight checks..."
ssh "$REMOTE_USER@$TARGET" bash -se <<'REMOTE'
    set -e
    if ! command -v chronyc >/dev/null; then
        echo "warn: chronyc missing — time sync not verified"
    else
        chronyc tracking | head -n5
    fi
    if [[ -r /sys/devices/system/cpu/isolated ]]; then
        echo "isolated cpus: $(cat /sys/devices/system/cpu/isolated)"
    fi
    free -h | head -n2
REMOTE

# 4. Restart under maintenance window
log "requesting restart of $UNIT_NAME"
ssh "$REMOTE_USER@$TARGET" "sudo systemctl daemon-reload && sudo systemctl restart $UNIT_NAME"

# 5. Smoke
log "waiting 5s for startup..."
sleep 5
ssh "$REMOTE_USER@$TARGET" "systemctl status $UNIT_NAME --no-pager | head -n10"

log "done."
