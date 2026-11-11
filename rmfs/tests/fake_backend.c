/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/fake_backend.c - Lifecycle and helpers of the fake backend.
 *
 * The conformance bench of layer 3 must not depend on a protocol: this fake
 * implements pgw_fs_backend_ops directly on a directory created by
 * pgw_test_tmpdir(), so the assertions that will run inside a QNX guest
 * against 9P or virtiofs run on the PC in milliseconds (FS-01..FS-15).
 *
 * This file holds the creation/teardown and the host-side helpers; the vtable
 * itself lives in fake_backend_ops.c.
 */
#define _XOPEN_SOURCE 700

#include "fake_backend.h"

#include "fake_backend_priv.h"

#include "pgw/log.h"
#include "pgw/test.h"
#include "pgw/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void fake_fill_attr(pgw_fs_attr *out, const struct stat *st)
{
    memset(out, 0, sizeof(*out));
    out->type = pgw_ftype_from_host_mode((uint32_t)st->st_mode);
    out->mode = (uint32_t)st->st_mode & PGW_S_PERM_MASK;
    out->uid = (uint32_t)st->st_uid;
    out->gid = (uint32_t)st->st_gid;
    out->ino = (uint64_t)st->st_ino;
    out->size = (uint64_t)st->st_size;
    out->nlink = (uint64_t)st->st_nlink;
    out->atime_ns = pgw_ns_join((uint64_t)st->st_atim.tv_sec,
                                (uint32_t)st->st_atim.tv_nsec);
    out->mtime_ns = pgw_ns_join((uint64_t)st->st_mtim.tv_sec,
                                (uint32_t)st->st_mtim.tv_nsec);
    out->ctime_ns = pgw_ns_join((uint64_t)st->st_ctim.tv_sec,
                                (uint32_t)st->st_ctim.tv_nsec);
}

bool fake_take_failure(fake_backend *b, pgw_err *out)
{
    if (!b->fail_next)
        return false;
    b->fail_next = false;
    *out = b->fail_err;
    return true;
}

fake_handle *fake_handle_at(void *impl, uint64_t handle)
{
    if (handle == 0 || handle > FAKE_MAX_OPEN)
        return NULL;
    fake_handle *h = fake_handles(impl);
    fake_handle *e = &h[handle - 1u];
    return e->used ? e : NULL;
}

void fake_backend_host_path(const fake_backend *b, const char *rel, char *out,
                            size_t cap)
{
    if (rel == NULL || rel[0] == '\0' || strcmp(rel, ".") == 0)
        snprintf(out, cap, "%s", b->root);
    else
        snprintf(out, cap, "%s/%s", b->root, rel);
}

static void fake_close_all_handles(void *impl)
{
    fake_handle *h = fake_handles(impl);
    for (int i = 0; i < FAKE_MAX_OPEN; i++) {
        if (!h[i].used)
            continue;
        if (h[i].is_dir)
            closedir(h[i].dirp);
        else
            close(h[i].fd);
        h[i].used = 0;
    }
}

static void fb_destroy(void *impl)
{
    fake_backend *b = (fake_backend *)impl;
    fake_close_all_handles(impl);
    b->destroyed++;
}

fake_backend *fake_backend_new(void)
{
    char *dir = pgw_test_tmpdir("pgw-rmfs");
    if (dir == NULL)
        return NULL;
    size_t sz = sizeof(fake_backend) + FAKE_MAX_OPEN * sizeof(fake_handle);
    fake_backend *b = (fake_backend *)calloc(1, sz);
    if (b == NULL)
        return NULL;
    snprintf(b->root, sizeof(b->root), "%s", dir);

    static pgw_fs_backend_ops ops; /* one vtable for every instance */
    fake_backend_install_ops(&ops);
    ops.destroy = fb_destroy;
    b->ops = &ops;
    return b;
}

void fake_backend_free(fake_backend *b)
{
    free(b);
}

int fake_backend_host_write(const fake_backend *b, const char *rel,
                            const char *data, size_t len)
{
    char host[1024];
    fake_backend_host_path(b, rel, host, sizeof(host));
    FILE *f = fopen(host, "wb");
    if (f == NULL)
        return -1;
    size_t n = (len > 0) ? fwrite(data, 1, len, f) : 0;
    if (fclose(f) != 0)
        return -1;
    return (n == len) ? 0 : -1;
}
