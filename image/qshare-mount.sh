#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# image/qshare-mount.sh - mount the PC's shared folder in the guest.
#
# The script runs in the guest, from the start-up script of the image (see
# passerelle.build). It:
#
#   1. waits for the virtio device to appear (the PCI server may still be
#      enumerating when the script starts);
#   2. creates the mount point if it is missing;
#   3. starts the driver, which registers itself at that point;
#   4. writes what happened to the system log (QNX-08), so an operator can
#      read the result with `slog2info` instead of guessing from the console.
#
# The environment variables come from the start-up script or from the person
# who started the guest:
#
#   QSHARE_TAG        mount tag of the share (default: "hostshare")
#   QSHARE_MOUNT      mount point (default: "/host")
#   QSHARE_OPTS       extra options for the driver (default: "ro,nosuid,noexec")
#   QSHARE_DRIVER     "fs9p" (default) or "fsfuse"
#   QSHARE_TIMEOUT    seconds to wait for the device (default: 30)
#
# Covered requirements: QNX-07 (mount and umount), QNX-08 (system log),
# VIO-05 (the share is chosen by its tag), FS-09 / SEC-01 (the default options
# refuse setuid and execution from the share).

TAG="${QSHARE_TAG:-hostshare}"
MOUNT="${QSHARE_MOUNT:-/host}"
OPTS="${QSHARE_OPTS:-ro,nosuid,noexec,debug}"
DRIVER="${QSHARE_DRIVER:-fs9p}"
TIMEOUT="${QSHARE_TIMEOUT:-30}"

log() {
    # slogger2 is running in the image; when it is not, the console is the
    # fallback. Both lines carry the same prefix.
    if command -v slog2 >/dev/null 2>&1; then
        slog2 -t qshare "qshare-mount: $*"
    fi
    echo "qshare-mount: $*"
}

# The device shows up when the PCI server has enumerated it. The driver's own
# error message tells the two cases apart, so the loop simply retries.
waited=0
while [ "$waited" -lt "$TIMEOUT" ]; do
    if [ ! -d "$MOUNT" ]; then
        mkdir -p "$MOUNT" 2>/dev/null || true
    fi
    log "mounting $TAG on $MOUNT with $DRIVER ($OPTS)"
    if "$DRIVER" -o "$OPTS" "$TAG" "$MOUNT"; then
        log "mounted $TAG on $MOUNT"
        log "listing $MOUNT:"
        ls "$MOUNT"
        log "reading $MOUNT/from-host.txt:"
        cat "$MOUNT/from-host.txt"
        exit 0
    fi
    log "device not ready yet, retrying"
    sleep 1
    waited=$((waited + 1))
done

log "no $DRIVER device with tag $TAG after ${TIMEOUT}s: giving up"
log "system log so far:"
slog2info
exit 1
