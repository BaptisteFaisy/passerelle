/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_ops2.c - Session operations, part 2: namespace mutations
 * and the remaining metadata calls (MKDIR, UNLINK, RMDIR, RENAME, SYMLINK,
 * LINK, READLINK, FSYNC, STATFS).
 *
 * Implements requirements:
 *   - VFS-03: directory, link and synchronisation operations
 *   - VFS-04: MKDIR, SYMLINK and LINK return a node, which is tracked
 */
#include "fusep_ops.h"

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

pgw_err pgw_fuse_mkdir(pgw_fuse_session *s, uint64_t parent, const char *name,
                       uint32_t mode, pgw_fuse_entry *out, uint32_t prio)
{
    if (s == NULL || name == NULL || out == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    uint32_t lmode = pgw_linux_mode(PGW_FT_DIR, mode);
    int32_t n = pgw_fuse_enc_mkdir(tx, sizeof(tx), u, parent, name, lmode, 0);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = u;
    c.a = out;
    pgw_err rc =
        pgw_fusep_call(s, tx, (size_t)n, false, prio, pgw_fusep_done_entry, &c);
    if (rc != PGW_OK)
        return rc;
    return pgw_fusep_track_node(s, out->nodeid);
}

pgw_err pgw_fuse_unlink(pgw_fuse_session *s, uint64_t parent, const char *name,
                        bool is_dir, uint32_t prio)
{
    if (s == NULL || name == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    uint32_t op = is_dir ? (uint32_t)PGW_FUSE_RMDIR : (uint32_t)PGW_FUSE_UNLINK;
    int32_t n = pgw_fuse_enc_name_op(tx, sizeof(tx), op, u, parent, name);
    if (n <= 0)
        return PGW_EINVAL;
    return pgw_fusep_call_empty(s, tx, n, u, prio);
}

pgw_err pgw_fuse_rename(pgw_fuse_session *s, uint64_t olddir,
                        const char *oldname, uint64_t newdir,
                        const char *newname, uint32_t prio)
{
    if (s == NULL || oldname == NULL || newname == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_rename(tx, sizeof(tx), u, olddir, oldname, newdir,
                                    newname, 0);
    if (n <= 0)
        return PGW_EINVAL;
    return pgw_fusep_call_empty(s, tx, n, u, prio);
}

pgw_err pgw_fuse_symlink(pgw_fuse_session *s, uint64_t parent, const char *name,
                         const char *target, pgw_fuse_entry *out, uint32_t prio)
{
    if (s == NULL || name == NULL || target == NULL || out == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_symlink(tx, sizeof(tx), u, parent, name, target);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = u;
    c.a = out;
    pgw_err rc =
        pgw_fusep_call(s, tx, (size_t)n, false, prio, pgw_fusep_done_entry, &c);
    if (rc != PGW_OK)
        return rc;
    return pgw_fusep_track_node(s, out->nodeid);
}

pgw_err pgw_fuse_link(pgw_fuse_session *s, uint64_t parent, uint64_t oldnodeid,
                      const char *name, pgw_fuse_entry *out, uint32_t prio)
{
    if (s == NULL || name == NULL || out == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_link(tx, sizeof(tx), u, parent, oldnodeid, name);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = u;
    c.a = out;
    pgw_err rc =
        pgw_fusep_call(s, tx, (size_t)n, false, prio, pgw_fusep_done_entry, &c);
    if (rc != PGW_OK)
        return rc;
    return pgw_fusep_track_node(s, out->nodeid);
}

pgw_err pgw_fuse_readlink(pgw_fuse_session *s, uint64_t nodeid, char *target,
                          size_t cap, uint32_t prio)
{
    if (s == NULL || target == NULL || cap == 0)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_readlink(tx, sizeof(tx), u, nodeid);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep3 c;
    c.unique = u;
    c.a = target;
    c.b = NULL;
    c.cap = cap;
    return pgw_fusep_call(s, tx, (size_t)n, false, prio,
                          pgw_fusep_done_readlink, &c);
}

pgw_err pgw_fuse_fsync(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                       bool is_dir, uint32_t prio)
{
    if (s == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_fsync(tx, sizeof(tx), u, nodeid, fh, false, is_dir);
    if (n <= 0)
        return PGW_EINVAL;
    return pgw_fusep_call_empty(s, tx, n, u, prio);
}

pgw_err pgw_fuse_statfs(pgw_fuse_session *s, uint64_t nodeid,
                        pgw_fuse_kstatfs *out, uint32_t prio)
{
    if (s == NULL || out == NULL)
        return PGW_EINVAL;
    uint8_t tx[PGW_FUSE_TX_SMALL];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_statfs(tx, sizeof(tx), u, nodeid);
    if (n <= 0)
        return PGW_EINVAL;
    pgw_fusep_rep1 c;
    c.unique = u;
    c.a = out;
    return pgw_fusep_call(s, tx, (size_t)n, false, prio,
                          pgw_fusep_done_statfs, &c);
}
