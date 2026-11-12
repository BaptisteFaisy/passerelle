/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_mount.c - the FUSE mount of the benches of tests/fuse_e2e.
 *
 * Starts the reference FUSE server of tests/fuse_ref over a real directory
 * (the truth), attaches a session to it and builds rmfs on top of the FUSE
 * adapter (pgw_rmfs_from_fuse). Nothing of the protocol is re-implemented
 * here: the transport is the one of the reference harness, a UNIX socket with
 * a 4-byte big-endian length per message.
 *
 * Covers VFS-05.
 */
#define _POSIX_C_SOURCE 200809L
#include "fuse_mount.h"

#include "fuse_ref_harness.h"

#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

/* The provider releases the context it built: the reference server and its
 * session (the rmfs destroy only frees the adapter, the session belongs to
 * the caller). */
static void fuse_release(void *user)
{
    fuse_ref_ctx_stop((fuse_ref_ctx *)user);
}

bool fuse_mount_open(fuse_mount *m, const pgw_fs_opts *opts)
{
    memset(m, 0, sizeof(*m));
    fuse_ref_ctx *ctx = fuse_ref_ctx_start(NULL, NULL);
    if (ctx == NULL)
        return false;
    pgw_fs_opts local;
    if (opts == NULL) {
        pgw_fs_opts_default(&local);
        opts = &local;
    }
    m->fs = pgw_rmfs_from_fuse(ctx->session, opts);
    if (m->fs == NULL) {
        fuse_ref_ctx_stop(ctx);
        return false;
    }
    snprintf(m->root, sizeof(m->root), "%s", ctx->root);
    m->ctx = ctx;
    return true;
}

void fuse_mount_close(fuse_mount *m)
{
    if (m == NULL)
        return;
    if (m->fs != NULL)
        pgw_rmfs_destroy(m->fs); /* the session belongs to the harness */
    m->fs = NULL;
    if (m->ctx != NULL) {
        fuse_ref_ctx_stop((fuse_ref_ctx *)m->ctx);
        m->ctx = NULL;
    }
}

bool fuse_diff_mount(diff_mount *m, void *user, char *err, size_t err_cap)
{
    (void)user; /* the provider needs no context */
    fuse_mount fm;
    if (!fuse_mount_open(&fm, NULL)) {
        snprintf(err, err_cap, "the reference FUSE server did not start");
        return false;
    }
    m->fs = fm.fs;
    snprintf(m->root, sizeof(m->root), "%s", fm.root);
    m->user = fm.ctx;
    m->release = fuse_release;
    m->leaked = NULL; /* the server counts its own descriptors */
    return true;
}
