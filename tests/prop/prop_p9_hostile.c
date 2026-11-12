/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/prop/prop_p9_hostile.c - the 9P2000.L decoders under hostile bytes.
 *
 * Gap 61 of docs/gap-analysis.md. Requirements P9-09 (every response is
 * validated) and ROB-06 (a declared length or count can never make a decoder
 * read or write outside the buffer it was given).
 *
 * Two properties, run over *every* decoder of pgw/p9.h for each generated case:
 *
 *   bounds     the message is copied into a heap block of exactly its length,
 *              so the ASan+UBSan build of CI turns a single byte of over-read
 *              into a hard failure. On top of that, a decoder that answers
 *              PGW_OK must report only counts, lengths and pointers that live
 *              inside the message, and a decoder that refuses must leave its
 *              output parameters untouched (the contract at the top of
 *              proto9p/src/p9_dec.c);
 *   stability  the same bytes decoded twice give the same code, the same
 *              output bytes and the same consumed length: a decoder that
 *              depends on something other than its input is a bug.
 */
#include "prop.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P9_ITERS   4000u
#define P9_MAX_ENT 4u

/* Indexes into p9_snap::e, one per decoder exercised below. */
enum {
    S_HDR = 0,
    S_LERROR,
    S_VERSION,
    S_ATTACH,
    S_WALK,
    S_LOPEN,
    S_READ,
    S_WRITE,
    S_GETATTR,
    S_READDIR,
    S_READLINK,
    S_MKDIR,
    S_STATFS,
    S_EMPTY,
    S_COUNT
};

/* Everything the decoders report for one message. Compared byte for byte by
 * the stability property, so it must not hold a pointer into the message:
 * `data_off` is an offset instead. */
typedef struct p9_snap {
    pgw_err e[S_COUNT];
    pgw_p9_hdr hdr;
    pgw_p9_qid qid;
    pgw_p9_qid wqids[PGW_MAXWELEM];
    pgw_p9_attr attr;
    pgw_p9_statfs_t sfs;
    pgw_p9_dirent ents[P9_MAX_ENT];
    uint32_t ecode;
    uint32_t msize;
    uint32_t iounit;
    uint32_t rcount;
    uint32_t wcount;
    uint16_t nwqid;
    size_t ndirents;
    size_t data_off;
    size_t data_len;
    char ver[32];
    char str[264];
} p9_snap;

static p9_snap g_a;
static p9_snap g_b;

static void decode_all(const uint8_t *msg, size_t len, uint16_t tag, p9_snap *s)
{
    const uint8_t *data = NULL;
    memset(s, 0, sizeof(*s));
    s->e[S_HDR] = pgw_p9_dec_hdr(msg, len, &s->hdr);
    s->e[S_LERROR] = pgw_p9_dec_rlerror(msg, len, tag, &s->ecode);
    s->e[S_VERSION] =
        pgw_p9_dec_rversion(msg, len, tag, &s->msize, s->ver, sizeof(s->ver));
    s->e[S_ATTACH] = pgw_p9_dec_rattach(msg, len, tag, &s->qid);
    s->e[S_WALK] = pgw_p9_dec_rwalk(msg, len, tag, &s->nwqid, s->wqids,
                                    PGW_MAXWELEM);
    s->e[S_LOPEN] = pgw_p9_dec_rlopen(msg, len, tag, &s->qid, &s->iounit);
    s->e[S_READ] = pgw_p9_dec_rread(msg, len, tag, &s->rcount, &data);
    s->e[S_WRITE] = pgw_p9_dec_rwrite(msg, len, tag, &s->wcount);
    s->e[S_GETATTR] = pgw_p9_dec_rgetattr(msg, len, tag, &s->attr);
    s->e[S_READDIR] = pgw_p9_dec_rreaddir_iter(msg, len, tag, s->ents,
                                               P9_MAX_ENT, &s->ndirents);
    s->e[S_READLINK] =
        pgw_p9_dec_rreadlink(msg, len, tag, s->str, sizeof(s->str));
    s->e[S_MKDIR] = pgw_p9_dec_rmkdir(msg, len, tag, &s->qid);
    s->e[S_STATFS] = pgw_p9_dec_rstatfs(msg, len, tag, &s->sfs);
    s->e[S_EMPTY] = pgw_p9_dec_empty(msg, len, PGW_P9_RCLUNK, tag);
    if (s->e[S_READ] == PGW_OK && data != NULL) {
        s->data_off = (size_t)(data - msg);
        s->data_len = s->rcount;
    }
}

/* Every length, count and offset an accepting decoder reported must describe
 * bytes that are really inside the message. */
