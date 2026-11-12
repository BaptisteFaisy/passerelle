/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_common.c - the shared fixture of the directed cases (spec 7.4).
 *
 * Implements cas.h: the counting backend decorator (every callback forwards
 * to the wrapped POSIX backend and bumps a counter, which is how a case proves
 * that fsync really reached the backend, FS-10), the mount fixture, the truth
 * side (plain POSIX calls on the shared directory) and the two host helpers
 * the load and durability cases need (open-descriptor count, page-cache drop).
 *
 * Covers PERF-04, PERF-05, FS-10, FS-12, FS-15, ROB-03.
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"

#include "test_cred.h"

#include "pgw/err.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

uint64_t cas_fake_clock(void *user)
{
    return *(uint64_t *)user;
}

/* --- the counting backend ------------------------------------------------- */

#define PROBE(impl) ((cas_probe *)(impl))

static void probe_destroy(void *impl)
{
    /* The wrapped backend belongs to the fixture, not to the decorator. */
    (void)impl;
}

static pgw_err probe_lookup(void *impl, const char *rel, pgw_fs_attr *out,
                            uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->lookup_calls++;
    return p->next->lookup(p->next_impl, rel, out, prio);
}

static pgw_err probe_getattr(void *impl, const char *rel, pgw_fs_attr *out,
                             uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->getattr_calls++;
    return p->next->getattr(p->next_impl, rel, out, prio);
}

static pgw_err probe_setattr(void *impl, const char *rel,
                             const pgw_fs_setattr *sa, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->setattr(p->next_impl, rel, sa, prio);
}

static pgw_err probe_open(void *impl, const char *rel, uint32_t oflags,
                          uint32_t mode, uint64_t *out_handle, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->open_calls++;
    return p->next->open(p->next_impl, rel, oflags, mode, out_handle, prio);
}

static pgw_err probe_close(void *impl, uint64_t handle, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->close_calls++;
    return p->next->close(p->next_impl, handle, prio);
}

static pgw_err probe_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                          size_t len, size_t *out_read, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->read_calls++;
    return p->next->read(p->next_impl, handle, off, buf, len, out_read, prio);
}

static pgw_err probe_write(void *impl, uint64_t handle, uint64_t off,
                           const void *buf, size_t len, size_t *out_written,
                           uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->write_calls++;
    return p->next->write(p->next_impl, handle, off, buf, len, out_written,
                          prio);
}

static pgw_err probe_readdir(void *impl, uint64_t handle, uint64_t off,
                             pgw_fs_dirent *out, size_t max_entries,
                             size_t *out_count, uint64_t *next_off,
                             uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->readdir_calls++;
    return p->next->readdir(p->next_impl, handle, off, out, max_entries,
                            out_count, next_off, prio);
}

static pgw_err probe_mkdir(void *impl, const char *rel, uint32_t mode,
                           uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->mkdir(p->next_impl, rel, mode, prio);
}

static pgw_err probe_unlink(void *impl, const char *rel, bool is_dir,
                            uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->unlink(p->next_impl, rel, is_dir, prio);
}

static pgw_err probe_rename(void *impl, const char *oldp, const char *newp,
                            uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->rename(p->next_impl, oldp, newp, prio);
}

static pgw_err probe_symlink(void *impl, const char *linkpath,
                             const char *target, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->symlink(p->next_impl, linkpath, target, prio);
}

static pgw_err probe_readlink(void *impl, const char *linkpath, char *target,
                              size_t cap, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->readlink(p->next_impl, linkpath, target, cap, prio);
}

static pgw_err probe_link(void *impl, const char *existing, const char *newpath,
                          uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->link(p->next_impl, existing, newpath, prio);
}

static pgw_err probe_fsync(void *impl, uint64_t handle, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    p->fsync_calls++;
    return p->next->fsync(p->next_impl, handle, prio);
}

static pgw_err probe_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio)
{
    cas_probe *p = PROBE(impl);
    return p->next->statfs(p->next_impl, out, prio);
}

const pgw_fs_backend_ops cas_probe_ops = {
    .destroy = probe_destroy,
    .lookup = probe_lookup,
    .getattr = probe_getattr,
    .setattr = probe_setattr,
    .open = probe_open,
    .close = probe_close,
    .read = probe_read,
    .write = probe_write,
    .readdir = probe_readdir,
    .mkdir = probe_mkdir,
    .unlink = probe_unlink,
    .rename = probe_rename,
    .symlink = probe_symlink,
    .readlink = probe_readlink,
    .link = probe_link,
    .fsync = probe_fsync,
    .statfs = probe_statfs,
};

