/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/prop/prop_p9_dec.c - the 9P2000.L reply round trip (gap 61).
 *
 * Requirements P9-02/P9-03 (every reply of the contract decodes back to the
 * values that were put in it) and P9-09 (a reply whose declared size, type,
 * tag or internal lengths disagree with the bytes received is refused).
 *
 * A reply is built here field by field with the same cursors the library writes
 * with (pgw_out), from values derived from the case knobs; the decoder then has
 * to hand back exactly those values. The comparison is therefore between the
 * values and not between two copies of the same bytes. The variable-length
 * replies (Rwalk, Rread, Rreadlink, Rversion) are the interesting ones: their
 * internal length fields are where a codec loses or invents a byte.
 */
#include "prop.h"

#include "pgw/buf.h"
#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

#define P9_BUF 2048u
#define P9_ITERS 2000u
#define P9_QIDS PGW_MAXWELEM
#define P9_DATA_MAX 512u

static uint8_t g_msg[P9_BUF];
static uint16_t g_tag;

/* The reply kinds of the round trip, and the wire type of each. */
enum {
    RP_LERROR = 0,
    RP_VERSION,
    RP_ATTACH,
    RP_MKDIR,
    RP_LOPEN,
    RP_READ,
    RP_WRITE,
    RP_WALK,
    RP_GETATTR,
    RP_STATFS,
    RP_READLINK,
    RP_CLUNK,
    RP_KINDS
};

static const uint8_t g_reply_type[RP_KINDS] = {
        PGW_P9_RLERROR,  PGW_P9_RVERSION, PGW_P9_RATTACH,   PGW_P9_RMKDIR,
        PGW_P9_RLOPEN,   PGW_P9_RREAD,    PGW_P9_RWRITE,    PGW_P9_RWALK,
        PGW_P9_RGETATTR, PGW_P9_RSTATFS,  PGW_P9_RREADLINK, PGW_P9_RCLUNK};

/* The values a reply carries, drawn from the case alone so that the generator
 * and the predicate always agree - and so that shrinking a knob shrinks the
 * reply that is rebuilt from it. */
typedef struct reply_vals {
    uint32_t num;
    uint32_t msize;
    uint8_t qt;
    uint32_t ver;
    uint64_t path;
    uint16_t nw;
    const uint8_t *data;
    uint32_t data_len;
    char str[40];
} reply_vals;

static void vals_from_case(const prop_case *c, reply_vals *v)
{
    memset(v, 0, sizeof(*v));
    v->num = c->a;
    v->msize = c->b;
    v->qt = (uint8_t)(c->value >> 24);
    v->ver = (uint32_t)c->value;
    v->path = c->value;
    v->nw = (uint16_t)(c->value % ((uint64_t)PGW_MAXWELEM + 1u));
    v->data = c->bytes;
    v->data_len = (uint32_t)c->len;
    prop_name(v->str, sizeof(v->str), (size_t)(c->value % 40u));
}

static size_t reply_begin(pgw_out *w, uint32_t kind)
{
    pgw_out_init(w, g_msg, sizeof(g_msg));
    pgw_put_u32(w, 0); /* size[4], patched by reply_end */
    pgw_put_u8(w, g_reply_type[kind]);
    pgw_put_u16(w, g_tag);
    return PGW_HEADER_SIZE;
}

static size_t reply_end(pgw_out *w)
{
    (void)pgw_patch_len_u32(w, 0);
    return pgw_written(w);
}

static void reply_qid(pgw_out *w, const reply_vals *v, uint32_t step)
{
    pgw_put_u8(w, v->qt);
    pgw_put_u32(w, v->ver + step);
    pgw_put_u64(w, v->path + step);
}

