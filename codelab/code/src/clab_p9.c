/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_p9.c - chapter 7's exercise, implementation.
 *
 * Two habits this file is meant to teach:
 *
 *  - encoders never write past the capacity they were given, and they patch the
 *    size field at the end, once the body length is known;
 *  - decoders validate before they read. clab_p9_check() is the only way to
 *    obtain a clab_p9_msg, and it checks the four things that matter: the size
 *    field agrees with the bytes received, the type is the reply we asked for,
 *    the tag is the one we sent, and the body length is consistent.
 *
 * Every integer is written and read byte by byte: no struct is ever cast onto a
 * message buffer, so alignment and endianness cannot bite (the same rule the
 * project's pgw_buf cursors follow).
 */
#include "clab_p9.h"

#include <string.h>

/* --- writer --------------------------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t cap;
    size_t pos;
    bool fail;
} writer;

static void put8(writer *w, uint8_t v)
{
    if (w->pos + 1u > w->cap) {
        w->fail = true;
        return;
    }
    w->b[w->pos++] = v;
}

static void put16(writer *w, uint16_t v)
{
    put8(w, (uint8_t)(v & 0xFFu));
    put8(w, (uint8_t)(v >> 8u));
}

static void put32(writer *w, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++)
        put8(w, (uint8_t)(v >> (8u * i)));
}

static void put64(writer *w, uint64_t v)
{
    for (unsigned i = 0; i < 8u; i++)
        put8(w, (uint8_t)(v >> (8u * i)));
}

static void put_str(writer *w, const char *s)
{
    const size_t n = strlen(s);

    if (n > 0xFFFFu) {
        w->fail = true;
        return;
    }
    put16(w, (uint16_t)n);
    for (size_t i = 0; i < n; i++)
        put8(w, (uint8_t)s[i]);
}

/* Start a message: 4 placeholder bytes for the size, then type and tag. */
static void begin(writer *w, uint8_t type, uint16_t tag)
{
    put32(w, 0);
    put8(w, type);
    put16(w, tag);
}

/* Patch the size field and return the message length, or a negative error. */
static int finish(writer *w)
{
    if (w->fail || w->pos > 0xFFFFFFFFu)
        return CLAB_ERR_INVAL;
    for (unsigned i = 0; i < 4u; i++)
        w->b[i] = (uint8_t)((uint32_t)w->pos >> (8u * i));
    return (int)w->pos;
}

static void init(writer *w, uint8_t *buf, size_t cap)
{
    w->b = buf;
    w->cap = cap;
    w->pos = 0;
    w->fail = buf == NULL || cap < CLAB_P9_HDR;
}

int clab_p9_enc_tversion(uint8_t *buf, size_t cap, uint16_t tag, uint32_t msize,
                         const char *version)
{
    writer w;
    init(&w, buf, cap);
    begin(&w, CLAB_P9_TVERSION, tag);
    put32(&w, msize);
    put_str(&w, version);
    return finish(&w);
}

int clab_p9_enc_tattach(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                        const char *uname, const char *aname)
{
    writer w;
    init(&w, buf, cap);
    begin(&w, CLAB_P9_TATTACH, tag);
    put32(&w, fid);
    put32(&w, CLAB_P9_NOFID); /* afid: no authentication */
    put_str(&w, uname);
    put_str(&w, aname);
    put32(&w, 0); /* n_uname, unused with the "none" security model */
    return finish(&w);
}

int clab_p9_enc_twalk(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                      uint32_t newfid, uint16_t nwname, const char *const *names)
{
    writer w;
    init(&w, buf, cap);
    begin(&w, CLAB_P9_TWALK, tag);
    put32(&w, fid);
    put32(&w, newfid);
    put16(&w, nwname);
    for (uint16_t i = 0; i < nwname && !w.fail; i++)
        put_str(&w, names[i]);
    return finish(&w);
}

int clab_p9_enc_tlopen(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                       uint32_t flags)
{
    writer w;
    init(&w, buf, cap);
    begin(&w, CLAB_P9_TLOPEN, tag);
    put32(&w, fid);
    put32(&w, flags); /* Linux O_* values, not QNX ones */
    return finish(&w);
}

int clab_p9_enc_tread(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                      uint64_t off, uint32_t count)
{
    writer w;
    init(&w, buf, cap);
    begin(&w, CLAB_P9_TREAD, tag);
    put32(&w, fid);
    put64(&w, off);
    put32(&w, count);
    return finish(&w);
}

/* --- validation ----------------------------------------------------------- */

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

clab_p9_err clab_p9_check(const uint8_t *buf, size_t len, uint8_t want_type,
                          uint16_t want_tag, clab_p9_msg *out)
{
    if (buf == NULL || len < CLAB_P9_HDR)
        return CLAB_ERR_SHORT;
    const uint32_t declared = get_le32(buf);
    /* The two must agree exactly: a reply shorter than its header claims is a
     * truncation, a reply longer is padding, and both mean the stream is out
     * of step. */
    if (declared != len)
        return CLAB_ERR_SIZE;
    if (buf[4] != want_type)
        return CLAB_ERR_TYPE;
    const uint16_t tag = (uint16_t)((uint16_t)buf[5] | ((uint16_t)buf[6] << 8u));
    if (tag != want_tag)
        return CLAB_ERR_TAG;
    out->body = buf + CLAB_P9_HDR;
    out->len = len - CLAB_P9_HDR;
    out->type = buf[4];
    out->tag = tag;
    return CLAB_OK;
}

/* --- reader --------------------------------------------------------------- */

void clab_p9_rd_init(clab_p9_rd *r, const clab_p9_msg *m)
{
    r->p = m->body;
    r->len = m->len;
    r->pos = 0;
    r->fail = false;
}

static bool need(clab_p9_rd *r, size_t n)
{
    if (r->fail || r->pos + n > r->len) {
        r->fail = true;
        return false;
    }
    return true;
}

uint8_t clab_p9_get8(clab_p9_rd *r)
{
    if (!need(r, 1))
        return 0;
    return r->p[r->pos++];
}

uint16_t clab_p9_get16(clab_p9_rd *r)
{
    const uint16_t lo = clab_p9_get8(r);
    const uint16_t hi = clab_p9_get8(r);
    return (uint16_t)(lo | (uint16_t)(hi << 8u));
}

uint32_t clab_p9_get32(clab_p9_rd *r)
{
    if (!need(r, 4))
        return 0;
    const uint32_t v = get_le32(r->p + r->pos);
    r->pos += 4;
    return v;
}

uint64_t clab_p9_get64(clab_p9_rd *r)
{
    const uint64_t lo = clab_p9_get32(r);
    const uint64_t hi = clab_p9_get32(r);
    return lo | (hi << 32u);
}

bool clab_p9_get_str(clab_p9_rd *r, char *dst, size_t cap, uint16_t *out_len)
{
    const uint16_t n = clab_p9_get16(r);

    if (r->fail || !need(r, n) || (size_t)n + 1u > cap) {
        r->fail = true;
        return false;
    }
    memcpy(dst, r->p + r->pos, n);
    dst[n] = '\0';
    r->pos += n;
    if (out_len != NULL)
        *out_len = n;
    return true;
}

bool clab_p9_rd_done(const clab_p9_rd *r)
{
    return !r->fail && r->pos == r->len;
}

clab_p9_err clab_p9_dec_rversion(const clab_p9_msg *m, uint32_t *msize, char *ver,
                                 size_t ver_cap)
{
    clab_p9_rd r;

    clab_p9_rd_init(&r, m);
    *msize = clab_p9_get32(&r);
    if (!clab_p9_get_str(&r, ver, ver_cap, NULL))
        return CLAB_ERR_INVAL;
    if (!clab_p9_rd_done(&r))
        return CLAB_ERR_TRAILING;
    return CLAB_OK;
}

clab_p9_err clab_p9_dec_rread(const clab_p9_msg *m, const uint8_t **data,
                              uint32_t *count)
{
    clab_p9_rd r;

    clab_p9_rd_init(&r, m);
    *count = clab_p9_get32(&r);
    if (r.fail || (size_t)*count != r.len - r.pos)
        return CLAB_ERR_SIZE;
    *data = r.p + r.pos; /* zero-copy: the reply buffer is the caller's */
    return CLAB_OK;
}

clab_p9_err clab_p9_dec_rlerror(const clab_p9_msg *m, uint32_t *linux_errno)
{
    clab_p9_rd r;

    clab_p9_rd_init(&r, m);
    *linux_errno = clab_p9_get32(&r);
    if (!clab_p9_rd_done(&r))
        return CLAB_ERR_TRAILING;
    return CLAB_ERR_REMOTE;
}