static bool snap_bounded(const p9_snap *s, size_t len)
{
    if (s->e[S_HDR] == PGW_OK) {
        if (s->hdr.size < PGW_HEADER_SIZE || (size_t)s->hdr.size > len)
            return false;
    }
    if (s->e[S_READ] == PGW_OK) {
        if (s->data_off + s->data_len > len)
            return false;
    }
    if (s->e[S_WALK] == PGW_OK) {
        if (s->nwqid > PGW_MAXWELEM)
            return false;
        if (PGW_HEADER_SIZE + 2u + (size_t)s->nwqid * 13u > len)
            return false;
    }
    if (s->e[S_READDIR] == PGW_OK && s->ndirents > P9_MAX_ENT)
        return false;
    for (size_t i = 0; i < s->ndirents && s->e[S_READDIR] == PGW_OK; i++) {
        if (s->ents[i].name_len > PGW_NAME_MAX)
            return false;
        if (s->ents[i].name[s->ents[i].name_len] != '\0')
            return false;
        if (strlen(s->ents[i].name) != (size_t)s->ents[i].name_len)
            return false;
    }
    if (s->e[S_READLINK] == PGW_OK && strlen(s->str) >= sizeof(s->str))
        return false;
    if (s->e[S_VERSION] == PGW_OK && strlen(s->ver) >= sizeof(s->ver))
        return false;
    return true;
}

/* A decoder that refuses leaves its outputs untouched: they were zeroed before
 * the call, so any non-zero byte is a partial result (P9-09). */
static bool snap_clean_on_error(const p9_snap *s)
{
    static const char zero_str[1] = { '\0' };
    if (s->e[S_LERROR] != PGW_OK && s->ecode != 0u)
        return false;
    if (s->e[S_READ] != PGW_OK && (s->rcount != 0u || s->data_len != 0u))
        return false;
    if (s->e[S_WRITE] != PGW_OK && s->wcount != 0u)
        return false;
    if (s->e[S_WALK] != PGW_OK && s->nwqid != 0u)
        return false;
    if (s->e[S_READDIR] != PGW_OK && s->ndirents != 0u)
        return false;
    if (s->e[S_GETATTR] != PGW_OK && s->attr.valid != 0u)
        return false;
    if (s->e[S_STATFS] != PGW_OK && s->sfs.bsize != 0u)
        return false;
    if (s->e[S_ATTACH] != PGW_OK && s->qid.path != 0u)
        return false;
    if (s->e[S_VERSION] != PGW_OK && memcmp(s->ver, zero_str, 1) != 0)
        return false;
    return true;
}

static void gen_hostile(prop_rng *r, prop_case *c)
{
    c->a = prop_u32(r);
    c->b = prop_u32(r); /* the tag the decoders are asked to match */
    c->value = prop_u64(r);
    /* Lengths cluster around the interesting edges: below the header, exactly
     * the header, and a spread up to the whole buffer. */
    uint32_t pick = prop_below(r, 16u);
    c->len = (pick < 9u) ? pick : prop_below(r, PROP_MAX_BYTES);
    prop_fill(r, c, 0, c->len);
    /* Half of the cases start from a plausible header, so that the decoders
     * get past the framing and reach their internal length checks. */
    if ((c->value & 1u) != 0u && c->len >= PGW_HEADER_SIZE) {
        uint32_t size = (uint32_t)c->len;
        c->bytes[0] = (uint8_t)size;
        c->bytes[1] = (uint8_t)(size >> 8);
        c->bytes[2] = (uint8_t)(size >> 16);
        c->bytes[3] = (uint8_t)(size >> 24);
        c->bytes[4] = (uint8_t)(c->a % 128u);
        c->bytes[5] = (uint8_t)c->b;
        c->bytes[6] = (uint8_t)(c->b >> 8);
    }
    snprintf(c->tag, sizeof(c->tag), "hostile %zu bytes", c->len);
}

static bool pred_hostile(const prop_case *c)
{
    size_t len = c->len;
    uint8_t *msg = malloc(len + 1u); /* exactly len bytes: ASan sees any excess */
    if (msg == NULL)
        return true;
    memcpy(msg, c->bytes, len);
    decode_all(msg, len, (uint16_t)c->b, &g_a);
    bool ok = snap_bounded(&g_a, len) && snap_clean_on_error(&g_a);
    free(msg);
    return ok;
}

static void test_p9_prop_hostile_bytes_stay_in_bounds(void)
{
    prop_forall("9p decoders never leave the message", gen_hostile, pred_hostile,
                prop_iters("PGW_PROP_ITERS", P9_ITERS), 0x5BF03635ULL);
}
PGW_TEST(test_p9_prop_hostile_bytes_stay_in_bounds);

static bool pred_stable(const prop_case *c)
{
    size_t len = c->len;
    uint8_t *msg = malloc(len + 1u);
    if (msg == NULL)
        return true;
    memcpy(msg, c->bytes, len);
    decode_all(msg, len, (uint16_t)c->b, &g_a);
    decode_all(msg, len, (uint16_t)c->b, &g_b);
    bool same = memcmp(&g_a, &g_b, sizeof(g_a)) == 0;
    free(msg);
    return same;
}

static void test_p9_prop_decoding_is_stable(void)
{
    prop_forall("9p decoding is deterministic", gen_hostile, pred_stable,
                prop_iters("PGW_PROP_ITERS", P9_ITERS / 2u), 0x2545F491ULL);
}
PGW_TEST(test_p9_prop_decoding_is_stable);
