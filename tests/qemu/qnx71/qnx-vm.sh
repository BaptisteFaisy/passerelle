#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# qnx-vm.sh - boot the QNX 7.1 guest on this PC with the Passerelle share.
#
# What this is: a real QNX Neutrino 7.1.0 x86_64 guest, booted by QEMU with a
# virtio-9p-pci device exported from a host directory. The image has no serial
# console, so the text channel is the VGA framebuffer (driven through the QEMU
# monitor by qnx_console.py) and SSH over a forwarded port.
#
# What this is NOT: a QNX SDP. The image ships no compiler (`qcc` is absent), so
# nothing can be cross-compiled here. The guest is for observing the QNX side of
# the bus: what QNX reports about a virtio-9p PCI device.
#
# The disk image comes from the community container `autosarai/qemu_qnx7_1`
# (QNX 7.1.0, built 2022-07-12), see README.md for the one-off extraction.
#
# Usage:
#   ./qnx-vm.sh start            # boot (QNX_VM_DIR holds the image)
#   ./qnx-vm.sh stop | status
#   ./qnx-vm.sh ssh '<command>'  # run a command in the guest
#   ./qnx-vm.sh console "text" [out.png]   # type on the VGA console
set -eu

DIR=$(cd "$(dirname "$0")" && pwd)
VM=${QNX_VM_DIR:-$HOME/qnx71-vm}
IMG="$VM/qnx71-fs.img"
SHARE="$VM/share"
MON="$VM/mon.sock"
PIDFILE="$VM/qemu.pid"

start() {
    [ -f "$IMG" ] || { echo "image missing: $IMG (see README.md)" >&2; exit 1; }
    mkdir -p "$SHARE"
    [ -f "$SHARE/from-host.txt" ] || echo "hello-from-the-pc" > "$SHARE/from-host.txt"
    rm -f "$MON"
    # -device usb-kbd: the image's console expects a USB keyboard, which is what
    # `sendkey` drives. The four hostfwd rules cover the addresses the image has
    # used across reboots; only one answers with an SSH banner.
    setsid nohup qemu-system-x86_64 \
        -drive format=raw,file="$IMG" \
        -m 1024 -smp 2 \
        -netdev "user,id=n0,net=192.168.100.0/24,dhcpstart=192.168.100.60,hostfwd=tcp:127.0.0.1:2222-192.168.100.12:22,hostfwd=tcp:127.0.0.1:2223-192.168.100.2:22,hostfwd=tcp:127.0.0.1:2224-192.168.100.100:22,hostfwd=tcp:127.0.0.1:2225-192.168.100.10:22" \
        -device vmxnet3,netdev=n0 \
        -device usb-ehci -device usb-kbd -usb \
        -fsdev local,id=pgwshare,path="$SHARE",security_model=mapped-xattr \
        -device virtio-9p-pci,fsdev=pgwshare,mount_tag=passerelle \
        -display none -serial file:"$VM/serial.log" \
        -monitor unix:"$MON",server,nowait \
        "$@" > "$VM/qemu.out" 2>&1 < /dev/null &
    echo $! > "$PIDFILE"
    echo "QNX 7.1 started (pid $(cat "$PIDFILE")): VGA console + ssh on 127.0.0.1:2222..2225"
}

stop() {
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
        echo "stopped"
    else
        pkill -f "$IMG" 2>/dev/null || true
        echo "no pid file, stopped by pattern"
    fi
}

status() {
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        echo "running (pid $(cat "$PIDFILE"))"
    else
        echo "not running"
    fi
}

case "${1:-status}" in
    start) shift; start "$@" ;;
    stop) stop ;;
    status) status ;;
    ssh) shift; exec "$DIR/qnx-ssh" "$@" ;;
    console) shift; exec python3 "$DIR/qnx_console.py" "$@" ;;
    *) echo "usage: $0 {start|stop|status|ssh|console}"; exit 2 ;;
esac
