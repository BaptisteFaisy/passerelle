#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# guest_probe.sh - ask the QNX 7.1 guest what it sees of our virtio-9p device.
#
# This is the automated form of the manual procedure recorded in
# tests/qemu/qnx71/EVIDENCE.md: the same commands, run over the same SSH channel
# (tests/qemu/qnx71/qnx-ssh), with every answer saved as text so the CI can
# upload it as an artifact, plus the host-side `info pci` of the QEMU monitor -
# the two values (interrupt pin and IRQ line) that image/pci_hw.cfg records.
#
# The assertion is deliberately narrow and deliberately strict: when the guest
# does not report our device (vid/did 1af4/1009), that is a device-discovery
# regression and this script exits 1, so the job goes red. A missing prerequisite
# exits 2 instead, which the caller reports as an explicit skip - the difference
# between "we did not look" and "we looked and it was gone" is the whole point.
#
# Usage: guest_probe.sh [--out DIR] [--port N] [--vm-dir DIR]
# Exit:  0 device seen   1 device missing or guest unreachable   2 prerequisite
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
QNX71=$(cd "$HERE/.." && pwd)
REPO=$(cd "$QNX71/../../.." && pwd)
SSH="$QNX71/qnx-ssh"

OUT=${QNX_PROBE_OUT:-/tmp/qnx71-probe}
PORT=${QNX_SSH_PORT:-2225}
VM=${QNX_VM_DIR:-$HOME/qnx71-vm}
GUEST_TIMEOUT=${QNX_GUEST_TIMEOUT:-90}
VID_DID=${PGW_VIRTIO_VID_DID:-1af4/1009}
VENDOR=${PGW_VIRTIO_VENDOR:-0x1af4}
DEVICE=${PGW_VIRTIO_DID_9P:-0x1009}

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;;
        --port) PORT=$2; shift 2 ;;
        --vm-dir) VM=$2; shift 2 ;;
        *) echo "guest_probe: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

log() { printf 'guest_probe: %s\n' "$*"; }

# Run one command in the guest, save its answer and its exit status.
# ssh exits 255 when the connection itself fails, and the banner of the image
# shows up a few seconds before sshd accepts a session (measured: exit 255 on
# the very first command of a probe that then ran fine), so that case is retried
# - three attempts, 5 s apart, never a longer fixed sleep.
run_guest() {
    name=$1
    shift
    log "guest> $*"
    attempt=1
    rc=1
    while [ "$attempt" -le 3 ]; do
        rc=0
        timeout "$GUEST_TIMEOUT" env QNX_VM_DIR="$VM" QNX_SSH_PORT="$PORT" \
            "$SSH" "$*" > "$OUT/$name" 2>&1 || rc=$?
        if [ "$rc" -eq 0 ] || [ "$rc" -ne 255 ]; then
            break
        fi
        if [ "$attempt" -lt 3 ]; then
            log "  ssh not ready yet (exit 255), attempt $attempt/3, retrying in 5s"
            sleep 5
        fi
        attempt=$((attempt + 1))
    done
    printf '%s: exit=%d (attempts=%d)\n' "$name" "$rc" "$attempt" \
        >> "$OUT/exit-codes.txt"
    if [ "$rc" -ne 0 ]; then
        log "  exit=$rc after $attempt attempt(s) (see $OUT/$name)"
    fi
    return 0
}

# Ask the QEMU monitor what the firmware assigned to our device (host side).
run_monitor() {
    log "monitor> info pci"
    rc=0
    timeout 60 python3 "$HERE/qemu_monitor.py" "info pci" \
        --vm-dir "$VM" --out "$OUT/host-qemu-info-pci.txt" > /dev/null || rc=$?
    printf 'host-qemu-info-pci.txt: exit=%d\n' "$rc" >> "$OUT/exit-codes.txt"
    [ "$rc" -eq 0 ] || log "  monitor unreachable (exit=$rc): host-side IRQ not captured"
}

