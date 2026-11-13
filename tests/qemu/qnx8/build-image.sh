#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# build-image.sh - build the QNX 8.0 IFS of the Passerelle guest (see README.md).
#
# Usage:
#   ./build-image.sh            # image at build/qnx8/passerelle.ifs
#
# Environment:
#   QNX_SDP_ENV   path to qnxsdp-env.sh (default: $HOME/qnx800/qnxsdp-env.sh)
#
# The fragment skips fs9p/fsfuse when they are not built yet, so the image
# boots with the kernel, the C library and the PCI server alone.

set -eu

SDP_ENV="${QNX_SDP_ENV:-$HOME/qnx800/qnxsdp-env.sh}"

# mkifs needs QNX_HOST / QNX_TARGET / PROCESSOR in its environment.
if [ -z "${QNX_TARGET:-}" ]; then
    if [ ! -f "$SDP_ENV" ]; then
        echo "SDP not found: $SDP_ENV (set QNX_SDP_ENV)" >&2
        exit 1
    fi
    . "$SDP_ENV"
fi

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
BUILD="$ROOT/build/qnx8"
mkdir -p "$BUILD/stage"

stage() {
    # Copy a built driver into the staging dir when it is present; the fragment's
    # [+optional] entries skip the ones that do not exist.
    src="$1"
    name="$2"
    if [ -x "$src" ]; then
        cp "$src" "$BUILD/stage/$name"
        echo "staged $name from $src"
    else
        echo "note: $name not built yet - the image boots without it"
    fi
}

stage "$ROOT/build-qnx/drivers/fs9p" fs9p
stage "$ROOT/build-qnx/drivers/fsfuse" fsfuse

cd "$ROOT"
mkifs -v -v -o "$BUILD" tests/qemu/qnx8/passerelle.build "$BUILD/passerelle.ifs"
ls -l "$BUILD/passerelle.ifs"