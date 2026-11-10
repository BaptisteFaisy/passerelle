/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/src/err.c - errno, open-flag and file-mode translation tables.
 *
 * The Linux (wire) numbers are from asm-generic/errno-base.h and
 * asm-generic/errno.h (identical across x86_64 and aarch64). The host/QNX side
 * compiles against the target's real <errno.h>, <fcntl.h>, <sys/stat.h>.
 */
#define _POSIX_C_SOURCE 200809L
#include "pgw/err.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>

typedef struct err_entry {
    pgw_err code;
    uint32_t linux_no;
    int host_no;
    const char *name;
} err_entry;

/* Helper macro: only register a host_no if the target defines the POSIX macro.
 * Everything in POSIX.1-2008 is present on both Linux and QNX 8.0. */
#define ERR_E(id, lno, hmacro) { PGW_##id, (lno), (hmacro), #id }

static const err_entry g_err_tab[] = {
    { PGW_OK, 0, 0, "OK" },
    ERR_E(EPERM, 1, EPERM),
    ERR_E(ENOENT, 2, ENOENT),
    ERR_E(ESRCH, 3, ESRCH),
    ERR_E(EINTR, 4, EINTR),
    ERR_E(EIO, 5, EIO),
    ERR_E(ENXIO, 6, ENXIO),
    ERR_E(E2BIG, 7, E2BIG),
    ERR_E(ENOEXEC, 8, ENOEXEC),
    ERR_E(EBADF, 9, EBADF),
    ERR_E(ECHILD, 10, ECHILD),
    ERR_E(EAGAIN, 11, EAGAIN),
    ERR_E(ENOMEM, 12, ENOMEM),
    ERR_E(EACCES, 13, EACCES),
    ERR_E(EFAULT, 14, EFAULT),
    ERR_E(ENOTBLK, 15, ENOTBLK),
    ERR_E(EBUSY, 16, EBUSY),
    ERR_E(EEXIST, 17, EEXIST),
    ERR_E(EXDEV, 18, EXDEV),
    ERR_E(ENODEV, 19, ENODEV),
    ERR_E(ENOTDIR, 20, ENOTDIR),
    ERR_E(EISDIR, 21, EISDIR),
    ERR_E(EINVAL, 22, EINVAL),
    ERR_E(ENFILE, 23, ENFILE),
    ERR_E(EMFILE, 24, EMFILE),
    ERR_E(ENOTTY, 25, ENOTTY),
    ERR_E(ETXTBSY, 26, ETXTBSY),
    ERR_E(EFBIG, 27, EFBIG),
    ERR_E(ENOSPC, 28, ENOSPC),
    ERR_E(ESPIPE, 29, ESPIPE),
    ERR_E(EROFS, 30, EROFS),
    ERR_E(EMLINK, 31, EMLINK),
    ERR_E(EPIPE, 32, EPIPE),
    ERR_E(EDOM, 33, EDOM),
    ERR_E(ERANGE, 34, ERANGE),
    ERR_E(EDEADLK, 35, EDEADLK),
    ERR_E(ENAMETOOLONG, 36, ENAMETOOLONG),
    ERR_E(ENOLCK, 37, ENOLCK),
    ERR_E(ENOSYS, 38, ENOSYS),
    ERR_E(ENOTEMPTY, 39, ENOTEMPTY),
    ERR_E(ELOOP, 40, ELOOP),
    ERR_E(ENOMSG, 42, ENOMSG),
    ERR_E(EOVERFLOW, 75, EOVERFLOW),
    ERR_E(EILSEQ, 84, EILSEQ),
    ERR_E(ENOTSOCK, 88, ENOTSOCK),
    ERR_E(EMSGSIZE, 90, EMSGSIZE),
    ERR_E(EOPNOTSUPP, 95, EOPNOTSUPP),
    ERR_E(EAFNOSUPPORT, 97, EAFNOSUPPORT),
    ERR_E(EADDRINUSE, 98, EADDRINUSE),
    ERR_E(EADDRNOTAVAIL, 99, EADDRNOTAVAIL),
    ERR_E(ENETDOWN, 100, ENETDOWN),
    ERR_E(ENETUNREACH, 101, ENETUNREACH),
    ERR_E(ENETRESET, 102, ENETRESET),
    ERR_E(ECONNABORTED, 103, ECONNABORTED),
    ERR_E(ECONNRESET, 104, ECONNRESET),
    ERR_E(ENOBUFS, 105, ENOBUFS),
    ERR_E(EISCONN, 106, EISCONN),
    ERR_E(ENOTCONN, 107, ENOTCONN),
    ERR_E(ESHUTDOWN, 108, ESHUTDOWN),
    ERR_E(ETOOMANYREFS, 109, ETOOMANYREFS),
    ERR_E(ETIMEDOUT, 110, ETIMEDOUT),
    ERR_E(ECONNREFUSED, 111, ECONNREFUSED),
    ERR_E(EHOSTDOWN, 112, EHOSTDOWN),
    ERR_E(EHOSTUNREACH, 113, EHOSTUNREACH),
    ERR_E(EALREADY, 114, EALREADY),
    ERR_E(EINPROGRESS, 115, EINPROGRESS),
    ERR_E(ESTALE, 116, ESTALE),
    ERR_E(EDQUOT, 122, EDQUOT),
    ERR_E(ECANCELED, 125, ECANCELED),
    ERR_E(EOWNERDEAD, 130, EOWNERDEAD),
    ERR_E(ENOTRECOVERABLE, 131, ENOTRECOVERABLE),
};

#define ERR_TAB_LEN (sizeof(g_err_tab) / sizeof(g_err_tab[0]))

uint32_t pgw_linux_errno(pgw_err e)
{
    for (size_t i = 0; i < ERR_TAB_LEN; i++) {
        if (g_err_tab[i].code == e)
            return g_err_tab[i].linux_no;
    }
    return 0;
}

pgw_err pgw_code_from_linux(uint32_t linux_errno)
{
    if (linux_errno == 0)
        return PGW_OK;
    if (linux_errno == 11) /* EWOULDBLOCK == EAGAIN on Linux */
        return PGW_EAGAIN;
    for (size_t i = 0; i < ERR_TAB_LEN; i++) {
        if (g_err_tab[i].linux_no == linux_errno)
            return g_err_tab[i].code;
    }
    return PGW_EIO;
}

int pgw_host_errno(pgw_err e)
{
    for (size_t i = 0; i < ERR_TAB_LEN; i++) {
        if (g_err_tab[i].code == e)
            return g_err_tab[i].host_no;
    }
    return 0;
}

pgw_err pgw_code_from_host(int host_errno)
{
    if (host_errno == 0)
        return PGW_OK;
#ifdef EWOULDBLOCK
    if (host_errno == EWOULDBLOCK)
        return PGW_EAGAIN;
#endif
    for (size_t i = 0; i < ERR_TAB_LEN; i++) {
        if (g_err_tab[i].host_no == host_errno)
            return g_err_tab[i].code;
    }
    return PGW_EIO;
}

const char *pgw_err_name(pgw_err e)
{
    for (size_t i = 0; i < ERR_TAB_LEN; i++) {
        if (g_err_tab[i].code == e)
            return g_err_tab[i].name;
    }
    return "EUNKNOWN";
}