collect() {
    mkdir -p "$OUT"
    : > "$OUT/exit-codes.txt"
    run_guest guest-uname.txt 'uname -a'
    run_guest guest-processes.txt 'pidin arg'
    run_guest guest-pci-tool-9p.txt "pci-tool -V $VENDOR -D $DEVICE -vvvvv"
    run_guest guest-pci-tool-all.txt 'pci-tool -vv'
    run_guest guest-proc-boot-pci_hw.cfg 'cat /proc/boot/pci_hw.cfg'
    run_guest guest-proc-boot-pci_server.cfg 'cat /proc/boot/pci_server.cfg'
    run_guest guest-proc-boot-pci-files.txt "ls /proc/boot | grep -E '^pci_'"
    run_monitor
}

# What QNX itself reports about our device: capabilities, address spaces, IRQs.
# The BDF header line precedes the vid/did line, so it is kept and printed too.
device_report() {
    awk -v dev="$VID_DID" '
        index($0, dev) { if (prev != "") print prev; keep = 14 }
        keep > 0 { print; keep-- }
        { prev = $0 }
    ' "$OUT/guest-pci-tool-9p.txt" 2>/dev/null | head -20
}

# The bus/device/function and the "IRQ n, pin X" QEMU assigned to our device.
qemu_assignment() {
    awk -v did="PCI device ${VID_DID%/*}:${VID_DID#*/}" '
        /^ *Bus/ { bus = $2; dev = $4; fun = $6; gsub(/[:,]/, "", bus);
                   gsub(/[:,]/, "", dev); gsub(/[:,]/, "", fun) }
        index($0, did) { hit = 1 }
        hit && /IRQ/ { irq = $2; gsub(/,/, "", irq); hit = 0;
                       printf "B%s:D%s:F%s IRQ %s pin %s\n", bus, dev, fun, irq, $4 }
    ' "$OUT/host-qemu-info-pci.txt" 2>/dev/null
}

# Does image/pci_hw.cfg still describe what QEMU wires today?
check_drift() {
    measured=$(qemu_assignment)
    [ -n "$measured" ] || { echo "fragment: no host-side assignment captured"; return 0; }
    fragment=$(awk '!/^#/ && NF >= 3 { print $1, $2, $3 }' "$REPO/image/pci_hw.cfg" | head -1)
    echo "measured (QEMU) : $measured"
    echo "image/pci_hw.cfg: $fragment"
    m_irq=$(printf '%s\n' "$measured" | awk '{ print $3 }')
    f_irq=$(printf '%s\n' "$fragment" | awk '{ print $3 }')
    if [ -n "$m_irq" ] && [ -n "$f_irq" ] && [ "$m_irq" != "$f_irq" ]; then
        echo "DRIFT: QEMU now wires IRQ $m_irq, the fragment still says $f_irq"
    fi
}

summarize() {
    {
        echo "# QNX 7.1 guest probe - $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        echo "# device under test: vid/did $VID_DID (virtio-9p-pci, mount tag passerelle)"
        echo
        echo "## What QNX reports"
        device_report
        echo
        echo "## What the image routes (its own /proc/boot/pci_hw.cfg)"
        grep -v '^#' "$OUT/guest-proc-boot-pci_hw.cfg" 2>/dev/null || true
        echo
        echo "## What the host assigned, and whether our fragment follows"
        check_drift
    } > "$OUT/summary.txt"
    cat "$OUT/summary.txt"
}

main() {
    if [ ! -x "$SSH" ]; then
        log "SKIP - $SSH is missing or not executable"
        exit 2
    fi
    log "guest at 127.0.0.1:$PORT, answers saved in $OUT"
    collect
    matches=$(cat "$OUT"/guest-pci-tool-*.txt 2>/dev/null | grep -c "$VID_DID" || true)
    summarize
    if [ "${matches:-0}" -eq 0 ]; then
        log "FAIL - the guest does not report vid/did $VID_DID: device discovery regressed"
        exit 1
    fi
    log "OK - the guest reports vid/did $VID_DID ($matches line(s))"
}

main "$@"
