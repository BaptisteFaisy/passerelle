/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * testfs.c - implementation of the shared end-to-end fixture.
 */
#define _POSIX_C_SOURCE 200809L
#include "testfs.h"

#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

bool testfs_open(testfs *t, const char *tag, const pgw_fs_opts *opts)
{
    memset(t, 0, sizeof(*t));
    t->root = pgw_test_tmpdir(tag);
    if (t->root == NULL)
        return false;
    t->be = pgw_posix_backend_open(t->root);
    if (t->be == NULL)
        return false;
    pgw_fs_opts local;
    if (opts == NULL) {
        pgw_fs_opts_default(&local);
        opts = &local;
    }
    t->fs = pgw_rmfs_create(&pgw_posix_backend_ops, t->be, opts);
    if (t->fs == NULL) {
        pgw_posix_backend_close(t->be);
        t->be = NULL;
        return false;
    }
    t->cred.uid = 0;
    t->cred.gid = 0;
    t->cred.prio = 10;
    return true;
}

void testfs_close(testfs *t)
{
    if (t == NULL)
        return;
    if (t->fs != NULL)
        pgw_rmfs_destroy(t->fs); /* closes the backend through ops->destroy */
    t->fs = NULL;
    t->be = NULL;
}

void testfs_abs(const testfs *t, const char *rel, char *out, size_t cap)
{
    if (rel == NULL || rel[0] == '\0')
        snprintf(out, cap, "%s", t->root);
    else
        snprintf(out, cap, "%s/%s", t->root, rel);
}
