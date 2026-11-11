/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pgw/dirent_qnx.h - the QNX `struct dirent` record, written by the readdir
 * handler.
 *
 * A QNX readdir() reply is a packed sequence of records whose layout differs
 * from Linux's (no d_type, but an explicit d_namelen, and records are 8-byte
 * aligned). The layout is reproduced here so the PC build can compile the
 * handler against the stub headers, and drivers/tests/qnx_abi_assert.c checks
 * field for field that it matches the real <dirent.h> on QNX.
 *
 * Covered requirement: FS-03 (a coherent directory listing: every entry typed,
 * resumable at an offset).
 */
#ifndef PGW_DIRENT_QNX_H
#define PGW_DIRENT_QNX_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct pgw_dirent_qnx {
    ino_t d_ino;
    off_t d_offset; /* cookie to resume the listing at (FS-03) */
    int16_t d_reclen;
    int16_t d_namelen;
    char d_name[1];
};

/* Offset of the name inside the record, and the aligned record size for a name
 * of `namelen` bytes plus its terminator. */
#define PGW_DIRENT_NAME_OFF offsetof(struct pgw_dirent_qnx, d_name)

static inline size_t pgw_dirent_reclen(size_t namelen)
{
    return (PGW_DIRENT_NAME_OFF + namelen + 1u + 7u) & ~(size_t)7u;
}

#endif /* PGW_DIRENT_QNX_H */
