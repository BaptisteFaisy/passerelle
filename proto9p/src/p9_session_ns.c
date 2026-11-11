/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_session_ns.c - namespace operations of the 9P session.
 *
 * Covers P9-02 (open, create, clunk, getattr, mkdir, unlinkat, renameat,
 * fsync, statfs), P9-03 (setattr, symlink, readlink, link), P9-04 (open flags
 * translated to their Linux values on the wire) and P9-06 (explicit Tflush).
 * Path walking lives in p9_session_walk.c and bulk transfers in p9_session_io.c.
 *
 * One rule shapes the code: every fid goes back to the pool, either clunked on
 * the server (Tclunk) or released locally when the server never saw it, so a
 * million opens and closes leak nothing (P9-07).
 */
#include "p9_session_priv.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <string.h>

/* --- one-shot operations -------------------------------------------------- */

pgw_err pgw_p9_clunk(pgw_p9_session *s, uint32_t fid, uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK) {
        pgw_p9__fid_free(s, fid); /* the fid is gone locally in every case */
        return e;
    }
    int32_t enc = pgw_p9_enc_tclunk(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag, fid);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RCLUNK, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_empty(pgw_p9__call_rx(&c), mlen, PGW_P9_RCLUNK, (uint16_t)c.tag);
    pgw_p9__call_end(&c);
    /* 9P: the fid is destroyed by Tclunk whether or not the reply is an error,
     * so it always goes back to the pool. */
    pgw_p9__fid_free(s, fid);
    return e;
}

pgw_err pgw_p9_lopen(pgw_p9_session *s, uint32_t fid, uint32_t pgw_oflags,
                     uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tlopen(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag, fid,
                                    pgw_p9__wire_flags(pgw_oflags));
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RLOPEN, prio, &mlen);
    if (e == PGW_OK) {
        pgw_p9_qid qid;
        uint32_t iounit = 0;
        e = pgw_p9_dec_rlopen(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, &qid, &iounit);
    }
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_lcreate(pgw_p9_session *s, uint32_t dfid, const char *name,
                       uint32_t pgw_oflags, uint32_t mode, uint32_t gid,
                       pgw_p9_qid *out_qid, uint32_t prio)
{
    if (name == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tlcreate(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                      dfid, name, pgw_p9__wire_flags(pgw_oflags),
                                      mode, gid);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RLCREATE, prio, &mlen);
    if (e == PGW_OK) {
        pgw_p9_qid qid;
        uint32_t iounit = 0;
        e = pgw_p9__dec_qid_iounit(pgw_p9__call_rx(&c), mlen, PGW_P9_RLCREATE,
                                   (uint16_t)c.tag, &qid, &iounit);
        if (e == PGW_OK && out_qid != NULL)
            *out_qid = qid;
    }
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_getattr(pgw_p9_session *s, uint32_t fid, pgw_p9_attr *out,
                       uint32_t prio)
{
    if (out == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tgetattr(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                      fid, PGW_P9_GETATTR_BASIC);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RGETATTR, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_rgetattr(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, out);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_setattr(pgw_p9_session *s, uint32_t fid,
                       const pgw_p9_setattr_t *sa, uint32_t prio)
{
    if (sa == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tsetattr(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                      fid, sa);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RSETATTR, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_empty(pgw_p9__call_rx(&c), mlen, PGW_P9_RSETATTR, (uint16_t)c.tag);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_mkdir(pgw_p9_session *s, uint32_t dfid, const char *name,
                     uint32_t mode, uint32_t gid, pgw_p9_qid *out_qid,
                     uint32_t prio)
{
    if (name == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tmkdir(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                    dfid, name, mode, gid);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RMKDIR, prio, &mlen);
    if (e == PGW_OK) {
        pgw_p9_qid qid;
        e = pgw_p9_dec_rmkdir(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, &qid);
        if (e == PGW_OK && out_qid != NULL)
            *out_qid = qid;
    }
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_unlinkat(pgw_p9_session *s, uint32_t dfid, const char *name,
                        bool is_dir, uint32_t prio)
{
    if (name == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    uint32_t flags = is_dir ? (uint32_t)PGW_P9_AT_REMOVEDIR : 0u;
    int32_t enc = pgw_p9_enc_tunlinkat(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                       dfid, name, flags);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RUNLINKAT, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_empty(pgw_p9__call_rx(&c), mlen, PGW_P9_RUNLINKAT,
                             (uint16_t)c.tag);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_renameat(pgw_p9_session *s, uint32_t olddfid, const char *oldname,
                        uint32_t newdfid, const char *newname, uint32_t prio)
{
    if (oldname == NULL || newname == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_trenameat(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                       olddfid, oldname, newdfid, newname);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RRENAMEAT, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_empty(pgw_p9__call_rx(&c), mlen, PGW_P9_RRENAMEAT,
                             (uint16_t)c.tag);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_symlink(pgw_p9_session *s, uint32_t dfid, const char *name,
                       const char *target, uint32_t gid, pgw_p9_qid *out_qid,
                       uint32_t prio)
{
    if (name == NULL || target == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tsymlink(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                      dfid, name, target, gid);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RSYMLINK, prio, &mlen);
    if (e == PGW_OK) {
        pgw_p9_qid qid;
        e = pgw_p9__dec_qid_only(pgw_p9__call_rx(&c), mlen, PGW_P9_RSYMLINK,
                                 (uint16_t)c.tag, &qid);
        if (e == PGW_OK && out_qid != NULL)
            *out_qid = qid;
    }
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_readlink(pgw_p9_session *s, uint32_t fid, char *target,
                        size_t cap, uint32_t prio)
{
    if (target == NULL || cap == 0)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_treadlink(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                       fid);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RREADLINK, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_rreadlink(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, target, cap);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_link(pgw_p9_session *s, uint32_t dfid, uint32_t fid,
                    const char *name, uint32_t prio)
{
    if (name == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tlink(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                   dfid, fid, name);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RLINK, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_empty(pgw_p9__call_rx(&c), mlen, PGW_P9_RLINK, (uint16_t)c.tag);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_fsync(pgw_p9_session *s, uint32_t fid, uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tfsync(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag, fid);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RFSYNC, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_empty(pgw_p9__call_rx(&c), mlen, PGW_P9_RFSYNC, (uint16_t)c.tag);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_statfs(pgw_p9_session *s, uint32_t fid, pgw_p9_statfs_t *out,
                      uint32_t prio)
{
    if (out == NULL)
        return PGW_EINVAL;
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tstatfs(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                     fid);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RSTATFS, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_rstatfs(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, out);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_flush(pgw_p9_session *s, uint16_t oldtag, uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tflush(pgw_p9__call_tx(&c), c.s->msize, (uint16_t)c.tag,
                                    oldtag);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RFLUSH, prio, &mlen);
    if (e == PGW_OK)
        e = pgw_p9_dec_empty(pgw_p9__call_rx(&c), mlen, PGW_P9_RFLUSH, (uint16_t)c.tag);
    pgw_p9__call_end(&c);
    pgw_stat_add(PGW_STAT_FLUSH, 1);
    return e;
}
