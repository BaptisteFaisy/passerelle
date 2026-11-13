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

# The machine is q35 (ICH9, PCI Express), not pc (i440fx), and that is not a
# preference: the i440fx has no MCFG table, so QNX 8.0's PCI server falls back
# to "I/O accesses only" and its client-side hardware module then faults on a
# wild pointer for the first configuration-space read a driver makes (measured
# here: SIGSEGV at a randomised address from pci_device_read_vid()). With q35
# the firmware publishes MCFG, the server maps the ECAM at 0xb0000000 and the
# same call answers 0x1af4. See README.md, "Why q35".
#
# The device keeps PCI slot 5 (`addr=0x05`), so its BDF stays B0:D5:F0 and the
# interrupt line it gets is the one image/pci_hw.cfg carries.
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm -cpu host"
else
    ACCEL="-accel tcg,thread=multi -cpu max"
fi

# The outer timeout kills QEMU cleanly when the guest has shown what we need;
# the guest itself keeps running a shell on the console until then.
timeout 90 qemu-system-x86_64 \
    -machine q35 \
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