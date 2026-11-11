/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_session_io.c - chunked read, write and readdir (P9-08).
 *
 * A 9P message cannot carry more than `msize` bytes, so a transfer larger than
 * one message must be split into several requests and resumed at the offset
 * the previous reply stopped at. That is done here, once, for the three
 * operations that need it:
 *
 *   - read:  Tread(offset, chunk) until the caller's buffer is full or the
 *            server returns 0 bytes (end of file);
 *   - write: Twrite(offset, chunk) until every byte is accepted; a server may
 *            accept less than asked, the remainder is re-sent at the new
 *            offset;
 *   - readdir: Treaddir(cookie, chunk) until the caller's array is full or the
 *            server returns an empty reply, keeping the offset cookie of the
 *            last entry handed out so that no entry is lost or repeated.
 *
 * The payload of one message is at most msize - PGW_P9_IO_OVERHEAD, which
 * covers both directions (Twrite overhead 23, Rread overhead 11). Every
 * request goes through the same RPC callback, so a split transfer costs one
 * round trip per chunk and nothing else: the count is visible in the shared
 * pgw_stat_* table (PGW_STAT_SPLIT, PGW_STAT_BYTES_*), which is what PERF-01
 * measures.
 */
#include "p9_session_priv.h"

#include "pgw/log.h"

#include <string.h>

/* One Tread, copying the payload into `dst` before the slot is released. */
static pgw_err read_once(pgw_p9_session *s, uint32_t fid, uint64_t off,
                         uint32_t want, void *dst, uint32_t *got, uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_tread(pgw_p9__call_tx(&c), c.s->msize,
                                   (uint16_t)c.tag, fid, off, want);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RREAD, prio, &mlen);
    if (e == PGW_OK) {
        uint32_t n = 0;
        const uint8_t *data = NULL;
        e = pgw_p9_dec_rread(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, &n,
                             &data);
        if (e == PGW_OK) {
            if (n > want)
                e = PGW_EIO; /* a server must not return more than asked */
            else if (n > 0)
                memcpy(dst, data, n);
        }
        *got = (e == PGW_OK) ? n : 0;
    }
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_read(pgw_p9_session *s, uint32_t fid, uint64_t off, void *buf,
                    size_t len, size_t *out_read, uint32_t prio)
{
    if (out_read != NULL)
        *out_read = 0;
    if (s == NULL || (buf == NULL && len > 0))
        return PGW_EINVAL;
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    unsigned calls = 0;
    uint32_t chunk = pgw_p9__chunk(s);
    pgw_err e = PGW_OK;
    while (done < len) {
        size_t want = len - done;
        if (want > (size_t)chunk)
            want = (size_t)chunk;
        uint32_t got = 0;
        e = read_once(s, fid, off + (uint64_t)done, (uint32_t)want, dst + done,
                      &got, prio);
        if (e != PGW_OK)
            break;
        done += got;
        calls++;
        if (got == 0)
            break; /* end of file */
    }
    if (calls > 1)
        pgw_stat_add(PGW_STAT_SPLIT, 1);
    pgw_stat_add(PGW_STAT_BYTES_READ, done);
    if (out_read != NULL)
        *out_read = done;
    return e;
}

/* One Twrite. `done` receives the number of bytes the server accepted. */
static pgw_err write_once(pgw_p9_session *s, uint32_t fid, uint64_t off,
                          const void *src, uint32_t want, uint32_t *done,
                          uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_twrite(pgw_p9__call_tx(&c), c.s->msize,
                                    (uint16_t)c.tag, fid, off, src, want);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RWRITE, prio, &mlen);
    if (e == PGW_OK) {
        uint32_t n = 0;
        e = pgw_p9_dec_rwrite(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, &n);
        if (e == PGW_OK && n > want)
            e = PGW_EIO;
        *done = (e == PGW_OK) ? n : 0;
    }
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_write(pgw_p9_session *s, uint32_t fid, uint64_t off,
                     const void *buf, size_t len, size_t *out_written,
                     uint32_t prio)
{
    if (out_written != NULL)
        *out_written = 0;
    if (s == NULL || (buf == NULL && len > 0))
        return PGW_EINVAL;
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    unsigned calls = 0;
    uint32_t chunk = pgw_p9__chunk(s);
    pgw_err e = PGW_OK;
    while (done < len) {
        size_t want = len - done;
        if (want > (size_t)chunk)
            want = (size_t)chunk;
        uint32_t sent = 0;
        e = write_once(s, fid, off + (uint64_t)done, src + done,
                       (uint32_t)want, &sent, prio);
        if (e != PGW_OK)
            break;
        calls++;
        if (sent == 0) {
            e = PGW_EIO; /* the server accepted nothing: no progress */
            break;
        }
        done += sent;
    }
    if (calls > 1)
        pgw_stat_add(PGW_STAT_SPLIT, 1);
    pgw_stat_add(PGW_STAT_BYTES_WRITTEN, done);
    if (out_written != NULL)
        *out_written = done;
    return e;
}

