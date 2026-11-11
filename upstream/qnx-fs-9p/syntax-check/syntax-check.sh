#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Syntax-check the QNX-only sources on a machine without the QNX SDP.
#
#   ./syntax-check.sh /path/to/qnx-fs-9p-worktree
#
# The virtio-pci transport includes <pci/pci.h>, <sys/neutrino.h>,
# <sys/mman.h>, <sys/dispatch.h>, <sys/rsrcdbmgr.h> and <sys/cpuinline.h>,
# and main.c reaches <sys/iofunc.h> and <sys/resmgr.h> through resmgr.h. This
# script supplies declarations-only stand-ins for all of them (include/) and
# runs the compiler with -fsyntax-only, so a typo, a wrong argument count or a
# type error is caught here instead of in a guest build.
#
# It proves that the files compile as C11 under the project's own strict flag
# set. It proves nothing about behaviour: only an SDP build plus a QEMU run
# does (see ../../validation.md).
set -eu

src=${1:?usage: syntax-check.sh <qnx-fs-9p-worktree>}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cc=${CC:-cc}

# _QNX_SOURCE is what the real build defines; the two glibc feature macros are
# only there so that <unistd.h> and <time.h> expose usleep()/nanosleep() when
# the check runs on a Linux host under -std=c11.
flags="-std=c11 -D_QNX_SOURCE -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
       -Wall -Wextra -Wpedantic -Werror -fsyntax-only"
incs="-I$here/include -I$src/src -I$src/lib9p/include"

for f in src/virtio_pci_caps.c src/transport_virtio_pci.c src/virtio_pci_ring.c \
         src/main.c; do
    printf 'syntax-check: %s\n' "$f"
    # shellcheck disable=SC2086 # the flag words are meant to split
    $cc $flags $incs "$src/$f"
done

printf 'syntax-check: OK\n'
