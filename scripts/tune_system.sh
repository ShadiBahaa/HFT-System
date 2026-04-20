#!/usr/bin/env bash
# =============================================================================
# tune_system.sh — one-shot host tuning for low-latency trading
#
# Idempotent: re-run after each reboot. Assumes a Linux host; see
# docs/runbook.md for Windows/IDE-based tuning.
#
# Call with --check to print current settings without modifying.
# =============================================================================
set -euo pipefail

CHECK_ONLY=${1:-}

need_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "error: must run as root (or via sudo)" >&2
        exit 1
    fi
}

log() { printf '[tune] %s\n' "$*"; }

# CPUs to isolate for trading threads (update to match your deploy topology).
ISOLATED_CPUS="${ISOLATED_CPUS:-2-7}"
NUMA_NODE="${NUMA_NODE:-0}"
HUGEPAGES="${HUGEPAGES:-128}"

set_governor() {
    local gov=$1
    for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [[ -w "$c" ]] && echo "$gov" > "$c"
    done
    log "cpu governor -> $gov"
}

disable_smt() {
    if [[ -w /sys/devices/system/cpu/smt/control ]]; then
        echo off > /sys/devices/system/cpu/smt/control
        log "SMT disabled"
    fi
}

set_hugepages() {
    echo "$HUGEPAGES" > /proc/sys/vm/nr_hugepages
    log "hugepages -> $HUGEPAGES (2MB each)"
}

disable_cstates() {
    # Force C0/C1 only; deep sleep destroys latency
    for c in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do
        [[ -w "$c" ]] && echo 1 > "$c" || true
    done
    log "deep C-states disabled"
}

tune_nic() {
    local nic=${NIC:-eth0}
    [[ -d /sys/class/net/$nic ]] || { log "nic $nic not found — skip"; return; }
    ethtool -C "$nic" rx-usecs 0 tx-usecs 0 adaptive-rx off adaptive-tx off || true
    ethtool -G "$nic" rx 4096 tx 4096 || true
    ethtool -K "$nic" gro off lro off tso off gso off || true
    log "nic $nic: interrupt coalescing off, large offloads off"
}

disable_irqbalance() {
    systemctl stop irqbalance 2>/dev/null || true
    systemctl disable irqbalance 2>/dev/null || true
    log "irqbalance stopped"
}

pin_irqs_away_from_trading_cpus() {
    # Route every IRQ to CPU 0 so our trading cores never see interrupts
    for irq in /proc/irq/[0-9]*/smp_affinity; do
        echo 1 > "$irq" 2>/dev/null || true
    done
    log "IRQs pinned to CPU 0"
}

print_status() {
    log "=== current system tuning ==="
    log "isolated cpus: $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo 'none')"
    log "governor:      $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    log "smt:           $(cat /sys/devices/system/cpu/smt/active 2>/dev/null)"
    log "hugepages:     $(cat /proc/sys/vm/nr_hugepages)"
    log "numa nodes:    $(numactl --hardware 2>/dev/null | head -n3 || echo 'numactl missing')"
}

if [[ "$CHECK_ONLY" == "--check" ]]; then
    print_status
    exit 0
fi

need_root

log "Applying low-latency tuning..."
set_governor performance
disable_smt
set_hugepages
disable_cstates
tune_nic
disable_irqbalance
pin_irqs_away_from_trading_cpus

log "-----------------------------------------------"
log "Reboot required for grub changes (isolcpus, nohz_full)."
log "Suggested /etc/default/grub append:"
log "  isolcpus=$ISOLATED_CPUS nohz_full=$ISOLATED_CPUS rcu_nocbs=$ISOLATED_CPUS"
log "  intel_pstate=disable processor.max_cstate=1 idle=poll"
log "  transparent_hugepage=never audit=0 mitigations=off"
log "-----------------------------------------------"
print_status