/* Decode every record of one Rreaddir payload. Records that do not fit in
 * `out` are validated then dropped: the caller resumes from the last offset it
 * kept, so nothing is lost and no record goes unchecked (P9-09). */
static pgw_err consume_entries(const void *msg, size_t len, uint16_t tag,
                               pgw_p9_dirent *out, size_t max_entries,
                               size_t *count, uint64_t *cursor, bool *end)
{
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
    if (n == 0) {
        *end = true;
        return PGW_OK;
    }
    pgw_buf d;
    pgw_buf_init(&d, data, n);
    pgw_p9_dirent scratch;
    while (pgw_left(&d) > 0) {
        bool room = *count < max_entries;
        pgw_p9_dirent *slot = room ? &out[*count] : &scratch;
        if (!pgw_p9__dirent_next(&d, slot))
            return PGW_EINVAL;
        if (room) {
            *cursor = slot->offset;
            (*count)++;
        }
    }
    return PGW_OK;
}

/* One Treaddir, feeding `count` entries and advancing `cursor`. */
static pgw_err readdir_once(pgw_p9_session *s, uint32_t fid, uint64_t *cursor,
                            uint32_t want, pgw_p9_dirent *out,
                            size_t max_entries, size_t *count, bool *end,
                            uint32_t prio)
{
    struct pgw_p9_call c;
    pgw_err e = pgw_p9__call_begin(s, &c);
    if (e != PGW_OK)
        return e;
    int32_t enc = pgw_p9_enc_treaddir(pgw_p9__call_tx(&c), c.s->msize,
                                      (uint16_t)c.tag, fid, *cursor, want);
    size_t mlen = 0;
    e = pgw_p9__call_send(&c, enc, PGW_P9_RREADDIR, prio, &mlen);
    if (e == PGW_OK)
        e = consume_entries(pgw_p9__call_rx(&c), mlen, (uint16_t)c.tag, out,
                            max_entries, count, cursor, end);
    pgw_p9__call_end(&c);
    return e;
}

pgw_err pgw_p9_readdir(pgw_p9_session *s, uint32_t fid, uint64_t off,
                       pgw_p9_dirent *out, size_t max_entries,
                       size_t *out_count, uint64_t *next_off, uint32_t prio)
{
    if (out_count != NULL)
        *out_count = 0;
    if (next_off != NULL)
        *next_off = off;
    if (s == NULL || (out == NULL && max_entries > 0))
        return PGW_EINVAL;
    uint64_t cursor = off;
    size_t count = 0;
    unsigned calls = 0;
    bool end = false;
    uint32_t chunk = pgw_p9__chunk(s);
    pgw_err e = PGW_OK;
    while (count < max_entries && !end) {
        size_t before = count;
        e = readdir_once(s, fid, &cursor, chunk, out, max_entries, &count, &end,
                         prio);
        if (e != PGW_OK)
            break;
        calls++;
        if (!end && count == before) {
            e = PGW_EIO; /* the server is not advancing the cookie */
            break;
        }
    }
    if (calls > 1)
        pgw_stat_add(PGW_STAT_SPLIT, 1);
    if (out_count != NULL)
        *out_count = count;
    /* A zero cookie means "the directory is exhausted"; any other value is the
     * cookie of the last entry handed out, to be passed as `off` next time. */
    if (next_off != NULL)
        *next_off = end ? 0 : cursor;
    return e;
}
