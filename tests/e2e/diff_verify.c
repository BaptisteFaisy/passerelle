/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * diff_verify.c - compare the view of the layer under test with the truth.
 *
 * Every comparison reads the real tree with plain POSIX calls and the reported
 * tree through pgw_rmfs. Nothing is cached on either side, so a mismatch is a
 * genuine divergence and not a stale artifact (this is also what exercises
 * FS-12: a change made "on the PC" must be visible on the next read).
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#include "diff.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERIFY_BUF 65536
#define PATH_BUF 4096

/* --- deterministic content ------------------------------------------------ */

uint64_t diff_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t diff_hash_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

void diff_fill(void *buf, size_t len, uint32_t seed, uint64_t off)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t state = seed ^ 0x5DEECE66u;
    for (size_t i = 0; i < len; i++) {
        state = state * 1103515245u + 12345u + (uint32_t)(off + i);
        p[i] = (uint8_t)((state >> 16) & 0xFFu);
    }
}

/* --- helpers -------------------------------------------------------------- */

static void cred_root(pgw_fs_cred *c)
{
    memset(c, 0, sizeof(*c));
    c->uid = 0;
    c->gid = 0;
    c->prio = 10;
}

static bool join2(const char *root, const char *rel, char *out, size_t cap)
{
    if (rel == NULL || rel[0] == '\0')
        return snprintf(out, cap, "%s", root) < (int)cap;
    return snprintf(out, cap, "%s/%s", root, rel) < (int)cap;
}

static uint64_t hash_file_posix(const char *path, char *err, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, cap, "truth open(%s): %s", path, strerror(errno));
        return 0;
    }
    static uint8_t buf[VERIFY_BUF];
    uint64_t h = 1469598103934665603ULL;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        h = diff_hash_mix(h, diff_hash(buf, (size_t)n));
    if (n < 0)
        snprintf(err, cap, "truth read(%s): %s", path, strerror(errno));
    close(fd);
    return h;
}

static uint64_t hash_file_rmfs(pgw_rmfs *fs, const char *rel, char *err,
                               size_t cap)
{
    pgw_fs_cred cred;
    cred_root(&cred);
    pgw_rmfs_fd *fd = NULL;
    pgw_err e = pgw_rmfs_open(fs, &cred, rel, PGW_O_RDONLY, 0, &fd);
    if (e != PGW_OK || fd == NULL) {
        snprintf(err, cap, "rmfs open(%s): %s", rel, pgw_err_name(e));
        return 0;
    }
    static uint8_t buf[VERIFY_BUF];
    uint64_t h = 1469598103934665603ULL;
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        e = pgw_rmfs_read(fs, fd, off, buf, sizeof(buf), &got);
        if (e != PGW_OK) {
            snprintf(err, cap, "rmfs read(%s@%llu): %s", rel,
                     (unsigned long long)off, pgw_err_name(e));
            h = 0;
            break;
        }
        if (got == 0)
            break;
        h = diff_hash_mix(h, diff_hash(buf, got));
        off += got;
    }
    pgw_rmfs_close(fs, fd);
    return h;
}

/* --- one path ------------------------------------------------------------- */

static int verify_attrs(pgw_rmfs *fs, const char *rel, const struct stat *truth,
                        char *err, size_t cap)
{
    pgw_fs_cred cred;
    cred_root(&cred);
    pgw_fs_attr got;
    pgw_err e = pgw_rmfs_lstat(fs, &cred, rel, &got);
    if (e != PGW_OK) {
        snprintf(err, cap, "%s: rmfs lstat failed: %s", rel, pgw_err_name(e));
        return -1;
    }
    pgw_ftype want = pgw_ftype_from_host_mode((uint32_t)truth->st_mode);
    if (got.type != want) {
        snprintf(err, cap, "%s: type %d != truth %d", rel, (int)got.type,
                 (int)want);
        return -1;
    }
    if ((uint64_t)truth->st_size != got.size && want == PGW_FT_REG) {
        snprintf(err, cap, "%s: size %llu != truth %lld", rel,
                 (unsigned long long)got.size, (long long)truth->st_size);
        return -1;
    }
    uint32_t want_mode = (uint32_t)truth->st_mode & PGW_S_PERM_MASK;
    if (got.mode != want_mode) {
        snprintf(err, cap, "%s: mode %04o != truth %04o", rel,
                 (unsigned)got.mode, (unsigned)want_mode);
        return -1;
    }
    return 0;
}

static int verify_symlink(pgw_rmfs *fs, const char *rel, const char *truth_path,
                          char *err, size_t cap)
{
    char want[PATH_BUF], got[PATH_BUF];
    ssize_t n = readlink(truth_path, want, sizeof(want) - 1);
    if (n < 0) {
        snprintf(err, cap, "%s: truth readlink: %s", rel, strerror(errno));
        return -1;
    }
    want[n] = '\0';
    pgw_fs_cred cred;
    cred_root(&cred);
    pgw_err e = pgw_rmfs_readlink(fs, &cred, rel, got, sizeof(got));
    if (e != PGW_OK) {
        snprintf(err, cap, "%s: rmfs readlink: %s", rel, pgw_err_name(e));
        return -1;
    }
    if (strcmp(want, got) != 0) {
        snprintf(err, cap, "%s: target \"%s\" != truth \"%s\"", rel, got, want);
        return -1;
    }
    return 0;
}

int diff_verify_path(pgw_rmfs *fs, const char *root, const char *rel,
                     char *err, size_t err_cap)
{
    char truth_path[PATH_BUF];
    if (!join2(root, rel, truth_path, sizeof(truth_path))) {
        snprintf(err, err_cap, "%s: path too long", rel);
        return -1;
    }
    struct stat st;
    if (lstat(truth_path, &st) != 0) {
        int saved = errno;
        pgw_fs_cred cred;
        cred_root(&cred);
        pgw_fs_attr got;
        pgw_err e = pgw_rmfs_lstat(fs, &cred, rel, &got);
        pgw_err want = pgw_code_from_host(saved);
        if (e == want)
            return 0; /* same refusal, or both say "absent" */
        if (saved == ENOENT) {
            snprintf(err, err_cap,
                     "%s: absent on the PC but the share reports it (%s)", rel,
                     pgw_err_name(e));
        } else {
            /* The PC refuses the lookup itself - typically a parent directory
             * whose search bit a previous chmod removed. The share must refuse
             * it the same way: neither inventing the entry nor claiming it is
             * missing. */
            snprintf(err, err_cap,
                     "%s: the PC refuses the lookup with %s, the share answers "
                     "%s", rel, pgw_err_name(want), pgw_err_name(e));
        }
        return -1;
    }
    if (verify_attrs(fs, rel, &st, err, err_cap) != 0)
        return -1;
    if (S_ISLNK(st.st_mode))
        return verify_symlink(fs, rel, truth_path, err, err_cap);
    if (!S_ISREG(st.st_mode))
        return 0;
    uint64_t a = hash_file_posix(truth_path, err, err_cap);
    uint64_t b = hash_file_rmfs(fs, rel, err, err_cap);
    if (a == 0 || b == 0)
        return (a == 0 && b == 0) ? 0 : -1;
    if (a != b) {
        snprintf(err, err_cap, "%s: content digest %016llx != truth %016llx",
                 rel, (unsigned long long)b, (unsigned long long)a);
        return -1;
    }
    return 0;
}
