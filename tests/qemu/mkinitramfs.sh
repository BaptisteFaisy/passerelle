#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# mkinitramfs.sh - build the minimal Linux guest used by tests/qemu (E5).
#
# The guest is an Alpine minirootfs plus the few filesystem modules the host
# kernel ships out-of-tree (virtiofs, 9p, 9pnet_virtio), plus guest-init.sh as
# /init. Nothing else: no network configuration, no package manager, no disk.
#
# Usage: mkinitramfs.sh <output.cpio.gz> [kernel-version] [cache-dir]
#
# Requires: curl, tar, cpio, gzip, unzstd (the Ubuntu modules are .ko.zst).
set -eu

OUT=${1:?usage: mkinitramfs.sh <output.cpio.gz> [kernel-version] [cache-dir]}
KVER=${2:-$(uname -r)}
CACHE=${3:-${HOME}/.cache/passerelle}
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-3.21.9-x86_64.tar.gz"
HERE=$(cd "$(dirname "$0")" && pwd)
MODDIR="/lib/modules/${KVER}/kernel"

mkdir -p "$CACHE"
ROOT="${CACHE}/rootfs-${KVER}"
TARBALL="${CACHE}/alpine-minirootfs.tar.gz"

if [ ! -x "${ROOT}/bin/busybox" ]; then
    echo "mkinitramfs: unpacking the Alpine minirootfs into ${ROOT}"
    [ -f "$TARBALL" ] || curl -fsSL --retry 2 -o "$TARBALL" "$ALPINE_URL"
    rm -rf "$ROOT"
    mkdir -p "$ROOT"
    tar xzf "$TARBALL" -C "$ROOT"
fi

echo "mkinitramfs: collecting modules of kernel ${KVER}"
rm -rf "${ROOT}/modules"
mkdir -p "${ROOT}/modules"
# Load order matters and the names below enforce it: netfs first (9pnet needs
# it, and it carries the fscache symbols since 6.x), then the 9p stack, then the
# filesystems. A missing module is not fatal: it may be built into the kernel.
found_count=0
i=10
for mod in fs/netfs/netfs net/9p/9pnet net/9p/9pnet_virtio fs/9p/9p \
           fs/fuse/virtiofs; do
    found=""
    for ext in ".ko.zst" ".ko.xz" ".ko"; do
        if [ -f "${MODDIR}/${mod}${ext}" ]; then
            found="${MODDIR}/${mod}${ext}"
            break
        fi
    done
    if [ -z "$found" ]; then
        echo "mkinitramfs: ${mod} not a module (built into the kernel?) - skipped"
        continue
    fi
    found_count=$((found_count + 1))
    name=$(basename "$found")
    name=${name%.ko.zst}; name=${name%.ko.xz}; name=${name%.ko}
    dest="${ROOT}/modules/${i}-${name}.ko"
    case "$found" in
        *.zst) unzstd -q -c "$found" > "$dest" ;;
        *.xz)  unxz  -q -c "$found" > "$dest" ;;
        *)     cp "$found" "$dest" ;;
    esac
    i=$((i + 10))
done
ls -1 "${ROOT}/modules" | sed 's/^/mkinitramfs:   + /'

# Neither virtiofs nor 9p available as a module: this kernel cannot take part in
# the test. Exit 3 so the caller reports an explicit skip rather than booting a
# guest that cannot mount anything and reporting a bogus failure.
if [ "$found_count" -eq 0 ]; then
    echo "mkinitramfs: no filesystem module found for ${KVER}" >&2
    exit 3
fi

cp "${HERE}/guest-init.sh" "${ROOT}/init"
chmod 0755 "${ROOT}/init"
mkdir -p "${ROOT}/proc" "${ROOT}/sys" "${ROOT}/dev" "${ROOT}/host"

echo "mkinitramfs: packing ${OUT}"
rm -f "$OUT"
(cd "$ROOT" && find . | LC_ALL=C sort | cpio --quiet -o -H newc) | gzip -1 > "$OUT"
echo "mkinitramfs: $(du -h "$OUT" | cut -f1) written"