/* Replies whose body length depends on a field of the body itself. */
static size_t build_var(uint32_t kind, const reply_vals *v)
{
    pgw_out w;
    reply_begin(&w, kind);
    switch (kind) {
    case RP_LERROR:
        pgw_put_u32(&w, v->num);
        break;
    case RP_VERSION:
        pgw_put_u32(&w, v->msize);
        (void)pgw_put_str(&w, v->str, strlen(v->str));
        break;
    case RP_READ:
        pgw_put_u32(&w, v->data_len);
        (void)pgw_put_data(&w, v->data, v->data_len);
        break;
    case RP_WRITE:
        pgw_put_u32(&w, v->num);
        break;
    case RP_WALK:
        pgw_put_u16(&w, v->nw);
        for (uint16_t i = 0; i < v->nw; i++)
            reply_qid(&w, v, i);
        break;
    default: /* RP_READLINK: a length-prefixed target */
        (void)pgw_put_str(&w, v->str, strlen(v->str));
        break;
    }
    return reply_end(&w);
}

/* Replies whose body has one fixed layout. */
static size_t build_fixed(uint32_t kind, const reply_vals *v)
{
    pgw_out w;
    reply_begin(&w, kind);
    switch (kind) {
    case RP_ATTACH:
    case RP_MKDIR:
        reply_qid(&w, v, 0u);
        break;
    case RP_LOPEN:
        reply_qid(&w, v, 0u);
        pgw_put_u32(&w, v->num); /* iounit */
        break;
    case RP_GETATTR:
        pgw_put_u64(&w, PGW_P9_GETATTR_BASIC);
        reply_qid(&w, v, 0u);
        pgw_put_u32(&w, v->num);           /* mode */
        pgw_put_u32(&w, v->msize);         /* uid */
        pgw_put_u32(&w, v->qt);            /* gid */
        for (uint32_t i = 0; i < 15u; i++) /* nlink .. data_version */
            pgw_put_u64(&w, v->path + i);
        break;
    case RP_STATFS:
        pgw_put_u32(&w, v->num);          /* type */
        pgw_put_u32(&w, v->msize);        /* bsize */
        for (uint32_t i = 0; i < 6u; i++) /* blocks .. fsid */
            pgw_put_u64(&w, v->path + i);
        pgw_put_u32(&w, v->qt); /* namelen */
        break;
    default: /* RP_CLUNK: the header is the whole message */
        break;
    }
    return reply_end(&w);
}

static size_t build_reply(uint32_t kind, const reply_vals *v)
{
    switch (kind) {
    case RP_ATTACH:
    case RP_MKDIR:
    case RP_LOPEN:
    case RP_GETATTR:
    case RP_STATFS:
    case RP_CLUNK:
        return build_fixed(kind, v);
    default:
        return build_var(kind, v);
    }
}

/* --- verification --------------------------------------------------------- */

static bool same_qid(const pgw_p9_qid *q, const reply_vals *v, uint32_t step)
{
    return q->type == v->qt && q->version == v->ver + step && q->path == v->path + step;
}

static bool verify_var(uint32_t kind, size_t len, const reply_vals *v)
{
    uint32_t n32 = 0;
    uint32_t msize = 0;
    const uint8_t *data = NULL;
    pgw_p9_qid wq[P9_QIDS];
    uint16_t nw = 0;
    char str[64];
    memset(wq, 0, sizeof(wq));
    str[0] = '\0';
    switch (kind) {
    case RP_LERROR:
        return pgw_p9_dec_rlerror(g_msg, len, g_tag, &n32) == PGW_OK && n32 == v->num;
    case RP_VERSION:
        return pgw_p9_dec_rversion(g_msg, len, g_tag, &msize, str, sizeof(str)) ==
                       PGW_OK &&
               msize == v->msize && strcmp(str, v->str) == 0;
    case RP_READ:
        return pgw_p9_dec_rread(g_msg, len, g_tag, &n32, &data) == PGW_OK &&
               n32 == v->data_len && (n32 == 0u || memcmp(data, v->data, n32) == 0);
    case RP_WRITE:
        return pgw_p9_dec_rwrite(g_msg, len, g_tag, &n32) == PGW_OK && n32 == v->num;
    case RP_WALK:
        if (pgw_p9_dec_rwalk(g_msg, len, g_tag, &nw, wq, P9_QIDS) != PGW_OK)
            return false;
        if (nw != v->nw)
            return false;
        for (uint16_t i = 0; i < nw; i++) {
            if (!same_qid(&wq[i], v, i))
                return false;
        }
        return true;
    default: /* RP_READLINK */
        return pgw_p9_dec_rreadlink(g_msg, len, g_tag, str, sizeof(str)) == PGW_OK &&
               strcmp(str, v->str) == 0;
    }
}

