/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * diff_ops.c - seeded operation suites and the differential runner.
 *
 * A suite is a plain array of operations generated from a seed, so any failure
 * can be replayed byte for byte and then reduced (diff_reduce) to the shortest
 * prefix that still diverges. Operations are executed through pgw_rmfs while
 * verification reads the PC directory directly: the folder is the truth
 * (spec 7.2). Operation failures are tolerated - creating a file that already
 * exists, renaming a missing entry - because what matters is that the resulting
 * *state* matches the truth, exactly as it would in a guest.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#include "diff.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IO_BUF 8192
#define PATH_BUF 4096
#define N_DIRS 4

static const char *const g_kind_names[OP_KIND_MAX] = {
    "create", "write", "read", "truncate", "mkdir", "rmdir", "symlink",
    "link", "rename", "unlink", "chmod", "readdir", "stat"
};

const char *diff_kind_name(diff_op_kind k)
{
    return (k >= 0 && k < OP_KIND_MAX) ? g_kind_names[k] : "?";
}

void diff_path(uint32_t suite_seed, unsigned i, char *out, size_t cap)
{
    (void)suite_seed; /* stable names keep a reduced case readable */
    if (i < N_DIRS)
        snprintf(out, cap, "d%u", i);
    else
        snprintf(out, cap, "d%u/f%u", i % N_DIRS, i);
}

/* --- generation ----------------------------------------------------------- */

static uint32_t rng_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void diff_gen(diff_op *ops, size_t n, uint32_t seed)
{
    uint32_t st = seed | 1u;
    for (size_t i = 0; i < n; i++) {
        diff_op *o = &ops[i];
        o->kind = (diff_op_kind)(rng_next(&st) % (uint32_t)OP_KIND_MAX);
        o->a = (uint8_t)(rng_next(&st) % PGW_DIFF_PATHS);
        o->b = (uint8_t)(rng_next(&st) % PGW_DIFF_PATHS);
        o->off = rng_next(&st) % 8192u;
        o->len = 1u + rng_next(&st) % 4096u;
        o->mode = 0600u | (rng_next(&st) & 077u);
        o->seed = rng_next(&st);
    }
}

/* --- execution ------------------------------------------------------------ */

static void cred_root(pgw_fs_cred *c)
{
    memset(c, 0, sizeof(*c));
    c->prio = 10;
}

static pgw_err rm_open(pgw_rmfs *fs, const char *p, uint32_t flags,
                       uint32_t mode, pgw_rmfs_fd **fd)
{
    pgw_fs_cred c;
    cred_root(&c);
    return pgw_rmfs_open(fs, &c, p, flags, mode, fd);
}

static void op_create(pgw_rmfs *fs, const char *p, uint32_t mode)
{
    pgw_rmfs_fd *fd = NULL;
    if (rm_open(fs, p, PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, mode, &fd) ==
        PGW_OK)
        pgw_rmfs_close(fs, fd);
}

static void op_write(pgw_rmfs *fs, const char *p, const diff_op *o)
{
    pgw_rmfs_fd *fd = NULL;
    if (rm_open(fs, p, PGW_O_RDWR | PGW_O_CREAT, o->mode, &fd) != PGW_OK)
        return;
    uint8_t buf[IO_BUF];
    size_t len = o->len < sizeof(buf) ? o->len : sizeof(buf);
    diff_fill(buf, len, o->seed, o->off);
    size_t done = 0;
    pgw_rmfs_write(fs, fd, o->off, buf, len, &done);
    pgw_rmfs_close(fs, fd);
}

/* Read through the share and compare with the same range read from the PC. */
static void op_read(pgw_rmfs *fs, const char *root, const char *p,
                    const diff_op *o, char *err, size_t cap)
{
    pgw_rmfs_fd *fd = NULL;
    if (rm_open(fs, p, PGW_O_RDONLY, 0, &fd) != PGW_OK)
        return; /* absent: the state comparison decides */
    uint8_t got[IO_BUF];
    size_t len = o->len < sizeof(got) ? o->len : sizeof(got);
    size_t n = 0;
    pgw_err e = pgw_rmfs_read(fs, fd, o->off, got, len, &n);
    pgw_rmfs_close(fs, fd);
    if (e != PGW_OK || n == 0)
        return;
    char abs[PATH_BUF];
    snprintf(abs, sizeof(abs), "%s/%s", root, p);
    FILE *f = fopen(abs, "rb");
    if (f == NULL) {
        snprintf(err, cap, "%s: share served data, no truth file", p);
        return;
    }
    uint8_t want[IO_BUF];
    size_t m = (fseek(f, (long)o->off, SEEK_SET) == 0) ? fread(want, 1, n, f) : 0;
    fclose(f);
    if (m != n || memcmp(got, want, n) != 0)
        snprintf(err, cap, "%s: read@%u differs from the truth", p, o->off);
}

static void op_namespace(pgw_rmfs *fs, const char *a, const char *b,
                         const diff_op *o)
{
    pgw_fs_cred c;
    cred_root(&c);
    switch (o->kind) {
    case OP_MKDIR:   pgw_rmfs_mkdir(fs, &c, a, o->mode); break;
    case OP_RMDIR:   pgw_rmfs_unlink(fs, &c, a, true); break;
    case OP_UNLINK:  pgw_rmfs_unlink(fs, &c, a, false); break;
    case OP_RENAME:  pgw_rmfs_rename(fs, &c, a, b); break;
    case OP_SYMLINK: pgw_rmfs_symlink(fs, &c, b, a); break;
    case OP_LINK:    pgw_rmfs_link(fs, &c, a, b); break;
    case OP_CHMOD:   pgw_rmfs_chmod(fs, &c, a, o->mode); break;
    case OP_TRUNC:   pgw_rmfs_truncate(fs, &c, a, o->off); break;
    default: break;
    }
}

