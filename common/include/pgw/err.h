/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/err.h - error codes, open flags and file modes across three worlds.
 *
 * Three worlds speak different numbers for the same idea:
 *   - the wire (9P2000.L Rlerror, FUSE fuse_out_header.error) carries *Linux*
 *     values, on every architecture;
 *   - the guest OS is QNX Neutrino, whose <errno.h>, <fcntl.h> and <sys/stat.h>
 *     numbers differ from Linux above 34 (ENOSYS is 38 on Linux, 89 on QNX);
 *   - the PC test build runs on the host OS.
 *
 * The bridge is a stable internal enum, pgw_err. Nothing is ever cast from one
 * numbering to the other:
 *
 *   wire (Linux number) <- pgw_linux_errno()      <- pgw_err
 *   host/QNX errno      <- pgw_host_errno()       <- pgw_err
 *
 * pgw_code_from_linux() is the table that the tests pin down value by value
 * (P9-04, spec 5.3 "valeurs Linux"). pgw_host_errno() is compiled from the real
 * <errno.h> of the target, so on QNX it yields QNX numbers and on the PC it
 * yields host numbers - the structure of the table is what is unit-tested, the
 * QNX numbers themselves are asserted at build time on QNX (see
 * tests/qnx_abi_assert.c and docs/qnx-abi.md).
 */
#ifndef PGW_ERR_H
#define PGW_ERR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal, stable error codes. Order and numbering are ours; only the mapping
 * tables are derived from POSIX. PGW_OK is 0 so that `if (rc)` works. */
typedef enum pgw_err {
    PGW_OK = 0,
    PGW_EPERM,
    PGW_ENOENT,
    PGW_ESRCH,
    PGW_EINTR,
    PGW_EIO,
    PGW_ENXIO,
    PGW_E2BIG,
    PGW_ENOEXEC,
    PGW_EBADF,
    PGW_ECHILD,
    PGW_EAGAIN,
    PGW_ENOMEM,
    PGW_EACCES,
    PGW_EFAULT,
    PGW_ENOTBLK,
    PGW_EBUSY,
    PGW_EEXIST,
    PGW_EXDEV,
    PGW_ENODEV,
    PGW_ENOTDIR,
    PGW_EISDIR,
    PGW_EINVAL,
    PGW_ENFILE,
    PGW_EMFILE,
    PGW_ENOTTY,
    PGW_ETXTBSY,
    PGW_EFBIG,
    PGW_ENOSPC,
    PGW_ESPIPE,
    PGW_EROFS,
    PGW_EMLINK,
    PGW_EPIPE,
    PGW_EDOM,
    PGW_ERANGE,
    PGW_EDEADLK,
    PGW_ENAMETOOLONG,
    PGW_ENOLCK,
    PGW_ENOSYS,
    PGW_ENOTEMPTY,
    PGW_ELOOP,
    PGW_ENOMSG,
    PGW_EOVERFLOW,
    PGW_EILSEQ,
    PGW_ENOTSOCK,
    PGW_EMSGSIZE,
    PGW_EOPNOTSUPP,
    PGW_EAFNOSUPPORT,
    PGW_EADDRINUSE,
    PGW_EADDRNOTAVAIL,
    PGW_ENETDOWN,
    PGW_ENETUNREACH,
    PGW_ENETRESET,
    PGW_ECONNABORTED,
    PGW_ECONNRESET,
    PGW_ENOBUFS,
    PGW_EISCONN,
    PGW_ENOTCONN,
    PGW_ESHUTDOWN,
    PGW_ETOOMANYREFS,
    PGW_ETIMEDOUT,
    PGW_ECONNREFUSED,
    PGW_EHOSTDOWN,
    PGW_EHOSTUNREACH,
    PGW_EALREADY,
    PGW_EINPROGRESS,
    PGW_ESTALE,
    PGW_EDQUOT,
    PGW_ECANCELED,
    PGW_EOWNERDEAD,
    PGW_ENOTRECOVERABLE,
    PGW_ERR__MAX
} pgw_err;

/* --- errno ---------------------------------------------------------------- */

/* Linux (wire) number for a code, 0 if the code is unknown. */
uint32_t pgw_linux_errno(pgw_err e);

/* Code for a Linux (wire) number. Unknown numbers map to PGW_EIO, never to
 * success: an unrecognised error must stay an error (ROB-01). */
