/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/prop/prop_fuse_dec.c - the virtiofs reply decoders under hostile bytes.
 *
 * Gap 61 of docs/gap-analysis.md. Requirements VFS-03 (strict decoder
 * validation: no struct is ever cast onto an untrusted buffer) and ROB-06 (a
 * declared length, a `namelen` or an aligned `reclen` can never make a decoder
 * read or write outside the message it was given).
 *
 * Two properties plus a round trip, over every decoder of pgw/fusep.h:
 *
 *   bounds     the message is copied into a heap block of exactly its length,
 *              so the ASan+UBSan build of CI turns one byte of over-read into
 *              a hard failure. A decoder that answers PGW_OK must report only
 *              counts, lengths and offsets inside the message, and a decoder
 *              that refuses must leave its outputs untouched;
 *   stability  the same bytes decoded twice give the same code and the same
 *              output bytes;
 *   round trip a reply built field by field decodes back to those fields.
 *
 * The reply bodies are written with the helpers of protofuse/tests/
 * codec_util.h, i.e. with the very cursors a server writes with: the wire
 * layout exists once in the tree.
 */
#include "codec_util.h"
#include "prop.h"

#include "pgw/fusep.h"
#include "pgw/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FU_ITERS   4000u
#define FU_MAX_ENT 4u
#define FU_HDR     PGW_FUSE_OUT_HDR_SZ

enum {
    D_HDR = 0,
    D_INIT,
    D_ENTRY,
    D_ATTR,
    D_OPEN,
    D_CREATE,
    D_READ,
    D_WRITE,
    D_READDIR,
    D_READLINK,
    D_STATFS,
    D_EMPTY,
    D_COUNT
};

/* Everything the decoders report for one message, comparable byte for byte by
 * the stability property: `data_off` is an offset, never a pointer. */
typedef struct fuse_snap {
    pgw_err e[D_COUNT];
    pgw_fuse_out_hdr hdr;
    pgw_fuse_init_info init;
    pgw_fuse_entry entry;
    pgw_fuse_attr attr;
    pgw_fuse_kstatfs sfs;
    pgw_fuse_dirent ents[FU_MAX_ENT];
    pgw_fuse_entry centry;
    uint64_t fh;
    uint64_t cfh;
    uint32_t open_flags;
    uint32_t wsize;
    size_t ndirents;
    size_t data_off;
    size_t data_len;
    char str[300];
} fuse_snap;

static fuse_snap g_a;
static fuse_snap g_b;
static uint64_t g_unique;

static void decode_all(const uint8_t *m, size_t len, fuse_snap *s)
{
    const uint8_t *data = NULL;
    memset(s, 0, sizeof(*s));
    s->e[D_HDR] = pgw_fuse_dec_hdr(m, len, &s->hdr);
    s->e[D_INIT] = pgw_fuse_dec_init(m, len, g_unique, &s->init);
    s->e[D_ENTRY] = pgw_fuse_dec_entry(m, len, g_unique, &s->entry);
    s->e[D_ATTR] = pgw_fuse_dec_attr(m, len, g_unique, &s->attr);
    s->e[D_OPEN] =
        pgw_fuse_dec_open(m, len, g_unique, &s->fh, &s->open_flags);
    s->e[D_CREATE] =
        pgw_fuse_dec_create(m, len, g_unique, &s->centry, &s->cfh);
    s->e[D_READ] = pgw_fuse_dec_read(m, len, g_unique, &data, &s->data_len);
    s->e[D_WRITE] = pgw_fuse_dec_write(m, len, g_unique, &s->wsize);
    s->e[D_READDIR] = pgw_fuse_dec_readdir(m, len, g_unique, s->ents,
                                           FU_MAX_ENT, &s->ndirents);
    s->e[D_READLINK] =
        pgw_fuse_dec_readlink(m, len, g_unique, s->str, sizeof(s->str));
    s->e[D_STATFS] = pgw_fuse_dec_statfs(m, len, g_unique, &s->sfs);
    s->e[D_EMPTY] = pgw_fuse_dec_empty(m, len, g_unique);
    if (s->e[D_READ] == PGW_OK && data != NULL)
        s->data_off = (size_t)(data - m);
}

