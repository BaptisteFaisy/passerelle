/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_dec.c - decoders for the R-messages of 9P2000.L.
 *
 * Every entry point here is a trust boundary (P9-09, ROB-06): the bytes come
 * from a server that may be buggy or hostile. Five rules are enforced for
 * every message, without exception:
 *
 *   1. size[4] >= 7 and size[4] <= the number of bytes actually received;
 *   2. the type is the one expected for that call;
 *   3. the tag is the one of the request it answers;
 *   4. every internal length (string length, count, nwqid, readdir record) is
 *      checked against what is left in the message before it is used;
 *   5. nothing is left over once the last field has been read.
 *
 * A rejected message becomes a pgw_err, never a partial result: the output
 * parameters of a failing decoder are left untouched.
 */
#include "p9_priv.h"

#include "pgw/buf.h"
#include "pgw/limits.h"

#include <string.h>

pgw_err pgw_p9_dec_hdr(const void *msg, size_t len, pgw_p9_hdr *out)
{
    if (msg == NULL || out == NULL)
        return PGW_EINVAL;
    if (len < PGW_P9_HDR_LEN)
        return PGW_EMSGSIZE;
    pgw_buf r;
    pgw_buf_init(&r, msg, PGW_P9_HDR_LEN);
    uint32_t size = pgw_get_u32(&r);
    uint8_t type = pgw_get_u8(&r);
    uint16_t tag = pgw_get_u16(&r);
    if (!pgw_buf_ok(&r))
        return PGW_EMSGSIZE;
    if (size < PGW_P9_HDR_LEN)
        return PGW_EINVAL;
    if ((size_t)size > len)
        return PGW_EMSGSIZE;
    out->size = size;
    out->type = type;
    out->tag = tag;
    return PGW_OK;
}

pgw_err pgw_p9__body(const void *msg, size_t len, uint8_t type, uint16_t tag,
                     pgw_buf *r)
{
    pgw_p9_hdr h;
    pgw_err e = pgw_p9_dec_hdr(msg, len, &h);
    if (e != PGW_OK)
        return e;
    if (h.type != type)
        return PGW_EINVAL;
    if (h.tag != tag)
        return PGW_EINVAL;
    pgw_buf_body(r, msg, h.size, PGW_P9_HDR_LEN);
    return pgw_buf_ok(r) ? PGW_OK : PGW_EMSGSIZE;
}

pgw_err pgw_p9__done(const pgw_buf *r)
{
    if (!pgw_buf_ok(r))
        return PGW_EMSGSIZE;
    return pgw_left(r) == 0 ? PGW_OK : PGW_EINVAL;
}

void pgw_p9__get_qid(pgw_buf *r, pgw_p9_qid *q)
{
    q->type = pgw_get_u8(r);
    q->version = pgw_get_u32(r);
    q->path = pgw_get_u64(r);
}

pgw_err pgw_p9_dec_rlerror(const void *msg, size_t len, uint16_t tag,
                           uint32_t *linux_errno)
{
    if (linux_errno == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RLERROR, tag, &r);
    if (e != PGW_OK)
        return e;
    uint32_t code = pgw_get_u32(&r);
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *linux_errno = code;
    return PGW_OK;
}

pgw_err pgw_p9_dec_rversion(const void *msg, size_t len, uint16_t tag,
                            uint32_t *msize, char *ver, size_t ver_cap)
{
    if (msize == NULL || ver == NULL || ver_cap == 0)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RVERSION, tag, &r);
    if (e != PGW_OK)
        return e;
    uint32_t m = pgw_get_u32(&r);
    size_t vlen = 0;
    int sres = pgw_get_str(&r, ver, ver_cap, &vlen);
    if (sres == PGW_STR_INVALID)
        return PGW_EMSGSIZE;
    if (sres == PGW_STR_TRUNCATED)
        return PGW_ENAMETOOLONG;
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *msize = m;
    return PGW_OK;
}

pgw_err pgw_p9_dec_rattach(const void *msg, size_t len, uint16_t tag,
                           pgw_p9_qid *qid)
{
    if (qid == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RATTACH, tag, &r);
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

pgw_err pgw_p9_dec_rwalk(const void *msg, size_t len, uint16_t tag,
                         uint16_t *nwqid, pgw_p9_qid *wqids, uint16_t max_wqids)
{
    if (nwqid == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RWALK, tag, &r);
    if (e != PGW_OK)
        return e;
    uint16_t n = pgw_get_u16(&r);
    if (!pgw_buf_ok(&r))
        return PGW_EMSGSIZE;
    /* A Rwalk cannot report more qids than names were asked for, and the
     * count must match the bytes that follow exactly. */
    if (n > (uint16_t)PGW_MAXWELEM || n > max_wqids || (n > 0 && wqids == NULL))
        return PGW_EINVAL;
    if (pgw_left(&r) != (size_t)n * 13u)
        return PGW_EINVAL;
    for (uint16_t i = 0; i < n; i++)
        pgw_p9__get_qid(&r, &wqids[i]);
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *nwqid = n;
    return PGW_OK;
}

pgw_err pgw_p9__dec_qid_iounit(const void *msg, size_t len, uint8_t expect_type,
                               uint16_t tag, pgw_p9_qid *qid, uint32_t *iounit)
{
    if (qid == NULL || iounit == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, expect_type, tag, &r);
    if (e != PGW_OK)
        return e;
    pgw_p9_qid q;
    pgw_p9__get_qid(&r, &q);
    uint32_t unit = pgw_get_u32(&r);
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *qid = q;
    *iounit = unit;
    return PGW_OK;
}

pgw_err pgw_p9_dec_rlopen(const void *msg, size_t len, uint16_t tag,
                          pgw_p9_qid *qid, uint32_t *iounit)
{
    return pgw_p9__dec_qid_iounit(msg, len, PGW_P9_RLOPEN, tag, qid, iounit);
}

pgw_err pgw_p9_dec_rread(const void *msg, size_t len, uint16_t tag,
                         uint32_t *count, const uint8_t **data_ptr)
{
    if (count == NULL || data_ptr == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RREAD, tag, &r);
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
    *count = n;
    *data_ptr = data;
    return PGW_OK;
}

pgw_err pgw_p9_dec_rwrite(const void *msg, size_t len, uint16_t tag,
                          uint32_t *count)
{
    if (count == NULL)
        return PGW_EINVAL;
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, PGW_P9_RWRITE, tag, &r);
    if (e != PGW_OK)
        return e;
    uint32_t n = pgw_get_u32(&r);
    e = pgw_p9__done(&r);
    if (e != PGW_OK)
        return e;
    *count = n;
    return PGW_OK;
}

pgw_err pgw_p9_dec_empty(const void *msg, size_t len, uint8_t expect_type,
                         uint16_t tag)
{
    pgw_buf r;
    pgw_err e = pgw_p9__body(msg, len, expect_type, tag, &r);
    if (e != PGW_OK)
        return e;
    return pgw_p9__done(&r);
}
