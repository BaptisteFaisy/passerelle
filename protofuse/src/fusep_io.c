/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_io.c - Data path: READ, WRITE and READDIR, split and
 * buffered to fit the negotiated message size.
 *
 * Implements requirements:
 *   - VFS-03: read, write and directory listing over virtiofs
 *   - P9-08 (applied to virtiofs): a transfer larger than one message is split
 *     into several requests of at most `max_write` bytes, counted as splits
 *   - VFS-02: every request goes through the session queue and the transport
 *     callback; nothing here touches a device or a file descriptor
 */
#include "fusep_ops.h"

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/log.h"

#include <string.h>

/* --- READ ----------------------------------------------------------------- */

pgw_err pgw_fuse_read(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                      uint64_t off, void *buf, size_t len, size_t *out_read,
                      uint32_t prio)
{
    if (out_read != NULL)
        *out_read = 0;
    if (s == NULL || (buf == NULL && len > 0))
        return PGW_EINVAL;

    size_t total = 0;
    unsigned chunks = 0;
    while (total < len) {
        size_t want = len - total;
        if (want > (size_t)s->max_write)
            want = (size_t)s->max_write;
        uint8_t tx[PGW_FUSE_IN_HDR_SZ + PGW_FUSE_READ_IN_SIZE];
        uint64_t u = pgw_fusep_next_unique(s);
        int32_t n = pgw_fuse_enc_read(tx, sizeof(tx), u, nodeid, fh,
                                      off + (uint64_t)total, (uint32_t)want,
                                      false);
        if (n <= 0)
            return PGW_EINVAL;
        const uint8_t *data = NULL;
        size_t dlen = 0;
        pgw_fusep_rep2 c;
        c.unique = u;
        c.a = &data;
        c.b = &dlen;
        pgw_err rc = pgw_fusep_call(s, tx, (size_t)n, false, prio,
                            pgw_fusep_done_read, &c);
        if (rc != PGW_OK)
            return rc;
        if (dlen > want)
            return PGW_EIO; /* the server sent more than we asked for */
        if (dlen > 0)
            memcpy((uint8_t *)buf + total, data, dlen);
        total += dlen;
        chunks++;
        if (dlen < want)
            break; /* short answer: end of file */
    }
    if (chunks > 1u)
        pgw_stat_add(PGW_STAT_SPLIT, 1);
    pgw_stat_add(PGW_STAT_BYTES_READ, (uint64_t)total);
    if (out_read != NULL)
        *out_read = total;
    return PGW_OK;
}

/* --- WRITE ---------------------------------------------------------------- */

pgw_err pgw_fuse_write(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                       uint64_t off, const void *buf, size_t len,
                       size_t *out_written, uint32_t prio)
{
    if (out_written != NULL)
        *out_written = 0;
    if (s == NULL || (buf == NULL && len > 0))
        return PGW_EINVAL;

    size_t total = 0;
    unsigned chunks = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > (size_t)s->max_write)
            chunk = (size_t)s->max_write;
        if ((size_t)PGW_FUSE_IN_HDR_SZ + PGW_FUSE_WRITE_IN_SIZE + chunk >
            s->tx_cap)
            return PGW_EINVAL;
        uint64_t u = pgw_fusep_next_unique(s);
        int32_t n = pgw_fuse_enc_write(s->tx, s->tx_cap, u, nodeid, fh,
                                       off + (uint64_t)total,
                                       (const uint8_t *)buf + total,
                                       (uint32_t)chunk);
        if (n <= 0)
            return PGW_EINVAL;
        uint32_t written = 0;
        pgw_fusep_rep1 c;
        c.unique = u;
        c.a = &written;
        pgw_err rc = pgw_fusep_call(s, s->tx, (size_t)n, false, prio,
                            pgw_fusep_done_write, &c);
        if (rc != PGW_OK)
            return rc;
        if (written > chunk)
            return PGW_EIO; /* more written than sent: refuse the answer */
        total += written;
        chunks++;
        if (written < chunk)
            break; /* short write: report what the server accepted */
    }
    if (chunks > 1u)
        pgw_stat_add(PGW_STAT_SPLIT, 1);
    pgw_stat_add(PGW_STAT_BYTES_WRITTEN, (uint64_t)total);
    if (out_written != NULL)
        *out_written = total;
    return PGW_OK;
}