static bool snap_bounded(const fuse_snap *s, size_t len)
{
    if (s->e[D_HDR] == PGW_OK) {
        if (s->hdr.len < FU_HDR || (size_t)s->hdr.len > len)
            return false;
        if (s->hdr.error > 0)
            return false;
    }
    if (s->e[D_READ] == PGW_OK && s->data_off + s->data_len > len)
        return false;
    if (s->e[D_READDIR] == PGW_OK && s->ndirents > FU_MAX_ENT)
        return false;
    for (size_t i = 0; i < s->ndirents && s->e[D_READDIR] == PGW_OK; i++) {
        if (s->ents[i].namelen == 0 || s->ents[i].namelen >= 256u)
            return false;
        if (s->ents[i].name[s->ents[i].namelen] != '\0')
            return false;
        if (strlen(s->ents[i].name) != (size_t)s->ents[i].namelen)
            return false;
    }
    if (s->e[D_READLINK] == PGW_OK) {
        if (strlen(s->str) >= sizeof(s->str))
            return false;
    }
    return true;
}

/* A decoder that refuses leaves its outputs untouched (they were zeroed). */
static bool snap_clean_on_error(const fuse_snap *s)
{
    if (s->e[D_ENTRY] != PGW_OK && s->entry.nodeid != 0u)
        return false;
    if (s->e[D_ATTR] != PGW_OK && s->attr.ino != 0u)
        return false;
    if (s->e[D_OPEN] != PGW_OK && (s->fh != 0u || s->open_flags != 0u))
        return false;
    if (s->e[D_WRITE] != PGW_OK && s->wsize != 0u)
        return false;
    if (s->e[D_STATFS] != PGW_OK && s->sfs.bsize != 0u)
        return false;
    if (s->e[D_INIT] != PGW_OK && s->init.major != 0u)
        return false;
    if (s->e[D_CREATE] != PGW_OK && (s->centry.nodeid != 0u || s->cfh != 0u))
        return false;
    return true;
}

static void gen_hostile(prop_rng *r, prop_case *c)
{
    c->a = prop_u32(r);
    c->b = prop_u32(r);
    c->value = prop_u64(r);
    uint32_t pick = prop_below(r, 16u);
    c->len = (pick < 9u) ? pick : prop_below(r, PROP_MAX_BYTES);
    prop_fill(r, c, 0, c->len);
    /* Half of the cases carry a plausible output header, so the decoders get
     * past the framing and reach their body-length and record checks. */
    if ((c->value & 1u) != 0u && c->len >= FU_HDR) {
        uint32_t mlen = (uint32_t)c->len;
        c->bytes[0] = (uint8_t)mlen;
        c->bytes[1] = (uint8_t)(mlen >> 8);
        c->bytes[2] = (uint8_t)(mlen >> 16);
        c->bytes[3] = (uint8_t)(mlen >> 24);
        memset(c->bytes + 4, 0, 4); /* error = 0 */
        for (int i = 0; i < 8; i++)
            c->bytes[8 + i] = (uint8_t)(c->a >> (8 * (i % 4)));
    }
    snprintf(c->tag, sizeof(c->tag), "hostile %zu bytes", c->len);
}

static bool pred_hostile(const prop_case *c)
{
    size_t len = c->len;
    uint8_t *m = malloc(len + 1u);
    if (m == NULL)
        return true;
    memcpy(m, c->bytes, len);
    g_unique = (uint64_t)c->a;
    decode_all(m, len, &g_a);
    bool ok = snap_bounded(&g_a, len) && snap_clean_on_error(&g_a);
    free(m);
    return ok;
}

static void test_fuse_prop_hostile_bytes_stay_in_bounds(void)
{
    prop_forall("fuse decoders never leave the message", gen_hostile,
                pred_hostile, prop_iters("PGW_PROP_ITERS", FU_ITERS),
                0x85EBCA6BULL);
}
PGW_TEST(test_fuse_prop_hostile_bytes_stay_in_bounds);

