/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_decode2.c - Reply decoders, part 2: READ, WRITE, READDIR,
 * READLINK, STATFS and empty replies.
 *
 * Implements requirements:
 *   - VFS-03: data path, directory listing and statistics replies
 *   - ROB-06 / P9-09: strict validation. In particular READDIR checks every
 *     record's `namelen`, its 8-byte aligned `reclen`, and refuses a reply
 *     whose last record does not end exactly on the message boundary.
 */
#include "fusep_internal.h"

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

pgw_err pgw_fuse_dec_read(const void *msg, size_t len, uint64_t unique,
                          const uint8_t **data, size_t *data_len)
{
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    size_t n = pgw_left(&body);
    const uint8_t *p = (n == 0) ? NULL : pgw_take(&body, n);
    if (n > 0 && p == NULL)
        return PGW_FUSEP_E_BAD_MSG;
    if (data != NULL)
        *data = p;
    if (data_len != NULL)
        *data_len = n;
    return PGW_OK;
}

pgw_err pgw_fuse_dec_write(const void *msg, size_t len, uint64_t unique,
                           uint32_t *size)
{
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;
    if (pgw_left(&body) != PGW_FUSE_WRITE_OUT_SIZE)
        return PGW_FUSEP_E_BAD_MSG;

    uint32_t written = pgw_get_u32(&body);
    (void)pgw_get_u32(&body); /* padding */
    if (!pgw_buf_ok(&body))
        return PGW_FUSEP_E_BAD_MSG;
    if (size != NULL)
        *size = written;
    return PGW_OK;
}

/* Decode one fuse_dirent record at the cursor. Returns PGW_OK, or
 * PGW_FUSEP_E_BAD_MSG when the record is impossible. */
static pgw_err dec_one_dirent(pgw_buf *r, pgw_fuse_dirent *out)
{
    if (pgw_left(r) < PGW_FUSE_DIRENT_HDR_SIZE)
        return PGW_FUSEP_E_BAD_MSG;
    uint64_t ino = pgw_get_u64(r);
    uint64_t off = pgw_get_u64(r);
    uint32_t namelen = pgw_get_u32(r);
    uint32_t type = pgw_get_u32(r);
    if (!pgw_buf_ok(r))
        return PGW_FUSEP_E_BAD_MSG;
    /* A name is never empty and never carries its NUL on the wire; it must
     * fit the fixed buffer of the public struct. */
    if (namelen == 0 || namelen >= sizeof(out->name))
        return PGW_FUSEP_E_BAD_MSG;

    size_t rec = pgw_fusep_align8((size_t)PGW_FUSE_DIRENT_HDR_SIZE + namelen);
    size_t payload = rec - PGW_FUSE_DIRENT_HDR_SIZE; /* name + padding */
    if (pgw_left(r) < payload)
        return PGW_FUSEP_E_BAD_MSG;
    const uint8_t *name = pgw_take(r, namelen);
    if (name == NULL)
        return PGW_FUSEP_E_BAD_MSG;
    memcpy(out->name, name, namelen);
    out->name[namelen] = '\0';
    out->namelen = (uint16_t)namelen;
    pgw_skip(r, payload - namelen); /* 8-byte padding, must be present */
    if (!pgw_buf_ok(r))
        return PGW_FUSEP_E_BAD_MSG;

    out->ino = ino;
    out->off = off;
    out->type = type;
    return PGW_OK;
}

pgw_err pgw_fuse_dec_readdir(const void *msg, size_t len, uint64_t unique,
                             pgw_fuse_dirent *out, size_t max_entries,
                             size_t *out_count)
{
    if (out_count != NULL)
        *out_count = 0;
    if (out == NULL && max_entries > 0)
        return PGW_EINVAL;
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    size_t count = 0;
    while (pgw_left(&body) > 0) {
        if (count >= max_entries)
            return PGW_E2BIG; /* caller must retry with a larger array */
        rc = dec_one_dirent(&body, &out[count]);
        if (rc != PGW_OK)
            return rc;
        count++;
    }
    if (!pgw_buf_ok(&body))
        return PGW_FUSEP_E_BAD_MSG;
    if (out_count != NULL)
        *out_count = count;
    return PGW_OK;
}

pgw_err pgw_fuse_dec_readlink(const void *msg, size_t len, uint64_t unique,
                              char *target, size_t cap)
{
    if (target == NULL || cap == 0)
        return PGW_EINVAL;
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    size_t n = pgw_left(&body);
    if (n == 0)
        return PGW_FUSEP_E_BAD_MSG; /* a symlink always has a target */
    if (n > cap - 1u)
        return PGW_ENAMETOOLONG;
    const uint8_t *p = pgw_take(&body, n);
    if (p == NULL)
        return PGW_FUSEP_E_BAD_MSG;
    memcpy(target, p, n);
    if (target[n - 1u] == '\0')
        n--; /* the wire string may or may not include its NUL */
    target[n] = '\0';
    return PGW_OK;
}

pgw_err pgw_fuse_dec_statfs(const void *msg, size_t len, uint64_t unique,
                            pgw_fuse_kstatfs *out)
{
    if (out == NULL)
        return PGW_EINVAL;
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;

    size_t left = pgw_left(&body);
    if (left != PGW_FUSE_KSTATFS_SIZE && left != PGW_FUSE_KSTATFS_COMPAT)
        return PGW_FUSEP_E_BAD_MSG;

    memset(out, 0, sizeof(*out));
    out->blocks = pgw_get_u64(&body);
    out->bfree = pgw_get_u64(&body);
    out->bavail = pgw_get_u64(&body);
    out->files = pgw_get_u64(&body);
    out->ffree = pgw_get_u64(&body);
    out->bsize = pgw_get_u32(&body);
    out->namelen = pgw_get_u32(&body);
    if (left >= PGW_FUSE_KSTATFS_SIZE) {
        out->frsize = pgw_get_u32(&body);
        (void)pgw_get_u32(&body);                /* padding */
        pgw_skip(&body, 6u * sizeof(uint32_t));  /* spare[6] */
    } else {
        out->frsize = out->bsize; /* pre-7.4 servers have no frsize */
    }
    if (!pgw_buf_ok(&body) || pgw_left(&body) != 0)
        return PGW_FUSEP_E_BAD_MSG;
    return PGW_OK;
}

pgw_err pgw_fuse_dec_empty(const void *msg, size_t len, uint64_t unique)
{
    pgw_buf body;
    pgw_err rc = pgw_fusep_dec_head(msg, len, unique, &body);
    if (rc != PGW_OK)
        return rc;
    return (pgw_left(&body) == 0) ? PGW_OK : PGW_FUSEP_E_BAD_MSG;
}