static void op_inspect(pgw_rmfs *fs, const char *a, const diff_op *o)
{
    pgw_fs_cred c;
    cred_root(&c);
    pgw_fs_attr at;
    pgw_fs_statvfs vfs;
    pgw_rmfs_fd *fd = NULL;
    if (o->kind == OP_STAT) {
        pgw_rmfs_stat(fs, &c, a, &at);
    } else if (rm_open(fs, a, PGW_O_RDONLY | PGW_O_DIRECTORY, 0, &fd) == PGW_OK) {
        pgw_fs_dirent ents[32];
        size_t got = 0;
        uint64_t next = 0;
        pgw_rmfs_readdir(fs, fd, 0, ents, 32, &got, &next);
        pgw_rmfs_close(fs, fd);
    } else {
        pgw_rmfs_statfs(fs, &c, &vfs);
    }
}

static void exec_op(pgw_rmfs *fs, const diff_op *o, uint32_t seed,
                    const char *root, char *err, size_t cap)
{
    char a[64], b[64];
    diff_path(seed, o->a, a, sizeof(a));
    diff_path(seed, o->b, b, sizeof(b));
    err[0] = '\0';
    switch (o->kind) {
    case OP_CREATE:  op_create(fs, a, o->mode); break;
    case OP_WRITE:   op_write(fs, a, o); break;
    case OP_READ:    op_read(fs, root, a, o, err, cap); break;
    case OP_READDIR:
    case OP_STAT:    op_inspect(fs, a, o); break;
    default:         op_namespace(fs, a, b, o); break;
    }
}

/* --- one run -------------------------------------------------------------- */

/* Verify the two paths an operation touched: either both sides agree, or both
 * sides report the entry as absent. */
static long check_paths(pgw_rmfs *fs, const char *root, const char *a,
                        const char *b, char *err, size_t cap)
{
    const char *paths[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        if (diff_verify_path(fs, root, paths[i], err, cap) != 0)
            return -1;
    }
    return 0;
}

/* Create the four directories of the pool, then replay the suite, checking the
 * two paths each operation touched. Returns the index of the first divergence,
 * `n` when only the final tree comparison disagrees, or -1 when clean. */
static long run_setup_and_ops(pgw_rmfs *fs, const diff_op *ops, size_t n,
                              uint32_t seed, const char *root, char *err,
                              size_t err_cap)
{
    pgw_fs_cred c;
    cred_root(&c);
    for (unsigned i = 0; i < N_DIRS; i++) {
        char d[16];
        diff_path(seed, i, d, sizeof(d));
        pgw_rmfs_mkdir(fs, &c, d, 0755u);
    }
    for (size_t i = 0; i < n; i++) {
        exec_op(fs, &ops[i], seed, root, err, err_cap);
        if (err[0] != '\0')
            return (long)i;
        char a[64], b[64];
        diff_path(seed, ops[i].a, a, sizeof(a));
        diff_path(seed, ops[i].b, b, sizeof(b));
        if (check_paths(fs, root, a, b, err, err_cap) != 0)
            return (long)i;
    }
    if (diff_verify_tree(fs, root, err, err_cap) != 0)
        return (long)n;
    return -1;
}

long diff_run(const diff_op *ops, size_t n, uint32_t seed, char *err,
              size_t err_cap)
{
    err[0] = '\0';
    char *root = pgw_test_tmpdir("pgwdiff");
    if (root == NULL) {
        snprintf(err, err_cap, "no temporary directory available");
        return -2;
    }
    pgw_posix_backend *be = pgw_posix_backend_open(root);
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    pgw_rmfs *fs = (be != NULL) ? pgw_rmfs_create(&pgw_posix_backend_ops, be,
                                                  &opts)
                                : NULL;
    if (fs == NULL) {
        pgw_posix_backend_close(be);
        snprintf(err, err_cap, "cannot create the backend or the filesystem");
        return -2;
    }
    long failed_at = run_setup_and_ops(fs, ops, n, seed, root, err, err_cap);
    size_t leaked = pgw_posix_backend_open_handles(be);
    pgw_rmfs_destroy(fs); /* closes the backend through ops->destroy */
    if (failed_at < 0 && leaked != 0) {
        snprintf(err, err_cap, "%zu handle(s) left open at the end (ROB-02)",
                 leaked);
        failed_at = (long)n;
    }
    return failed_at;
}

long diff_reduce(const diff_op *ops, size_t n, uint32_t seed, char *err,
                 size_t err_cap)
{
    long lo = 1, hi = (long)n;
    char tmp[PGW_DIFF_MSG];
    if (diff_run(ops, (size_t)hi, seed, tmp, sizeof(tmp)) < 0)
        return -1; /* does not reproduce: nothing to reduce */
    while (lo < hi) {
        long mid = (lo + hi) / 2;
        if (diff_run(ops, (size_t)mid, seed, tmp, sizeof(tmp)) >= 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    /* The binary search ends on a run that may have been the clean one; replay
     * the minimal prefix so the reported message describes the real failure. */
    if (diff_run(ops, (size_t)lo, seed, err, err_cap) < 0)
        snprintf(err, err_cap, "%s", tmp);
    return lo;
}
