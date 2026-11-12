/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas_9p.h - the directed cases seen from the client side of a real 9P link.
 *
 * The POSIX fixture proves the layer; it does not prove what a *client* sees,
 * because the client sits behind the 9P adapter. This fixture wires the same
 * temporary directory to the reference 9P server of tests/ref9p (an
 * independent implementation), opens a pgw_p9_session over it and hands that
 * session to pgw_rmfs_from_9p(), so a case can compare the bytes the client
 * obtained with the bytes the real directory holds (share -> 9P -> client ->
 * rmfs -> truth).
 *
 * Covers PERF-04 (the size and the entry count the client sees), FS-15
 * (exotic names survive the wire), P9-02/P9-03 through the adapter.
 */
#ifndef PGW_TESTS_CAS_9P_H
#define PGW_TESTS_CAS_9P_H

#include "cas.h"

#include "pgw/p9.h"
#include "ref9p_harness.h"

struct cas_9p {
    struct ref9p_server *srv;
    pgw_p9_session *session;
    pgw_rmfs *fs;
    char root[512];   /* the served directory: the truth */
    pgw_fs_cred cred; /* the owner of the files, never a literal uid */
};
typedef struct cas_9p cas_9p;

/* Start the reference server over a fresh temporary directory, attach a
 * session and build the layer on top of it. */
bool cas_9p_open(struct cas_9p *m, const char *tag, const pgw_fs_opts *opts);
void cas_9p_close(struct cas_9p *m);

/* Absolute host path of a relative path inside the served directory. */
void cas_9p_abs(const struct cas_9p *m, const char *rel, char *out, size_t cap);

/* The view the shared case bodies take (cas_cases.h): the client view of the
 * 9P adapter over the served directory. */
static inline void cas_view_of_9p(cas_view *v, const struct cas_9p *m)
{
    v->fs = m->fs;
    v->root = m->root;
    v->cred = m->cred;
}

#endif /* PGW_TESTS_CAS_9P_H */
