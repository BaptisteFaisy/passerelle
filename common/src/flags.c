/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/src/flags.c - open flag, file mode and dirent type translations.
 *
 * Linux O_* values follow the asm-generic/fcntl.h encoding used on wire by
 * 9P2000.L and FUSE. The host/QNX side reads the real <fcntl.h> and
 * <sys/stat.h> constants of the target build.
 */
#define _POSIX_C_SOURCE 200809L
#include "pgw/err.h"

#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>

typedef struct flag_entry {
    uint32_t pgw;
    uint32_t lin;
    uint32_t host;
} flag_entry;

/* Linux asm-generic O_* constants (octal). Note: O_ACCMODE is 3; O_RDONLY=0,
 * O_WRONLY=1, O_RDWR=2 are handled outside the bitmask loop below. */
#define L_O_CREAT     00000100u
#define L_O_EXCL      00000200u
#define L_O_NOCTTY    00000400u
#define L_O_TRUNC     00001000u
#define L_O_APPEND    00002000u
#define L_O_NONBLOCK  00004000u
#define L_O_DSYNC     00010000u
#define L_O_DIRECT    00040000u
#define L_O_LARGEFILE 00100000u
#define L_O_DIRECTORY 00200000u
#define L_O_NOFOLLOW  00400000u
#define L_O_NOATIME   01000000u
#define L_O_CLOEXEC   02000000u
#define L_O_SYNC      04010000u /* __O_SYNC | O_DSYNC on Linux */
#define L_O_PATH      010000000u
#define L_O_TMPFILE   020200000u

#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_PATH
#define O_PATH 0
#endif
#ifndef O_TMPFILE
#define O_TMPFILE 0
#endif

static const flag_entry g_oflag_tab[] = {
    { PGW_O_CREAT, L_O_CREAT, O_CREAT },
    { PGW_O_EXCL, L_O_EXCL, O_EXCL },
    { PGW_O_NOCTTY, L_O_NOCTTY, O_NOCTTY },
    { PGW_O_TRUNC, L_O_TRUNC, O_TRUNC },
    { PGW_O_APPEND, L_O_APPEND, O_APPEND },
    { PGW_O_NONBLOCK, L_O_NONBLOCK, O_NONBLOCK },
    { PGW_O_DSYNC, L_O_DSYNC, O_DSYNC },
    { PGW_O_SYNC, L_O_SYNC, O_SYNC },
    { PGW_O_DIRECT, L_O_DIRECT, O_DIRECT },
    { PGW_O_LARGEFILE, L_O_LARGEFILE, O_LARGEFILE },
    { PGW_O_DIRECTORY, L_O_DIRECTORY, O_DIRECTORY },
    { PGW_O_NOFOLLOW, L_O_NOFOLLOW, O_NOFOLLOW },
    { PGW_O_NOATIME, L_O_NOATIME, O_NOATIME },
    { PGW_O_CLOEXEC, L_O_CLOEXEC, O_CLOEXEC },
    { PGW_O_PATH, L_O_PATH, O_PATH },
    { PGW_O_TMPFILE, L_O_TMPFILE, O_TMPFILE },
};

#define OFLAG_TAB_LEN (sizeof(g_oflag_tab) / sizeof(g_oflag_tab[0]))

uint32_t pgw_linux_oflags(uint32_t pgw_flags)
{
    uint32_t out = 0;
    if (pgw_flags & PGW_O_RDWR)
        out |= 2u;
    else if (pgw_flags & PGW_O_WRONLY)
        out |= 1u;
    for (size_t i = 0; i < OFLAG_TAB_LEN; i++) {
        if (pgw_flags & g_oflag_tab[i].pgw)
            out |= g_oflag_tab[i].lin;
    }
    return out;
}

uint32_t pgw_host_oflags(uint32_t pgw_flags)
{
    uint32_t out = O_RDONLY;
    if (pgw_flags & PGW_O_RDWR)
        out = O_RDWR;
    else if (pgw_flags & PGW_O_WRONLY)
        out = O_WRONLY;
    for (size_t i = 0; i < OFLAG_TAB_LEN; i++) {
        if ((pgw_flags & g_oflag_tab[i].pgw) && g_oflag_tab[i].host != 0)
            out |= g_oflag_tab[i].host;
    }
    return out;
}

uint32_t pgw_flags_from_host(uint32_t host_flags)
{
    uint32_t out = 0;
    uint32_t acc = host_flags & (uint32_t)O_ACCMODE;
    if (acc == (uint32_t)O_RDWR)
        out |= PGW_O_RDWR;
    else if (acc == (uint32_t)O_WRONLY)
        out |= PGW_O_WRONLY;
    else
        out |= PGW_O_RDONLY;
    for (size_t i = 0; i < OFLAG_TAB_LEN; i++) {
        uint32_t h = g_oflag_tab[i].host;
        if (h != 0 && (host_flags & h) == h)
            out |= g_oflag_tab[i].pgw;
    }
    return out;
}

