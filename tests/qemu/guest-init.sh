#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# guest-init.sh - /init of the test guest used by tests/qemu/run_share_check.py.
#
# It runs inside a minimal Linux initramfs booted by QEMU with the exact share
# options that `qshare` generates. Its job is feasibility test E5 of the
# specification (does virtiofsd + shared guest memory work on this PC with a
# Linux guest?) and, by the same run, the 9P half of the host-side plumbing.
#
# Everything is reported on the serial console as PGW-E5-* markers, because that
# is the only channel the host harness reads. No marker means failure.
#
# Kernel command line (set by the harness):
#   pgw.share=virtiofs|9p  pgw.tag=<tag>  pgw.mount=<dir>

set -u

MARK="PGW-E5"
SHARE=/host
say() { echo "$MARK $*"; }

pgw_cmdline_value() {
    # $1 = key; prints the value found on /proc/cmdline, empty if absent
    tr ' ' '\n' < /proc/cmdline | sed -n "s/^$1=//p" | head -n 1
}

mount_pseudo() {
    mount -t proc  proc  /proc  2>/dev/null
    mount -t sysfs sysfs /sys   2>/dev/null
    mount -t devtmpfs dev /dev  2>/dev/null
    [ -d /dev ] || mkdir -p /dev
}

load_modules() {
    # Built as modules in the Ubuntu 6.14 kernel: virtiofs, 9p, 9pnet_virtio.
    # A missing module is not fatal on its own: the mount below decides.
    for m in /modules/*.ko; do
        [ -e "$m" ] || continue
        insmod "$m" 2>/dev/null || say "WARN insmod $m failed"
    done
}

mount_share() {
    kind=$(pgw_cmdline_value pgw.share)
    tag=$(pgw_cmdline_value pgw.tag)
    mkdir -p "$SHARE"
    case "$kind" in
        virtiofs)
            mount -t virtiofs "$tag" "$SHARE" 2>&1 | sed "s/^/$MOUNT-ERR /"
            ;;
        9p)
            mount -t 9p -o trans=virtio,version=9p2000.L,cache=none "$tag" \
                "$SHARE" 2>&1 | sed "s/^/$MOUNT-ERR /"
            ;;
        *)
            say "FAIL unknown pgw.share='$kind'"; return 1 ;;
    esac
    if mountpoint -q "$SHARE" 2>/dev/null || grep -q " $SHARE " /proc/mounts; then
        say "PASS mounted $kind tag=$tag on $SHARE"
        return 0
    fi
    say "FAIL mount of $kind tag=$tag did not succeed"
    grep -E " $SHARE | virtiofs | 9p " /proc/mounts || true
    return 1
}

check_host_to_guest() {
    # Written by the harness on the PC *before* the boot.
    if [ -r "$SHARE/from-host.txt" ]; then
        content=$(cat "$SHARE/from-host.txt")
        say "INFO host->guest content: $content"
        say "PASS host-to-guest read"
    else
        say "FAIL host-to-guest: $SHARE/from-host.txt not readable"
        return 1
    fi
}

check_guest_to_host() {
    stamp=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)
    if echo "written-from-guest $stamp" > "$SHARE/from-guest.txt" 2>/dev/null; then
        say "PASS guest-to-host write"
    else
        say "FAIL guest-to-host write refused"
        return 1
    fi
}

check_exec_from_share() {
    # FS-06 in spirit: a binary served by the share runs without being copied.
    if [ -x "$SHARE/hello" ]; then
        out=$("$SHARE/hello" 2>&1)
        rc=$?
        say "INFO exec-from-share rc=$rc out=$out"
        [ "$rc" -eq 0 ] && say "PASS execute-from-share" \
                        || say "FAIL execute-from-share rc=$rc"
    else
        say "INFO execute-from-share skipped (no $SHARE/hello provided)"
    fi
}

report_environment() {
    say "INFO kernel $(uname -r) $(uname -m)"
    say "INFO virtio devices: $(ls -d /sys/bus/virtio/devices/* 2>/dev/null | wc -l)"
    for d in /sys/bus/virtio/devices/*; do
        [ -e "$d/device" ] || continue
        say "INFO virtio $(basename "$d") device=$(cat "$d/device")"
    done
}

main() {
    mount_pseudo
    load_modules
    report_environment
    rc=0
    mount_share            || rc=1
    if [ "$rc" -eq 0 ]; then
        check_host_to_guest  || rc=1
        check_guest_to_host  || rc=1
        check_exec_from_share
    fi
    [ "$rc" -eq 0 ] && say "RESULT PASS" || say "RESULT FAIL"
    say "DONE"
    sync
    # -f: no init system, no /sbin/poweroff binary in a minimal initramfs.
    poweroff -f 2>/dev/null || halt -f 2>/dev/null || echo o > /proc/sysrq-trigger
}

main "$@"
