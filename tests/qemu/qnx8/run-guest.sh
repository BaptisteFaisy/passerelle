#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# run-guest.sh - boot the Passerelle guest under QEMU and capture the console.
#
# The guest is a QNX 8.0 IFS booted with `-kernel`; the PCI server runs in the
# start-up script and prints a `pci-tool -v` inventory that must show the
# virtio-9p-pci device (vid/did 1af4/1009). The device is pinned to PCI slot 5
# (`addr=0x05`), which SeaBIOS routes to IRQ 10 pin A - the entry already in
# image/pci_hw.cfg.
#
# Usage:
#   ./run-guest.sh [ifs_image] [share_dir] [console_log]
#
# Environment:
#   TAG    mount tag of the share (default: passerelle)

set -eu

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD="$ROOT/build/qnx8"
IFS_IMAGE="${1:-$BUILD/passerelle.ifs}"
SHARE_DIR="${2:-$BUILD/share}"
LOG="${3:-$BUILD/console.log}"
# The tag must be the one the guest's mount script asks for: the driver opens
# the device whose mount tag matches (VIO-05), and the guest script defaults
# to "hostshare", the tag the whole project uses.
TAG="${TAG:-hostshare}"

mkdir -p "$SHARE_DIR"
printf 'hello-from-the-pc\n' > "$SHARE_DIR/from-host.txt"
rm -f "$LOG"

# KVM when the device is readable and writable, TCG otherwise (see QEMU.md of
# the sibling relais project). `-accel` and `-enable-kvm` are never combined.
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm -cpu host"
else
    ACCEL="-accel tcg,thread=multi -cpu max"
fi

# The outer timeout kills QEMU cleanly when the guest has shown what we need;
# the guest itself keeps running a shell on the console until then.
timeout 90 qemu-system-x86_64 \
    -machine pc \
    -smp 1 \
    -m 512 \
    $ACCEL \
    -kernel "$IFS_IMAGE" \
    -fsdev "local,id=host,path=$SHARE_DIR,security_model=mapped-xattr" \
    -device "virtio-9p-pci,fsdev=host,mount_tag=$TAG,addr=0x05" \
    -display none \
    -serial "file:$LOG" \
    -monitor none \
    -no-reboot

echo "console captured in $LOG"