pgw_err pgw_code_from_linux(uint32_t linux_errno);

/* errno value of this build (QNX on target, host on PC), 0 if unknown. */
int pgw_host_errno(pgw_err e);

/* Code for this build's errno. Unknown values map to PGW_EIO. */
pgw_err pgw_code_from_host(int host_errno);

/* Human-readable name of a code ("ENOENT"), never NULL. */
const char *pgw_err_name(pgw_err e);

/* --- open flags (P9-04) ---------------------------------------------------- */

/* Bit flags understood by the translation tables. Deliberately our own bits,
 * not those of any OS, so both directions are total functions. */
#define PGW_O_RDONLY   0x00000001u
#define PGW_O_WRONLY   0x00000002u
#define PGW_O_RDWR     0x00000004u
#define PGW_O_CREAT    0x00000008u
#define PGW_O_EXCL     0x00000010u
#define PGW_O_NOCTTY   0x00000020u
#define PGW_O_TRUNC    0x00000040u
#define PGW_O_APPEND   0x00000080u
#define PGW_O_NONBLOCK 0x00000100u
#define PGW_O_DSYNC    0x00000200u
#define PGW_O_SYNC     0x00000400u
#define PGW_O_DIRECT   0x00000800u
#define PGW_O_DIRECTORY 0x00001000u
#define PGW_O_NOFOLLOW 0x00002000u
#define PGW_O_NOATIME  0x00004000u
#define PGW_O_CLOEXEC  0x00008000u
#define PGW_O_LARGEFILE 0x00010000u
#define PGW_O_PATH     0x00020000u
#define PGW_O_TMPFILE  0x00040000u

/* Our bits -> Linux bits (wire) and -> this build's bits (host/QNX). */
uint32_t pgw_linux_oflags(uint32_t pgw_flags);
uint32_t pgw_host_oflags(uint32_t pgw_flags);

/* This build's bits -> our bits, and Linux bits -> our bits. Unknown bits are
 * dropped, which is documented behaviour: an option we cannot honour must not
 * silently change the meaning of the open. */
uint32_t pgw_flags_from_host(uint32_t host_flags);
uint32_t pgw_flags_from_linux(uint32_t linux_flags);

/* --- file type and permission bits ---------------------------------------- */

/* Our own type tags, so mode translation does not depend on S_IF* values. */
typedef enum pgw_ftype {
    PGW_FT_UNKNOWN = 0,
    PGW_FT_FIFO,
    PGW_FT_CHR,
    PGW_FT_DIR,
    PGW_FT_BLK,
    PGW_FT_REG,
    PGW_FT_LNK,
    PGW_FT_SOCK
} pgw_ftype;

pgw_ftype pgw_ftype_from_linux_mode(uint32_t linux_mode);
pgw_ftype pgw_ftype_from_host_mode(uint32_t host_mode);
uint32_t pgw_linux_mode(pgw_ftype t, uint32_t perm_bits); /* 0777 mask kept */
uint32_t pgw_host_mode(pgw_ftype t, uint32_t perm_bits);

/* Permission bits are the same in Linux, QNX and POSIX (07777 plus
 * setuid/setgid/sticky); the helpers exist so that no code hard-codes them. */
#define PGW_S_ISUID 04000u
#define PGW_S_ISGID 02000u
#define PGW_S_ISVTX 01000u
#define PGW_S_IRWXU 00700u
#define PGW_S_IRWXG 00070u
#define PGW_S_IRWXO 00007u
#define PGW_S_PERM_MASK 07777u

/* --- dirent types (FS-03) -------------------------------------------------- */

/* The `d_type` byte of a 9P2000.L readdir entry / FUSE dirent is the Linux
 * DT_* value. These are the wire numbers, not host numbers. */
#define PGW_DT_UNKNOWN 0u
#define PGW_DT_FIFO    1u
#define PGW_DT_CHR     2u
#define PGW_DT_DIR     4u
#define PGW_DT_BLK     6u
#define PGW_DT_REG     8u
#define PGW_DT_LNK     10u
#define PGW_DT_SOCK    12u

uint8_t pgw_dt_from_ftype(pgw_ftype t);
pgw_ftype pgw_ftype_from_dt(uint8_t dt);

#ifdef __cplusplus
}
#endif

#endif /* PGW_ERR_H */