/* --- READDIR -------------------------------------------------------------- */

typedef struct rd_ctx {
    uint64_t unique;
    pgw_fuse_dirent *out;
    size_t max;
    size_t count;
    size_t body_len;
} rd_ctx;

static pgw_err rd_done(void *ctx, const void *rx, size_t rx_len)
{
    rd_ctx *c = (rd_ctx *)ctx;
    pgw_fuse_out_hdr h;
    pgw_err rc = pgw_fuse_dec_hdr(rx, rx_len, &h);
    if (rc != PGW_OK)
        return rc;
    c->body_len = (size_t)h.len - PGW_FUSE_OUT_HDR_SZ;
    return pgw_fuse_dec_readdir(rx, rx_len, c->unique, c->out, c->max,
                                &c->count);
}

static void rd_cache_reset(pgw_fuse_session *s)
{
    s->rd_valid = false;
    s->rd_count = 0;
    s->rd_pos = 0;
    s->rd_eof = false;
}

/* Ask the server for one chunk of directory entries at `off`. */
static pgw_err rd_fetch(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                        uint64_t off, uint32_t prio)
{
    uint8_t tx[PGW_FUSE_IN_HDR_SZ + PGW_FUSE_READ_IN_SIZE];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_read(tx, sizeof(tx), u, nodeid, fh, off,
                                  PGW_FUSE_READDIR_CHUNK, true);
    if (n <= 0)
        return PGW_EINVAL;
    rd_ctx c;
    c.unique = u;
    c.out = s->rd_cache;
    c.max = PGW_FUSE_READDIR_MAX;
    c.count = 0;
    c.body_len = 0;
    rd_cache_reset(s);
    pgw_err rc = pgw_fusep_call(s, tx, (size_t)n, false, prio, rd_done, &c);
    if (rc != PGW_OK)
        return rc;
    s->rd_count = c.count;
    s->rd_nodeid = nodeid;
    s->rd_fh = fh;
    s->rd_off_start = off;
    s->rd_valid = true;
    /* A reply smaller than the requested chunk is the end of the listing. */
    s->rd_eof = (c.count == 0) || (c.body_len < (size_t)PGW_FUSE_READDIR_CHUNK);
    return PGW_OK;
}

/* Try to serve `off` from the cached chunk. Returns true when the cache can
 * answer, and positions `rd_pos` on the first entry to hand out. */
static bool rd_seek(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                    uint64_t off)
{
    if (!s->rd_valid || s->rd_nodeid != nodeid || s->rd_fh != fh)
        return false;
    if (off == s->rd_off_start) {
        s->rd_pos = 0; /* the caller wants this chunk from the beginning */
        return true;
    }
    for (size_t k = 0; k < s->rd_count; k++) {
        if (s->rd_cache[k].off == off) {
            s->rd_pos = k + 1u; /* resume after the entry the caller saw */
            return true;
        }
    }
    return false;
}

pgw_err pgw_fuse_readdir(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                         uint64_t off, pgw_fuse_dirent *out, size_t max_entries,
                         size_t *out_count, uint32_t prio)
{
    if (out_count != NULL)
        *out_count = 0;
    if (s == NULL || (out == NULL && max_entries > 0))
        return PGW_EINVAL;
    if (max_entries == 0)
        return PGW_OK;

    size_t produced = 0;
    for (;;) {
        if (!rd_seek(s, nodeid, fh, off)) {
            pgw_err rc = rd_fetch(s, nodeid, fh, off, prio);
            if (rc != PGW_OK)
                return rc;
            if (s->rd_count == 0)
                break; /* end of directory */
        }
        size_t before = produced;
        while (s->rd_pos < s->rd_count && produced < max_entries)
            out[produced++] = s->rd_cache[s->rd_pos++];
        if (produced >= max_entries)
            break;
        s->rd_valid = false;
        if (s->rd_eof || produced == before || produced == 0)
            break; /* nothing more to fetch, or no progress possible */
        off = out[produced - 1u].off;
    }
    if (out_count != NULL)
        *out_count = produced;
    return PGW_OK;
}
