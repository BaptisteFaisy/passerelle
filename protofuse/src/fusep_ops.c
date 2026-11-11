/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_ops.c - Session operations, part 1: LOOKUP, GETATTR,
 * SETATTR, OPEN, CREATE and RELEASE.
 *
 * Implements requirements:
 *   - VFS-03: the metadata and open path of the virtiofs client
 *   - VFS-04: every successful LOOKUP or CREATE adds one reference to the node
 *     it returns, which is what FORGET later gives back
 *   - ROB-05: each request carries the caller priority and the session deadline
 */
#include "fusep_ops.h"

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

#include <string.h>

/* Send an already-encoded request that expects an empty reply. */
pgw_err pgw_fusep_call_empty(pgw_fuse_session *s, const void *tx, int32_t len,
                             uint64_t unique, uint32_t prio)
{
    if (len <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = unique;
    c.a = NULL;
    return pgw_fusep_call(s, tx, (size_t)len, false, prio,
                          pgw_fusep_done_empty, &c);
}

pgw_err pgw_fusep_track_node(pgw_fuse_session *s, uint64_t nodeid)
{
    if (s == NULL)
        return PGW_EINVAL;
    if (nodeid == 0)
        return PGW_OK;
    return pgw_fusep_refs_add(&s->refs, nodeid, 1);
}

pgw_err pgw_fuse_lookup(pgw_fuse_session *s, uint64_t parent, const char *name,
                        pgw_fuse_entry *out, uint32_t prio)
{
    if (s == NULL || out == NULL || name == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_lookup(tx, sizeof(tx), u, parent, name);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = u;
    c.a = out;
    pgw_err rc = pgw_fusep_call(s, tx, (size_t)n, false, prio,
                            pgw_fusep_done_entry, &c);
    if (rc != PGW_OK)
        return rc;
    /* nodeid 0 is a negative entry: no node, no reference to track. */
    return pgw_fusep_track_node(s, out->nodeid);
}

pgw_err pgw_fuse_getattr(pgw_fuse_session *s, uint64_t nodeid,
                         pgw_fuse_attr *out, uint32_t prio)
{
    if (s == NULL || out == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_getattr(tx, sizeof(tx), u, nodeid, 0, false);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = u;
    c.a = out;
    return pgw_fusep_call(s, tx, (size_t)n, false, prio,
                            pgw_fusep_done_attr, &c);
}

pgw_err pgw_fuse_setattr(pgw_fuse_session *s, uint64_t nodeid,
                         const pgw_fuse_setattr_in *sa, pgw_fuse_attr *out,
                         uint32_t prio)
{
    if (s == NULL || sa == NULL || out == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_setattr(tx, sizeof(tx), u, nodeid, sa);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = u;
    c.a = out;
    return pgw_fusep_call(s, tx, (size_t)n, false, prio,
                            pgw_fusep_done_attr, &c);
}

pgw_err pgw_fuse_open(pgw_fuse_session *s, uint64_t nodeid, uint32_t pgw_oflags,
                      bool is_dir, uint64_t *out_fh, uint32_t prio)
{
    if (s == NULL || out_fh == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    uint32_t lflags = pgw_linux_oflags(pgw_oflags); /* wire numbering */
    int32_t n = pgw_fuse_enc_open(tx, sizeof(tx), u, nodeid, lflags, is_dir);
    if (n <= 0)
        return PGW_EINVAL;
    uint32_t open_flags = 0;
    pgw_fusep_rep2 c;
    c.unique = u;
    c.a = out_fh;
    c.b = &open_flags;
    return pgw_fusep_call(s, tx, (size_t)n, false, prio,
                            pgw_fusep_done_open, &c);
}

pgw_err pgw_fuse_create(pgw_fuse_session *s, uint64_t parent, const char *name,
                        uint32_t pgw_oflags, uint32_t mode,
                        pgw_fuse_entry *out_e, uint64_t *out_fh, uint32_t prio)
{
    if (s == NULL || name == NULL || out_e == NULL || out_fh == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    uint32_t lflags = pgw_linux_oflags(pgw_oflags);
    uint32_t lmode = pgw_linux_mode(PGW_FT_REG, mode); /* perm bits + S_IFREG */
    int32_t n = pgw_fuse_enc_create(tx, sizeof(tx), u, parent, name, lflags,
                                    lmode, 0);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep2 c;
    c.unique = u;
    c.a = out_e;
    c.b = out_fh;
    pgw_err rc = pgw_fusep_call(s, tx, (size_t)n, false, prio,
                            pgw_fusep_done_create, &c);
    if (rc != PGW_OK)
        return rc;
    return pgw_fusep_track_node(s, out_e->nodeid);
}

pgw_err pgw_fuse_release(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                         bool is_dir, uint32_t prio)
{
    if (s == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_release(tx, sizeof(tx), u, nodeid, fh, is_dir);
    if (n <= 0)
        return PGW_EINVAL;
    return pgw_fusep_call_empty(s, tx, n, u, prio);
}