uint32_t pgw_flags_from_linux(uint32_t linux_flags)
{
    uint32_t out = 0;
    uint32_t acc = linux_flags & 3u;
    if (acc == 2u)
        out |= PGW_O_RDWR;
    else if (acc == 1u)
        out |= PGW_O_WRONLY;
    else
        out |= PGW_O_RDONLY;
    for (size_t i = 0; i < OFLAG_TAB_LEN; i++) {
        uint32_t l = g_oflag_tab[i].lin;
        if ((linux_flags & l) == l)
            out |= g_oflag_tab[i].pgw;
    }
    return out;
}

/* Linux/POSIX S_IFMT type bits (0170000). */
#define L_S_IFMT   0170000u
#define L_S_IFIFO  0010000u
#define L_S_IFCHR  0020000u
#define L_S_IFDIR  0040000u
#define L_S_IFBLK  0060000u
#define L_S_IFREG  0100000u
#define L_S_IFLNK  0120000u
#define L_S_IFSOCK 0140000u

pgw_ftype pgw_ftype_from_linux_mode(uint32_t m)
{
    switch (m & L_S_IFMT) {
    case L_S_IFIFO:  return PGW_FT_FIFO;
    case L_S_IFCHR:  return PGW_FT_CHR;
    case L_S_IFDIR:  return PGW_FT_DIR;
    case L_S_IFBLK:  return PGW_FT_BLK;
    case L_S_IFREG:  return PGW_FT_REG;
    case L_S_IFLNK:  return PGW_FT_LNK;
    case L_S_IFSOCK: return PGW_FT_SOCK;
    default:         return PGW_FT_UNKNOWN;
    }
}

pgw_ftype pgw_ftype_from_host_mode(uint32_t m)
{
    switch (m & (uint32_t)S_IFMT) {
    case S_IFIFO:  return PGW_FT_FIFO;
    case S_IFCHR:  return PGW_FT_CHR;
    case S_IFDIR:  return PGW_FT_DIR;
    case S_IFBLK:  return PGW_FT_BLK;
    case S_IFREG:  return PGW_FT_REG;
    case S_IFLNK:  return PGW_FT_LNK;
    case S_IFSOCK: return PGW_FT_SOCK;
    default:       return PGW_FT_UNKNOWN;
    }
}

uint32_t pgw_linux_mode(pgw_ftype t, uint32_t perm)
{
    uint32_t p = perm & PGW_S_PERM_MASK;
    switch (t) {
    case PGW_FT_FIFO: return L_S_IFIFO | p;
    case PGW_FT_CHR:  return L_S_IFCHR | p;
    case PGW_FT_DIR:  return L_S_IFDIR | p;
    case PGW_FT_BLK:  return L_S_IFBLK | p;
    case PGW_FT_REG:  return L_S_IFREG | p;
    case PGW_FT_LNK:  return L_S_IFLNK | p;
    case PGW_FT_SOCK: return L_S_IFSOCK | p;
    default:          return p;
    }
}

uint32_t pgw_host_mode(pgw_ftype t, uint32_t perm)
{
    uint32_t p = perm & PGW_S_PERM_MASK;
    switch (t) {
    case PGW_FT_FIFO: return (uint32_t)S_IFIFO | p;
    case PGW_FT_CHR:  return (uint32_t)S_IFCHR | p;
    case PGW_FT_DIR:  return (uint32_t)S_IFDIR | p;
    case PGW_FT_BLK:  return (uint32_t)S_IFBLK | p;
    case PGW_FT_REG:  return (uint32_t)S_IFREG | p;
    case PGW_FT_LNK:  return (uint32_t)S_IFLNK | p;
    case PGW_FT_SOCK: return (uint32_t)S_IFSOCK | p;
    default:          return p;
    }
}

uint8_t pgw_dt_from_ftype(pgw_ftype t)
{
    switch (t) {
    case PGW_FT_FIFO: return PGW_DT_FIFO;
    case PGW_FT_CHR:  return PGW_DT_CHR;
    case PGW_FT_DIR:  return PGW_DT_DIR;
    case PGW_FT_BLK:  return PGW_DT_BLK;
    case PGW_FT_REG:  return PGW_DT_REG;
    case PGW_FT_LNK:  return PGW_DT_LNK;
    case PGW_FT_SOCK: return PGW_DT_SOCK;
    default:          return PGW_DT_UNKNOWN;
    }
}

pgw_ftype pgw_ftype_from_dt(uint8_t dt)
{
    switch (dt) {
    case PGW_DT_FIFO: return PGW_FT_FIFO;
    case PGW_DT_CHR:  return PGW_FT_CHR;
    case PGW_DT_DIR:  return PGW_FT_DIR;
    case PGW_DT_BLK:  return PGW_FT_BLK;
    case PGW_DT_REG:  return PGW_FT_REG;
    case PGW_DT_LNK:  return PGW_FT_LNK;
    case PGW_DT_SOCK: return PGW_FT_SOCK;
    default:          return PGW_FT_UNKNOWN;
    }
}
