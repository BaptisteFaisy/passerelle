/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_9p.c - implementation of the 9P-side fixture of the directed cases.
 *
 * Starts ref9p_server.py over a fresh temporary directory (the truth),
 * attaches a pgw_p9_session through the harness transport and builds the
 * layer under test on top of the 9P adapter, so a case can compare what the
 * client obtained with what the real directory holds.
 *
 * Covers PERF-04, FS-15 through the adapter (P9-02, P9-03).
 */
#define _POSIX_C_SOURCE 200809L
#include "cas_9p.h"

#include "test_cred.h"

#include "pgw/limits.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool cas_9p_open(cas_9p *m, const char *tag, const pgw_fs_opts *opts)
{
    memset(m, 0, sizeof(*m));
    char *root = pgw_test_tmpdir(tag);
    if (root == NULL)
        return false;
    snprintf(m->root, sizeof(m->root), "%s", root);
    m->srv = ref9p_start(m->root, "none");
    if (m->srv == NULL)
        return false;
    m->session = pgw_p9_session_open(ref9p_rpc, ref9p_rpc_user(m->srv),
                                     PGW_MSIZE_DEFAULT, "cas", "",
                                     (uint32_t)getuid());
    if (m->session == NULL)
        return false;

    pgw_fs_opts local;
    if (opts == NULL) {
        pgw_fs_opts_default(&local);
        opts = &local;
    }
    m->fs = pgw_rmfs_from_9p(m->session, opts);
    if (m->fs == NULL)
        return false;
    m->cred = tc_self(10);
    return true;
}

void cas_9p_close(cas_9p *m)
{
    if (m == NULL)
        return;
    if (m->fs != NULL)
        pgw_rmfs_destroy(m->fs); /* the session belongs to this fixture */
    m->fs = NULL;
    if (m->session != NULL) {
        pgw_p9_session_close(m->session);
        m->session = NULL;
    }
    if (m->srv != NULL) {
        ref9p_stop(m->srv);
        m->srv = NULL;
    }
}

void cas_9p_abs(const cas_9p *m, const char *rel, char *out, size_t cap)
{
    cas_path_join(out, cap, m->root, (rel != NULL) ? rel : "");
}
