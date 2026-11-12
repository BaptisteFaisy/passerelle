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
# What makes it reproducible (gap 74): the container is not pulled by tag but
# by **digest**, taken from the committed manifest
# (tools/qnx-image/qnx71-image.manifest.json), and the extracted disk is
# verified against the size and the SHA256 the same manifest pins. A tag that
# was re-pushed, a truncated extraction or a disk from another build is
# refused with a loud message, never used.
#
# Rules it follows (see ~/Bureau/CI-BUDGET-5MIN.md and the workflow that calls
# it): every network and disk step is bounded by `timeout`, an image already on
# disk is verified and reused instead of downloaded again, and a missing
# prerequisite is reported as a *skip* (exit 2) - the caller turns that into an
# explicit notice, never into a silent green and never into a red build.
#
# Exit status:
#   0  the disk image is ready at $QNX_VM_DIR/qnx71-fs.img and matches the pin
#   2  prerequisite unavailable (no docker, no daemon, no free space, pull
#      failed): the caller must skip with an explicit notice
#   1  the image was fetched but is not usable (truncated extraction, digest
#      mismatch, pinned artefact replaced)
#
# Environment (all optional):
#   QNX_VM_DIR         working directory of the guest  (default ~/qnx71-vm)
#   QNX_IMAGE_MANIFEST  pinned manifest            (default the one in tools/)
#   QNX_IMAGE_TOOL      manifest tool              (default the one in tools/)
#   QNX_PULL_TIMEOUT   ceiling of docker pull, s   (default 600)
#   QNX_COPY_TIMEOUT   ceiling of docker cp, s     (default 600)
#   QNX_REQUIRED_KB    free space needed, KiB      (default 9000000)
#   QNX_MIN_IMAGE_BYTES  smallest accepted image   (default 1 GiB)
#   QNX_CI_CLEANUP=1   drop the pulled image afterwards (ephemeral runners)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
MANIFEST=${QNX_IMAGE_MANIFEST:-$REPO/tools/qnx-image/qnx71-image.manifest.json}
TOOL=${QNX_IMAGE_TOOL:-$REPO/tools/qnx-image/image_manifest.py}

VM=${QNX_VM_DIR:-$HOME/qnx71-vm}
IMG="$VM/qnx71-fs.img"
PULL_TIMEOUT=${QNX_PULL_TIMEOUT:-600}
COPY_TIMEOUT=${QNX_COPY_TIMEOUT:-600}
REQUIRED_KB=${QNX_REQUIRED_KB:-9000000}
MIN_BYTES=${QNX_MIN_IMAGE_BYTES:-1073741824}

log() { printf 'fetch_image: %s\n' "$*"; }

skip() { log "SKIP - $*"; exit 2; }

fail() { log "FAIL - $*"; exit 1; }

# One field of the pinned manifest (docker-reference, digest, disk-sha256...).
manifest_field() {
    python3 "$TOOL" show --manifest "$MANIFEST" --field "$1"
}

# Path of the disk inside the container: the pin says where it is. The
# environment variable overrides it, for the mock exercises of the README.
disk_path() {
    printf '%s\n' "${QNX_GUEST_DISK:-$(manifest_field disk-path)}"
}

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

# Pull the pinned image. The reference is repository@digest: docker verifies
# every layer against the digests of the manifest, so the pull *is* the
# checksum verification of the source artefact.
pull_image() {
    ref=$(manifest_field docker-reference)
    tagref=$(manifest_field reference-by-tag)
    log "pulling $ref (ceiling ${PULL_TIMEOUT}s)"
    if timeout "$PULL_TIMEOUT" docker pull "$ref"; then
        IMAGE_REF=$ref
        return 0
    fi
    # The pinned digest did not pull. Is it the network, or was the pinned
    # artefact replaced? Pulling the tag tells the two apart.
    log "the pinned digest did not pull; trying $tagref to tell a network problem from a replaced artefact"
    if timeout "$PULL_TIMEOUT" docker pull "$tagref"; then
        fail "the pinned digest $ref is not pullable while $tagref is: the pinned artefact was replaced or removed"
    fi
    skip "docker pull failed (registry, bandwidth or rate limit)"
}

# The pulled image must carry the pinned digest: a tag that moved is refused.
check_source_digest() {
    pinned=$(manifest_field digest)
    digests=$(docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' \
        "$1" 2>/dev/null || true)
    case "$digests" in
        *"$pinned"*) log "the pulled image carries the pinned digest $pinned" ;;
        *) fail "the pulled image does not carry the pinned digest $pinned (RepoDigests: ${digests:-none})" ;;
    esac
}

# Copy the disk out of a throw-away container. $1 = container id.
extract_disk() {
    cid=$1
    disk=$(disk_path)
    log "extracting $disk to $IMG (ceiling ${COPY_TIMEOUT}s)"
    rc=0
    timeout "$COPY_TIMEOUT" docker cp "$cid:$disk" "$IMG" || rc=$?
    timeout 60 docker rm "$cid" >/dev/null 2>&1 || true
    [ "$rc" -eq 0 ] || skip "docker cp $disk failed (code $rc)"
}

cleanup_image() {
    [ "${QNX_CI_CLEANUP:-0}" = "1" ] || return 0
    log "QNX_CI_CLEANUP=1: dropping $IMAGE_REF to give the disk back"
    timeout 120 docker rmi "$IMAGE_REF" >/dev/null 2>&1 || true
}

# The disk must match the pin before it is used. Exit 3 from the tool means
# "the manifest pins no SHA256 yet": the observed digest is recorded and the
# job says so out loud, instead of pretending the disk is verified.
verify_disk() {
    rc=0
    python3 "$TOOL" verify --manifest "$MANIFEST" --file "$IMG" \
        > "$VM/disk-verify.txt" 2>&1 || rc=$?
    cat "$VM/disk-verify.txt"
    case "$rc" in
        0) return 0 ;;
        3)
            python3 "$TOOL" digest --file "$IMG" > "$VM/disk.sha256" 2>/dev/null || true
            log "NOTICE - the manifest pins no SHA256 for the disk yet;"
            log "         the observed digest is in $VM/disk.sha256"
            log "         pin it with: python3 tools/qnx-image/image_manifest.py pin"
            log "           --manifest tools/qnx-image/qnx71-image.manifest.json --file $IMG"
            return 0
            ;;
        *) fail "the disk does not match the pinned manifest (see $VM/disk-verify.txt)" ;;
    esac
}

main() {
    mkdir -p "$VM"
    [ -f "$MANIFEST" ] || fail "the pinned manifest is missing: $MANIFEST"
    [ -f "$TOOL" ] || fail "the manifest tool is missing: $TOOL"
    have=$(image_bytes)
    if [ "$have" -ge "$MIN_BYTES" ]; then
        log "image already on disk: $IMG ($have bytes) - not downloading it again"
        verify_disk
        return 0
    fi
    [ "$have" -eq 0 ] || log "image on disk is $have bytes (< $MIN_BYTES), fetching it again"
    check_docker
    check_space
    pull_image
    check_source_digest "$IMAGE_REF"
    cid=$(timeout 60 docker create "$IMAGE_REF") || skip "docker create $IMAGE_REF failed"
    extract_disk "$cid"
    cleanup_image
    have=$(image_bytes)
    [ "$have" -ge "$MIN_BYTES" ] || fail "extracted image is $have bytes, expected >= $MIN_BYTES"
    log "image ready: $IMG ($have bytes)"
    verify_disk
}

main "$@"
