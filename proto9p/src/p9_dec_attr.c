/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_dec_attr.c - decoders for the metadata replies of 9P2000.L.
 *
 * Same trust rules as p9_dec.c (P9-09): declared size, type, tag, internal
 * lengths and "no trailing bytes" are all checked before any field is handed
 * to the caller. The readdir record decoder is the one place where a
 * variable-length record repeats until the payload is exhausted, so it is
 * driven by an explicit cursor and never trusts the count[4] field on its own.
 */
#include "p9_priv.h"

#include "pgw/buf.h"
#include "pgw/limits.h"

pgw_err pgw_p9_dec_rgetattr(const void *msg, size_t len, uint16_t tag,
                            pgw_p9_attr *attr)
{
    if (attr == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RGETATTR, tag, &r);
    if (e != PGW_OK)
        return e;
    pgw_p9_attr a;
    a.valid = pgw_get_u64(&r);
    pgw_p9__get_qid(&r, &a.qid);
    a.mode = pgw_get_u32(&r);
    a.uid = pgw_get_u32(&r);
    a.gid = pgw_get_u32(&r);
    a.nlink = pgw_get_u64(&r);
    a.rdev = pgw_get_u64(&r);
    a.size = pgw_get_u64(&r);
    a.blksize = pgw_get_u64(&r);
    a.blocks = pgw_get_u64(&r);
    a.atime_sec = pgw_get_u64(&r);
    a.atime_nsec = pgw_get_u64(&r);
    a.mtime_sec = pgw_get_u64(&r);
    a.mtime_nsec = pgw_get_u64(&r);
    a.ctime_sec = pgw_get_u64(&r);
    a.ctime_nsec = pgw_get_u64(&r);
    /* btime, gen and data_version are reserved in 9P2000.L and have no field
     * in pgw_p9_attr, but they are part of the reply and must be present. */
    (void)pgw_get_u64(&r); /* btime_sec */
    (void)pgw_get_u64(&r); /* btime_nsec */
    (void)pgw_get_u64(&r); /* gen */
    (void)pgw_get_u64(&r); /* data_version */
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *attr = a;
    return PGW_OK;
}

bool pgw_p9__dirent_next(pgw_buf *r, pgw_p9_dirent *out)
{
    if (r == NULL || out == NULL || pgw_left(r) == 0)
        return false;
    pgw_p9_qid q;
    pgw_p9__get_qid(r, &q);
    uint64_t off = pgw_get_u64(r);
    uint8_t type = pgw_get_u8(r);
    size_t nlen = 0;
    int sres = pgw_get_str(r, out->name, sizeof(out->name), &nlen);
    if (sres != PGW_STR_OK || nlen > PGW_NAME_MAX || !pgw_buf_ok(r))
        return false;
    out->qid = q;
    out->offset = off;
    out->type = type;
    out->name_len = (uint16_t)nlen;
    return true;
}

pgw_err pgw_p9_dec_rreaddir_iter(const void *msg, size_t len, uint16_t tag,
                                 pgw_p9_dirent *out, size_t max_entries,
                                 size_t *out_count)
{
    if (out_count == NULL || (out == NULL && max_entries > 0))
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RREADDIR, tag, &r);
    if (e != PGW_OK)
        return e;
    uint32_t n = pgw_get_u32(&r);
    if (!pgw_buf_ok(&r))
        return PGW_EMSGSIZE;
    const uint8_t *data = pgw_take(&r, n);
    if (data == NULL)
        return PGW_EMSGSIZE;
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    pgw_buf d;
    pgw_buf_init(&d, data, n);
    size_t count = 0;
    while (pgw_left(&d) > 0) {
        if (count >= max_entries)
            return PGW_ENOMEM; /* caller's array is too small for this reply */
        if (!pgw_p9__dirent_next(&d, &out[count]))
            return PGW_EINVAL;
        count++;
    }
    *out_count = count;
    return PGW_OK;
}

pgw_err pgw_p9_dec_rreadlink(const void *msg, size_t len, uint16_t tag,
                             char *target, size_t cap)
{
    if (target == NULL || cap == 0)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RREADLINK, tag, &r);
    if (e != PGW_OK)
        return e;
    size_t tlen = 0;
    int sres = pgw_get_str(&r, target, cap, &tlen);
    if (sres == PGW_STR_INVALID)
        return PGW_EMSGSIZE;
    if (sres == PGW_STR_TRUNCATED)
        return PGW_ENAMETOOLONG;
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    return PGW_OK;
}

pgw_err pgw_p9__dec_qid_only(const void *msg, size_t len, uint8_t expect_type,
                             uint16_t tag, pgw_p9_qid *qid)
{
    if (qid == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, expect_type, tag, &r);
    if (e != PGW_OK)
        return e;
    pgw_p9_qid q;
    pgw_p9__get_qid(&r, &q);
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *qid = q;
    return PGW_OK;
}

pgw_err pgw_p9_dec_rmkdir(const void *msg, size_t len, uint16_t tag,
                          pgw_p9_qid *qid)
{
    return pgw_p9__dec_qid_only(msg, len, PGW_P9_RMKDIR, tag, qid);
}

pgw_err pgw_p9_dec_rstatfs(const void *msg, size_t len, uint16_t tag,
                           pgw_p9_statfs_t *st)
{
    if (st == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RSTATFS, tag, &r);
    if (e != PGW_OK)
        return e;
    pgw_p9_statfs_t s;
    s.type = pgw_get_u32(&r);
    s.bsize = pgw_get_u32(&r);
    s.blocks = pgw_get_u64(&r);
    s.bfree = pgw_get_u64(&r);
    s.bavail = pgw_get_u64(&r);
    s.files = pgw_get_u64(&r);
    s.ffree = pgw_get_u64(&r);
    s.fsid = pgw_get_u64(&r);
    s.namelen = pgw_get_u32(&r);
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *st = s;
    return PGW_OK;
}