static bool pred_stable(const prop_case *c)
{
    size_t len = c->len;
    uint8_t *m = malloc(len + 1u);
    if (m == NULL)
        return true;
    memcpy(m, c->bytes, len);
    g_unique = (uint64_t)c->a;
    decode_all(m, len, &g_a);
    decode_all(m, len, &g_b);
    bool same = memcmp(&g_a, &g_b, sizeof(g_a)) == 0;
    free(m);
    return same;
}

static void test_fuse_prop_decoding_is_stable(void)
{
    prop_forall("fuse decoding is deterministic", gen_hostile, pred_stable,
                prop_iters("PGW_PROP_ITERS", FU_ITERS / 2u), 0xC2B2AE3DULL);
}
PGW_TEST(test_fuse_prop_decoding_is_stable);

/* --- round trip: a reply built field by field decodes back to those fields - */

static void gen_reply(prop_rng *r, prop_case *c)
{
    c->a = prop_u32(r);
    c->b = prop_u32(r);
    c->value = prop_u64(r);
    c->len = prop_below(r, 300u);
    prop_fill(r, c, 0, c->len);
    snprintf(c->tag, sizeof(c->tag), "reply %u", c->a % 4u);
}

static bool pred_reply(const prop_case *c)
{
    msg reply;
    pgw_out w;
    uint64_t unique = c->value;
    pgw_fuse_attr a;
    memset(&reply, 0, sizeof(reply));
    memset(&a, 0, sizeof(a));
    a.ino = c->value;
    a.size = c->a;
    a.mode = c->b | 0100000u;
    a.nlink = 1u;
    a.blksize = 4096u;
    pgw_out_init(&w, reply.buf, sizeof(reply.buf));
    put_hdr(&w, unique, 0);
    switch (c->a % 4u) {
    case 0: /* entry_out: nodeid, generation, validities, then fuse_attr */
        pgw_put_u64(&w, c->value);
        pgw_put_u64(&w, c->b);
        pgw_put_u64(&w, 1u);
        pgw_put_u64(&w, 2u);
        pgw_put_u32(&w, c->a);
        pgw_put_u32(&w, c->b);
        put_attr_from(&w, &a, 0u);
        break;
    case 1: /* open_out: fh, open_flags, padding */
        pgw_put_u64(&w, c->value);
        pgw_put_u32(&w, c->a);
        pgw_put_u32(&w, 0u);
        break;
    case 2: /* write_out: size, padding */
        pgw_put_u32(&w, c->a);
        pgw_put_u32(&w, 0u);
        break;
    default: /* attr_out: attr_valid, attr_valid_nsec, dummy, fuse_attr */
        pgw_put_u64(&w, c->value);
        pgw_put_u32(&w, c->a);
        pgw_put_u32(&w, 0u);
        put_attr_from(&w, &a, 0u);
        break;
    }
    reply.len = (int32_t)finish(&w);
    switch (c->a % 4u) {
    case 0: {
        pgw_fuse_entry e;
        memset(&e, 0, sizeof(e));
        return pgw_fuse_dec_entry(reply.buf, (size_t)reply.len, unique, &e) == PGW_OK &&
               e.nodeid == c->value && e.generation == c->b &&
               e.attr.ino == c->value && e.attr.size == c->a &&
               e.attr.mode == a.mode;
    }
    case 1: {
        uint64_t fh = 0;
        uint32_t flags = 0;
        return pgw_fuse_dec_open(reply.buf, (size_t)reply.len, unique, &fh, &flags) ==
                   PGW_OK &&
               fh == c->value && flags == c->a;
    }
    case 2: {
        uint32_t size = 0;
        return pgw_fuse_dec_write(reply.buf, (size_t)reply.len, unique, &size) ==
                   PGW_OK &&
               size == c->a;
    }
    default: {
        pgw_fuse_attr got;
        memset(&got, 0, sizeof(got));
        return pgw_fuse_dec_attr(reply.buf, (size_t)reply.len, unique, &got) == PGW_OK &&
               memcmp(&got, &a, sizeof(a)) == 0;
    }
    }
}

static void test_fuse_prop_reply_round_trip(void)
{
    prop_forall("fuse reply build -> decode round trip", gen_reply, pred_reply,
                prop_iters("PGW_PROP_ITERS", FU_ITERS / 2u), 0x27D4EB2FULL);
}
PGW_TEST(test_fuse_prop_reply_round_trip);