static bool verify_fixed(uint32_t kind, size_t len, const reply_vals *v)
{
    pgw_p9_qid q;
    pgw_p9_attr a;
    pgw_p9_statfs_t s;
    uint32_t iounit = 0;
    memset(&q, 0, sizeof(q));
    memset(&a, 0, sizeof(a));
    memset(&s, 0, sizeof(s));
    switch (kind) {
    case RP_ATTACH:
        return pgw_p9_dec_rattach(g_msg, len, g_tag, &q) == PGW_OK && same_qid(&q, v, 0u);
    case RP_MKDIR:
        return pgw_p9_dec_rmkdir(g_msg, len, g_tag, &q) == PGW_OK && same_qid(&q, v, 0u);
    case RP_LOPEN:
        return pgw_p9_dec_rlopen(g_msg, len, g_tag, &q, &iounit) == PGW_OK &&
               same_qid(&q, v, 0u) && iounit == v->num;
    case RP_GETATTR:
        if (pgw_p9_dec_rgetattr(g_msg, len, g_tag, &a) != PGW_OK)
            return false;
        return a.valid == PGW_P9_GETATTR_BASIC && same_qid(&a.qid, v, 0u) &&
               a.mode == v->num && a.uid == v->msize && a.gid == v->qt &&
               a.nlink == v->path && a.ctime_nsec == v->path + 10u;
    case RP_STATFS:
        if (pgw_p9_dec_rstatfs(g_msg, len, g_tag, &s) != PGW_OK)
            return false;
        return s.type == v->num && s.bsize == v->msize && s.blocks == v->path &&
               s.fsid == v->path + 5u && s.namelen == v->qt;
    default: /* RP_CLUNK */
        return pgw_p9_dec_empty(g_msg, len, PGW_P9_RCLUNK, g_tag) == PGW_OK;
    }
}

/* --- the property --------------------------------------------------------- */

static void gen_reply(prop_rng *r, prop_case *c)
{
    c->a = prop_u32(r); /* which reply, and its first integer field */
    c->b = prop_u32(r); /* tag and second integer field */
    c->value = prop_u64(r);
    c->len = prop_below(r, P9_DATA_MAX);
    prop_fill(r, c, 0, c->len);
    snprintf(c->tag, sizeof(c->tag), "reply kind %u", c->a % RP_KINDS);
}

static bool pred_reply(const prop_case *c)
{
    reply_vals v;
    uint32_t kind = c->a % RP_KINDS;
    vals_from_case(c, &v);
    g_tag = (uint16_t)c->b;
    size_t len = build_reply(kind, &v);
    bool ok = (kind == RP_ATTACH || kind == RP_MKDIR || kind == RP_LOPEN ||
               kind == RP_GETATTR || kind == RP_STATFS || kind == RP_CLUNK) ?
                      verify_fixed(kind, len, &v) :
                      verify_var(kind, len, &v);
    /* The framing of a reply we just built must always be readable. */
    pgw_p9_hdr h;
    memset(&h, 0, sizeof(h));
    if (pgw_p9_dec_hdr(g_msg, len, &h) != PGW_OK || h.size != (uint32_t)len ||
        h.type != g_reply_type[kind] || h.tag != g_tag)
        return false;
    return ok;
}

static void test_p9_prop_reply_round_trip(void)
{
    uint32_t iters = prop_iters("PGW_PROP_ITERS", P9_ITERS);
    for (uint32_t k = 0; k < RP_KINDS; k++) {
        prop_forall("9p reply build -> decode round trip", gen_reply, pred_reply, iters,
                    0x14057B7EF767814FULL ^ k);
    }
}
PGW_TEST(test_p9_prop_reply_round_trip);
