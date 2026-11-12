#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# fetch_image.sh - make the QNX 7.1 guest disk available under $QNX_VM_DIR.
#
# The disk is not in this repository: it is QNX software under QNX's
# non-commercial terms. Like the workstation procedure of
# tests/qemu/qnx71/README.md, it is extracted here from the public community
# container `autosarai/qemu_qnx7_1`; this script is the CI form of those four
# commands, so a job never depends on a disk somebody left on a machine.
#
# Rules it follows (see ~/Bureau/CI-BUDGET-5MIN.md and the workflow that calls
# it): every network and disk step is bounded by `timeout`, an image already on
# disk is reused instead of downloaded again, and a missing prerequisite is
# reported as a *skip* (exit 2) - the caller turns that into an explicit notice,
# never into a silent green and never into a red build.
#
# Exit status:
#   0  the disk image is ready at $QNX_VM_DIR/qnx71-fs.img
#   2  prerequisite unavailable (no docker, no daemon, no free space, pull
#      failed): the caller must skip with an explicit notice
#   1  the image was fetched but is not usable (truncated extraction)
#
# Environment (all optional):
#   QNX_VM_DIR         working directory of the guest  (default ~/qnx71-vm)
#   QNX_GUEST_IMAGE    container to pull      (default autosarai/qemu_qnx7_1:1.1.0)
#   QNX_GUEST_DISK     disk inside it         (default /home/popcornsar/fs_3GB.img)
#   QNX_PULL_TIMEOUT   ceiling of docker pull, s   (default 600)
#   QNX_COPY_TIMEOUT   ceiling of docker cp, s     (default 600)
#   QNX_REQUIRED_KB    free space needed, KiB      (default 9000000)
#   QNX_MIN_IMAGE_BYTES  smallest accepted image   (default 1 GiB)
#   QNX_CI_CLEANUP=1   drop the pulled image afterwards (ephemeral runners)
set -eu

VM=${QNX_VM_DIR:-$HOME/qnx71-vm}
IMG="$VM/qnx71-fs.img"
IMAGE=${QNX_GUEST_IMAGE:-autosarai/qemu_qnx7_1:1.1.0}
DISK=${QNX_GUEST_DISK:-/home/popcornsar/fs_3GB.img}
PULL_TIMEOUT=${QNX_PULL_TIMEOUT:-600}
COPY_TIMEOUT=${QNX_COPY_TIMEOUT:-600}
REQUIRED_KB=${QNX_REQUIRED_KB:-9000000}
MIN_BYTES=${QNX_MIN_IMAGE_BYTES:-1073741824}

log() { printf 'fetch_image: %s\n' "$*"; }

skip() { log "SKIP - $*"; exit 2; }

fail() { log "FAIL - $*"; exit 1; }

# Size in bytes of the extracted disk, 0 when it is not there.
image_bytes() {
    [ -f "$IMG" ] || { printf '0\n'; return 0; }
    wc -c < "$IMG" | tr -d '[:space:]'
}

# Free space in KiB of the filesystem holding $1 (empty when unknown).
free_kb() {
    df -Pk "$1" 2>/dev/null | awk 'NR==2 {print $4}'
}

check_docker() {
    command -v docker >/dev/null 2>&1 || skip "docker is not installed"
    timeout 30 docker info >/dev/null 2>&1 || skip "the docker daemon is not reachable"
    log "docker $(docker --version 2>/dev/null | head -1)"
}

check_space() {
    avail=$(free_kb "$VM")
    [ -n "$avail" ] || skip "cannot read the free space of $VM"
    if [ "$avail" -lt "$REQUIRED_KB" ]; then
        skip "only ${avail} KiB free under $VM, ${REQUIRED_KB} KiB needed"
    fi
    log "${avail} KiB free under $VM"
}

pull_image() {
    log "pulling $IMAGE (ceiling ${PULL_TIMEOUT}s)"
    if ! timeout "$PULL_TIMEOUT" docker pull "$IMAGE"; then
        skip "docker pull $IMAGE failed (registry, bandwidth or rate limit)"
    fi
}

# Copy the disk out of a throw-away container. $1 = container id.
extract_disk() {
    cid=$1
    log "extracting $DISK to $IMG (ceiling ${COPY_TIMEOUT}s)"
    rc=0
    timeout "$COPY_TIMEOUT" docker cp "$cid:$DISK" "$IMG" || rc=$?
    timeout 60 docker rm "$cid" >/dev/null 2>&1 || true
    [ "$rc" -eq 0 ] || skip "docker cp $DISK failed (code $rc)"
}

cleanup_image() {
    [ "${QNX_CI_CLEANUP:-0}" = "1" ] || return 0
    log "QNX_CI_CLEANUP=1: dropping $IMAGE to give the disk back"
    timeout 120 docker rmi "$IMAGE" >/dev/null 2>&1 || true
}

main() {
    mkdir -p "$VM"
    have=$(image_bytes)
    if [ "$have" -ge "$MIN_BYTES" ]; then
        log "image already on disk: $IMG ($have bytes) - not downloading it again"
        return 0
    fi
    [ "$have" -eq 0 ] || log "image on disk is $have bytes (< $MIN_BYTES), fetching it again"
    check_docker
    check_space
    pull_image
    cid=$(timeout 60 docker create "$IMAGE") || skip "docker create $IMAGE failed"
    extract_disk "$cid"
    cleanup_image
    have=$(image_bytes)
    [ "$have" -ge "$MIN_BYTES" ] || fail "extracted image is $have bytes, expected >= $MIN_BYTES"
    log "image ready: $IMG ($have bytes)"
}

main "$@"
