/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_session_walk.c - path resolution over Twalk (P9-02, P9-07).
 *
 * A 9P server only walks one name at a time: it never crosses a directory for
 * the client, and it refuses "." and "..". This file turns a caller path into
 * the smallest number of Twalk messages that 9P2000.L allows (at most
 * PGW_MAXWELEM names each), resolves ".." and "." before they reach the wire,
 * and clunks every intermediate fid so that a deep lookup leaves exactly one
 * open fid behind (P9-07).
 *
 * 9P semantics that the code depends on (walk(5)): newfid is created only when
 * every name of the Twalk was walked; a partial walk returns the qids walked
 * so far and leaves newfid untouched, which is how a missing component is
 * reported.
 */
#include "p9_session_priv.h"

#include "pgw/limits.h"

#include <string.h>

/* --- path normalisation and walking --------------------------------------- */

/* Collapse slashes, drop ".", resolve ".." textually against the components
 * already accepted. Symlinks are followed by the caller, not here: a 9P server
 * does not cross a symlink during Twalk. */
static pgw_err normalize(const char *rel, char *out, size_t cap)
{
    if (rel == NULL || out == NULL || cap == 0)
        return PGW_EINVAL;
    size_t len = 0;
    const char *p = rel;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *end = p;
        while (*end != '\0' && *end != '/')
            end++;
        size_t clen = (size_t)(end - p);
        if (clen == 2 && p[0] == '.' && p[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;
        } else if (!(clen == 1 && p[0] == '.')) {
            if (clen > PGW_NAME_MAX || len + 1u + clen + 1u > cap)
                return PGW_ENAMETOOLONG;
            out[len++] = '/';
            memcpy(out + len, p, clen);
            len += clen;
        }
        p = end;
    }
    out[len] = '\0';
    return PGW_OK;
}

static pgw_err walk_batch(pgw_p9_session *s, uint32_t from, uint32_t to,
                          uint16_t n, const char *const *names,
                          uint16_t *walked, pgw_p9_qid *last, uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_twalk(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag, from,
                                   to, n, names);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RWALK, prio, &mlen);
    if (e == PGW_OK) {
        pgw_p9_qid qids[PGW_MAXWELEM];
        e = pgw_p9_dec_rwalk(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, walked, qids,
                             (uint16_t)PGW_MAXWELEM);
        if (e == PGW_OK && *walked > 0)
            *last = qids[*walked - 1];
    }
    pgw_p9__call_end(&c);
    return e;
}

/* A partial Rwalk carries no error code: deduce it from where it stopped. */
static pgw_err walk_partial_error(const pgw_p9_qid *last, uint16_t walked)
{
    if (walked == 0)
        return PGW_ENOENT;
    if ((last->type & PGW_P9_QTSYMLINK) != 0)
        return PGW_ELOOP;
    if ((last->type & PGW_P9_QTDIR) == 0)
        return PGW_ENOTDIR;
    return PGW_ENOENT;
}

/* Walk the (already normalised) components in place, at most PGW_MAXWELEM per
 * Twalk. Returns the deepest fid created, or PGW_FID_NONE for an empty path. */
static pgw_err walk_components(pgw_p9_session *s, uint32_t base_fid, char *norm,
                               uint32_t *out_fid, pgw_p9_qid *last, uint32_t prio)
{
    char *p = norm;
    char *stop = norm + strlen(norm);
    uint32_t cur = base_fid;
    uint32_t created = PGW_FID_NONE;
    pgw_err e = PGW_OK;
    while (e == PGW_OK && p < stop) {
        const char *names[PGW_MAXWELEM];
        uint16_t n = 0;
        while (p < stop && n < (uint16_t)PGW_MAXWELEM) {
            while (p < stop && *p == '/')
                p++;
            if (p >= stop)
                break;
            names[n++] = p;
            while (p < stop && *p != '/')
                p++;
            if (p < stop) {
                *p = '\0'; /* names[] are NUL-terminated views into `norm` */
                p++;
            }
        }
        uint32_t next_fid = 0;
        e = pgw_p9__fid_alloc(s, &next_fid);
        if (e != PGW_OK)
            break;
        uint16_t walked = 0;
        e = walk_batch(s, cur, next_fid, n, names, &walked, last, prio);
        if (e == PGW_OK && walked < n)
            e = walk_partial_error(last, walked);
        if (e != PGW_OK) {
            /* A partial Rwalk does not create newfid on the server, so the
             * identifier only has to go back to the pool, not to be clunked. */
            pgw_p9__fid_free(s, next_fid);
            break;
        }
        if (created != PGW_FID_NONE)
            (void)pgw_p9_clunk(s, created, prio); /* intermediate fid: P9-07 */
        created = next_fid;
        cur = next_fid;
    }
    if (e != PGW_OK) {
        if (created != PGW_FID_NONE)
            (void)pgw_p9_clunk(s, created, prio);
        return e;
    }
    *out_fid = created;
    return PGW_OK;
}

/* Clone `base` into a fresh fid (Twalk with nwname 0), fetching its qid when
 * the caller asked for one. */
static pgw_err clone_fid(pgw_p9_session *s, uint32_t base, uint32_t *out_fid,
                         pgw_p9_qid *qid, bool want_qid, uint32_t prio)
{
    uint32_t fid = 0;
    pgw_err e = pgw_p9__fid_alloc(s, &fid);
    if (e != PGW_OK)
        return e;
    uint16_t walked = 0;
    e = walk_batch(s, base, fid, 0, NULL, &walked, qid, prio);
    if (e == PGW_OK && want_qid) {
        pgw_p9_attr a;
        e = pgw_p9_getattr(s, fid, &a, prio);
        if (e == PGW_OK)
            *qid = a.qid;
    }
    if (e != PGW_OK) {
        (void)pgw_p9_clunk(s, fid, prio);
        return e;
    }
    *out_fid = fid;
    return PGW_OK;
}

pgw_err pgw_p9_walk_path(pgw_p9_session *s, uint32_t base_fid, const char *rel,
                         uint32_t *out_fid, pgw_p9_qid *out_qid, uint32_t prio)
{
    if (s == NULL || rel == NULL || out_fid == NULL)
        return PGW_EINVAL;
    char norm[PGW_PATH_MAX];
    pgw_err e = normalize(rel, norm, sizeof norm);
    if (e != PGW_OK)
        return e;
    pgw_p9_qid last = { 0, 0, 0 };
    uint32_t fid = PGW_FID_NONE;
    e = walk_components(s, base_fid, norm, &fid, &last, prio);
    if (e != PGW_OK)
        return e;
    if (fid == PGW_FID_NONE) {
        e = clone_fid(s, base_fid, &fid, &last, out_qid != NULL, prio);
        if (e != PGW_OK)
            return e;
    }
    *out_fid = fid;
    if (out_qid != NULL)
        *out_qid = last;
    return PGW_OK;
}
