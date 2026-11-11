/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_decode.c - Reply decoders, part 1: header, INIT, entry,
 * attr, OPEN and CREATE.
 *
 * Implements requirements:
 *   - VFS-03: version negotiation reply, attribute and entry replies
 *   - ROB-06 / P9-09: nothing the server sends is trusted. Each decoder checks
 *     the reply length, the `unique` tag, the sign of the error field and the
 *     exact size of the body, and rejects trailing bytes.
 */
#include "fusep_internal.h"

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

pgw_err pgw_fuse_dec_hdr(const void *msg, size_t len, pgw_fuse_out_hdr *out)
{
    if (msg == NULL || out == NULL)
        return PGW_EINVAL;
    if (len < PGW_FUSE_OUT_HDR_SZ)
        return PGW_FUSEP_E_BAD_MSG; /* truncated header */

    pgw_buf r;
    pgw_buf_init(&r, msg, len);
    uint32_t mlen = pgw_get_u32(&r);
    int32_t error = (int32_t)pgw_get_u32(&r);
    uint64_t unique = pgw_get_u64(&r);
    if (!pgw_buf_ok(&r))
        return PGW_FUSEP_E_BAD_MSG;
    if (mlen < PGW_FUSE_OUT_HDR_SZ || (size_t)mlen > len)
        return PGW_FUSEP_E_BAD_MSG; /* declared length impossible */
    if (error > 0)
        return PGW_FUSEP_E_BAD_MSG; /* must be 0 or a negative Linux errno */

    out->len = mlen;
    out->error = error;
    out->unique = unique;
    return PGW_OK;
}

/* Body must be exactly `want` bytes: shorter is truncation, longer is garbage. */
static pgw_err body_exact(const pgw_buf *body, size_t want)
{
    return (pgw_left(body) == want) ? PGW_OK : PGW_FUSEP_E_BAD_MSG;
}

pgw_err pgw_fuse_dec_init(const void *msg, size_t len, uint64_t unique,
                          pgw_fuse_init_info *out)
{
    if (out == NULL)
        return PGW_EINVAL;
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    size_t left = pgw_left(&body);
    /* Three widths are legal: 8 (7.0 compat), 24 (7.5/7.6) and 64 (7.23+). */
    if (left != PGW_FUSE_INIT_OUT_COMPAT && left != PGW_FUSE_INIT_OUT_7_22 &&
        left != PGW_FUSE_INIT_OUT_SIZE)
        return PGW_FUSEP_E_BAD_MSG;

    memset(out, 0, sizeof(*out));
    out->major = pgw_get_u32(&body);
    out->minor = pgw_get_u32(&body);
    if (left >= PGW_FUSE_INIT_OUT_7_22) {
        out->max_readahead = pgw_get_u32(&body);
        out->flags = pgw_get_u32(&body);
        (void)pgw_get_u16(&body); /* max_background */
        (void)pgw_get_u16(&body); /* congestion_threshold */
        out->max_write = pgw_get_u32(&body);
    }
    if (left >= PGW_FUSE_INIT_OUT_SIZE) {
        (void)pgw_get_u32(&body); /* time_gran */
        out->max_pages = pgw_get_u16(&body);
        (void)pgw_get_u16(&body);            /* map_alignment */
        out->flags |= (uint64_t)pgw_get_u32(&body) << 32; /* flags2 */
        pgw_skip(&body, 7u * sizeof(uint32_t));           /* unused[7] */
    }
    if (!pgw_buf_ok(&body))
        return PGW_FUSEP_E_BAD_MSG;
    return PGW_OK;
}

pgw_err pgw_fuse_dec_attr(const void *msg, size_t len, uint64_t unique,
                          pgw_fuse_attr *out)
{
    if (out == NULL)
        return PGW_EINVAL;
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    /* fuse_attr_out: attr_valid, attr_valid_nsec, dummy, then fuse_attr. */
    size_t left = pgw_left(&body);
    if (left < 16u)
        return PGW_FUSEP_E_BAD_MSG;
    (void)pgw_get_u64(&body); /* attr_valid */
    (void)pgw_get_u32(&body); /* attr_valid_nsec */
    (void)pgw_get_u32(&body); /* dummy */
    return pgw_fusep_dec_attr_bytes(&body, left - 16u, out);
}

pgw_err pgw_fuse_dec_entry(const void *msg, size_t len, uint64_t unique,
                           pgw_fuse_entry *out)
{
    if (out == NULL)
        return PGW_EINVAL;
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    /* fuse_entry_out: 40 bytes of entry fields, then fuse_attr. */
    size_t left = pgw_left(&body);
    if (left < PGW_FUSE_ENTRY_HDR_SIZE)
        return PGW_FUSEP_E_BAD_MSG;
    memset(out, 0, sizeof(*out));
    out->nodeid = pgw_get_u64(&body);
    out->generation = pgw_get_u64(&body);
    out->entry_valid = pgw_get_u64(&body);
    out->attr_valid = pgw_get_u64(&body);
    out->entry_valid_nsec = pgw_get_u32(&body);
    out->attr_valid_nsec = pgw_get_u32(&body);
    return pgw_fusep_dec_attr_bytes(&body, left - PGW_FUSE_ENTRY_HDR_SIZE,
                                    &out->attr);
}

pgw_err pgw_fuse_dec_open(const void *msg, size_t len, uint64_t unique,
                          uint64_t *fh, uint32_t *open_flags)
{
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;
    rc = body_exact(&body, PGW_FUSE_OPEN_OUT_SIZE);
    if (rc != PGW_OK)
        return rc;

    uint64_t f = pgw_get_u64(&body);
    uint32_t flags = pgw_get_u32(&body);
    (void)pgw_get_u32(&body); /* padding */
    if (!pgw_buf_ok(&body))
        return PGW_FUSEP_E_BAD_MSG;
    if (fh != NULL)
        *fh = f;
    if (open_flags != NULL)
        *open_flags = flags;
    return PGW_OK;
}

pgw_err pgw_fuse_dec_create(const void *msg, size_t len, uint64_t unique,
                            pgw_fuse_entry *entry, uint64_t *fh)
{
    if (entry == NULL || fh == NULL)
        return PGW_EINVAL;
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    /* CREATE replies with a fuse_entry_out immediately followed by
     * fuse_open_out; the entry part must be a whole number of attrs. */
    size_t left = pgw_left(&body);
    if (left < PGW_FUSE_ENTRY_HDR_SIZE + PGW_FUSE_OPEN_OUT_SIZE)
        return PGW_FUSEP_E_BAD_MSG;
    size_t attr_bytes =
        left - PGW_FUSE_ENTRY_HDR_SIZE - PGW_FUSE_OPEN_OUT_SIZE;
    memset(entry, 0, sizeof(*entry));
    entry->nodeid = pgw_get_u64(&body);
    entry->generation = pgw_get_u64(&body);
    entry->entry_valid = pgw_get_u64(&body);
    entry->attr_valid = pgw_get_u64(&body);
    entry->entry_valid_nsec = pgw_get_u32(&body);
    entry->attr_valid_nsec = pgw_get_u32(&body);
    rc = pgw_fusep_dec_attr_bytes(&body, attr_bytes, &entry->attr);
    if (rc != PGW_OK)
        return rc;

    uint64_t f = pgw_get_u64(&body);
    (void)pgw_get_u32(&body); /* open_flags */
    (void)pgw_get_u32(&body); /* padding */
    if (!pgw_buf_ok(&body) || pgw_left(&body) != 0)
        return PGW_FUSEP_E_BAD_MSG;
    *fh = f;
    return PGW_OK;
}
