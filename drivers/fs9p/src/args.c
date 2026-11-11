/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/args.c - command line of the 9P driver.
 *
 * Portable: no QNX header, so the PC build compiles and tests it. The grammar is
 * the one mount(8) hands over on QNX 8.0 and the one the reference client uses:
 *
 *   fs9p [-o opt[,opt...]] mount_tag mount_point
 *
 * `mount_tag` selects the device (VIO-05) and `mount_point` is the path the
 * resource manager takes over (QNX-01). Every option is validated by the shared
 * parser (pgw/drvopts.h), so a typo is an error rather than a silent change.
 *
 * Covered requirements: QNX-01, QNX-07, VIO-05, P9-01, FS-13, ROB-05, SEC-01.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvopts.h"
#include "pgw/err.h"
#include "pgw/fs9p.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <string.h>

/* A mount point must be absolute, must not be the root and must not keep a
 * trailing slash: symlink redirects are built from it (FS-08). */
static pgw_err normalize_mount_point(char *path)
{
    size_t n = strnlen(path, PGW_PATH_MAX);
    if (n == 0 || n >= PGW_PATH_MAX)
        return PGW_ENAMETOOLONG;
    while (n > 1u && path[n - 1u] == '/')
        path[--n] = '\0';
    if (path[0] != '/' || n < 2u)
        return PGW_EINVAL;
    return PGW_OK;
}

/* The one option the 9P driver has that virtiofs does not. */
static bool fs9p_opt_hook(pgw_mount_opts *opts, const char *key, size_t key_len,
                          const char *val, size_t val_len, pgw_err *rc)
{
    if (pgw_opts_key_is(key, key_len, "noatime")) {
        /* Accepted and ignored: 9P2000.L always reports the host's times, and
         * the driver never writes an atime of its own (FS-12). Accepting it
         * keeps a shared option list usable by both drivers. */
        *rc = (val != NULL && val_len != 0) ? PGW_EINVAL : PGW_OK;
        (void)opts;
        return true;
    }
    return false;
}

pgw_err fs9p_parse_args(int argc, char *argv[], pgw_mount_opts *opts,
                        char *mount_point, size_t cap)
{
    if (argv == NULL || opts == NULL || mount_point == NULL || cap == 0u)
        return PGW_EINVAL;
    pgw_opts_default(opts);

    /* The option list may come as one argument after -o, or as a single string
     * the caller already split; both shapes appear in mount(8) scripts. */
    int pos = 1;
    while (pos < argc && argv[pos] != NULL && argv[pos][0] == '-') {
        if (strcmp(argv[pos], "-o") == 0) {
            if (pos + 1 >= argc || argv[pos + 1] == NULL)
                return PGW_EINVAL;
            pgw_err e = pgw_opts_parse(argv[pos + 1], opts, fs9p_opt_hook);
            if (e != PGW_OK)
                return e;
            pos += 2;
        } else {
            return PGW_EINVAL; /* unknown switch */
        }
    }
    if (pos + 2 != argc || argv[pos] == NULL || argv[pos + 1] == NULL)
        return PGW_EINVAL;

    if (opts->tag[0] == '\0') {
        size_t n = strnlen(argv[pos], sizeof opts->tag);
        if (n == 0 || n >= sizeof opts->tag)
            return PGW_EINVAL;
        memcpy(opts->tag, argv[pos], n);
        opts->tag[n] = '\0';
    }
    size_t mp = strnlen(argv[pos + 1], cap);
    if (mp == 0 || mp >= cap)
        return PGW_ENAMETOOLONG;
    memcpy(mount_point, argv[pos + 1], mp);
    mount_point[mp] = '\0';
    pgw_err e = normalize_mount_point(mount_point);
    if (e != PGW_OK)
        return e;
    opts->mount_point = mount_point;
    return PGW_OK;
}