/* --- the fixture ---------------------------------------------------------- */

bool cas_mount_open(cas_mount *m, const char *tag, const pgw_fs_opts *opts,
                    cas_probe *probe)
{
    memset(m, 0, sizeof(*m));
    char *root = pgw_test_tmpdir(tag);
    if (root == NULL)
        return false;
    snprintf(m->root, sizeof(m->root), "%s", root);
    m->be = pgw_posix_backend_open(m->root);
    if (m->be == NULL)
        return false;

    pgw_fs_opts local;
    if (opts == NULL) {
        pgw_fs_opts_default(&local);
        opts = &local;
    }
    const pgw_fs_backend_ops *ops = &pgw_posix_backend_ops;
    void *impl = m->be;
    if (probe != NULL) {
        memset(probe, 0, sizeof(*probe));
        probe->next = &pgw_posix_backend_ops;
        probe->next_impl = m->be;
        m->probe = probe;
        ops = &cas_probe_ops;
        impl = probe;
    }
    m->fs = pgw_rmfs_create(ops, impl, opts);
    if (m->fs == NULL) {
        pgw_posix_backend_close(m->be);
        m->be = NULL;
        return false;
    }
    m->cred = tc_self(10); /* the owner of the files, never a literal uid */
    return true;
}

void cas_mount_close(cas_mount *m)
{
    if (m == NULL)
        return;
    if (m->fs != NULL)
        pgw_rmfs_destroy(m->fs); /* the backend destroy is the probe's no-op */
    if (m->probe != NULL && m->be != NULL)
        pgw_posix_backend_close(m->be);
    m->fs = NULL;
    m->be = NULL;
    m->probe = NULL;
}

void cas_mount_abs(const cas_mount *m, const char *rel, char *out, size_t cap)
{
    cas_path_join(out, cap, m->root, (rel != NULL) ? rel : "");
}

void cas_path_join(char *out, size_t cap, const char *a, const char *b)
{
    if (b == NULL || b[0] == '\0')
        snprintf(out, cap, "%s", a);
    else
        snprintf(out, cap, "%s/%s", a, b);
}

/* --- scale of a case ------------------------------------------------------ */

uint64_t cas_env_u64(const char *name, uint64_t def)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0')
        return def;
    errno = 0;
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0')
        return def;
    return (uint64_t)n;
}

/* --- truth side ----------------------------------------------------------- */

static bool cas_truth_stat(const char *root, const char *rel, struct stat *st)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    return lstat(path, st) == 0;
}

uint64_t cas_truth_size(const char *root, const char *rel)
{
    struct stat st;
    if (!cas_truth_stat(root, rel, &st))
        return 0;
    return (uint64_t)st.st_size;
}

bool cas_truth_exists(const char *root, const char *rel)
{
    struct stat st;
    return cas_truth_stat(root, rel, &st);
}

uint64_t cas_truth_blocks(const char *root, const char *rel)
{
    struct stat st;
    if (!cas_truth_stat(root, rel, &st))
        return 0;
    return (uint64_t)st.st_blocks * 512u; /* POSIX: st_blocks is in 512 B units */
}

size_t cas_truth_count(const char *root, const char *rel)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    DIR *d = opendir(path);
    if (d == NULL)
        return (size_t)-1;
    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        n++;
    }
    closedir(d);
    return n;
}

bool cas_truth_read(const char *root, const char *rel, void *buf, size_t cap,
                    size_t *out_len)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    *out_len = fread(buf, 1, cap, f);
    fclose(f);
    return true;
}

int cas_truth_mkdir_errno(const char *root, const char *rel, uint32_t mode)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    if (mkdir(path, (mode_t)mode) == 0)
        return 0;
    return errno;
}

int cas_truth_stat_errno(const char *root, const char *rel)
{
    struct stat st;
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    if (lstat(path, &st) == 0)
        return 0;
    return errno;
}

int cas_truth_create_errno(const char *root, const char *rel)
{
    char path[CAS_PATH_MAX];
    cas_path_join(path, sizeof(path), root, rel);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return errno;
    close(fd);
    return 0;
}

/* --- host helpers --------------------------------------------------------- */

size_t cas_fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL)
        return 0; /* the platform does not tell: the caller falls back */
    size_t n = 0;
    while (readdir(d) != NULL)
        n++;
    closedir(d);
    return (n > 3) ? n - 3 : 0; /* minus ".", ".." and the directory itself */
}

bool cas_drop_cache(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    int rc = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
    return rc == 0;
}
