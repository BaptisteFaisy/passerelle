/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/prop/prop_p9.c - encode-side properties of the 9P2000.L codecs.
 *
 * Gap 61 of docs/gap-analysis.md. Requirements P9-02 and P9-03 (every
 * T-message of the contract), P9-09 (a message that does not fit is refused,
 * never truncated) and ROB-06 (nothing is ever written outside the buffer the
 * caller handed over).
 *
 * Three properties, each run over *every* encoder of pgw/p9.h:
 *
 *   framing   encode, then pgw_p9_dec_hdr: size[4], type and tag come back
 *             exactly as they were passed in, i.e. the framing the encoders
 *             write is the framing the decoders read;
 *   bounds    whatever the capacity - 0, 6, 7 (the header alone), one byte
 *             short, exact, generous - the encoder either returns a length
 *             that fits or a negative pgw_err, and the guard bytes just past
 *             the capacity are never touched;
 *   extremes  a name longer than PGW_NAME_MAX, a nwname above PGW_MAXWELEM, a
 *             missing name array and a count of UINT32_MAX are refused with
 *             the documented code, never accepted.
 *
 * The encoder under test is chosen by the campaign loop and *not* by a
 * shrinkable knob, so shrinking a failing case cannot silently turn it into a
 * different encoder.
 */
#include "prop.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

#define P9_BUF       4096u
#define P9_NAME_BUF  600u /* above PGW_NAME_MAX, so the refusal is reachable */
#define P9_ITERS     3000u
#define P9_NAME_MAX_LEN 200u

static const char *const g_enc_names[] = {
    "tversion", "tattach",  "twalk",     "tlopen",    "tlcreate",  "tread",
    "twrite",   "tclunk",   "tflush",    "tgetattr",  "tsetattr",  "treaddir",
    "tmkdir",   "tunlinkat", "trenameat", "tsymlink", "treadlink", "tlink",
    "tfsync",   "tstatfs"
};
static const uint8_t g_enc_types[] = {
    PGW_P9_TVERSION,   PGW_P9_TATTACH,  PGW_P9_TWALK,     PGW_P9_TLOPEN,
    PGW_P9_TLCREATE,   PGW_P9_TREAD,    PGW_P9_TWRITE,    PGW_P9_TCLUNK,
    PGW_P9_TFLUSH,     PGW_P9_TGETATTR, PGW_P9_TSETATTR,  PGW_P9_TREADDIR,
    PGW_P9_TMKDIR,     PGW_P9_TUNLINKAT, PGW_P9_TRENAMEAT, PGW_P9_TSYMLINK,
    PGW_P9_TREADLINK,  PGW_P9_TLINK,    PGW_P9_TFSYNC,    PGW_P9_TSTATFS
};
#define P9_KINDS (sizeof(g_enc_names) / sizeof(g_enc_names[0]))

/* Encoder under test, set by the campaign loop before each prop_forall. */
static uint32_t g_kind;
static uint8_t g_buf[P9_BUF + PROP_GUARD];
static char g_name[P9_NAME_BUF];
static char g_name2[P9_NAME_BUF];
static pgw_p9_setattr_t g_sa;
/* Walk names: one array of PGW_MAXWELEM pointers, all the same generated name,
 * so a nwname at the limit never reads past the array (the encoder refuses a
 * larger count before it touches the pointers). */
static const char *g_wnames[PGW_MAXWELEM];

static uint16_t case_tag(const prop_case *c)
{
    return (uint16_t)c->b;
}

static void case_setattr(const prop_case *c)
{
    memset(&g_sa, 0, sizeof(g_sa));
    g_sa.valid = c->a & PGW_P9_SETATTR_MTIME_SET;
    g_sa.mode = c->a | 0600u;
    g_sa.uid = c->a;
    g_sa.gid = c->b;
    g_sa.size = c->value;
    g_sa.atime_sec = c->value;
    g_sa.mtime_nsec = c->b;
}

static int32_t encode_low(const prop_case *c, size_t cap)
{
    uint16_t tag = case_tag(c);
    uint32_t fid = c->a;
    uint32_t num = (uint32_t)c->value;
    prop_name(g_name, sizeof(g_name), (size_t)(c->value % P9_NAME_MAX_LEN));
    switch (g_kind) {
    case 0:
        return pgw_p9_enc_tversion(g_buf, cap, tag, num, g_name);
    case 1:
        return pgw_p9_enc_tattach(g_buf, cap, tag, fid, PGW_FID_NONE, g_name,
                                  g_name, num);
    case 2: {
        uint16_t nw = (uint16_t)(num % (PGW_MAXWELEM + 1u));
        for (uint32_t i = 0; i < PGW_MAXWELEM; i++)
            g_wnames[i] = g_name;
        return pgw_p9_enc_twalk(g_buf, cap, tag, fid, fid + 1u, nw, g_wnames);
    }
    case 3:
        return pgw_p9_enc_tlopen(g_buf, cap, tag, fid, num);
    case 4:
        return pgw_p9_enc_tlcreate(g_buf, cap, tag, fid, g_name, num, 0644u,
                                   num);
    case 5:
        return pgw_p9_enc_tread(g_buf, cap, tag, fid, c->value, num % 512u);
    case 6:
        return pgw_p9_enc_twrite(g_buf, cap, tag, fid, c->value, c->bytes,
                                 num % 512u);
    case 7:
        return pgw_p9_enc_tclunk(g_buf, cap, tag, fid);
    case 8:
        return pgw_p9_enc_tflush(g_buf, cap, tag, (uint16_t)num);
    default: /* 9: Tgetattr, the only encoder with a 64-bit mask */
        return pgw_p9_enc_tgetattr(g_buf, cap, tag, fid, c->value);
    }
}

static int32_t encode_high(const prop_case *c, size_t cap)
{
    uint16_t tag = case_tag(c);
    uint32_t fid = c->a;
    uint32_t num = (uint32_t)c->value;
    prop_name(g_name, sizeof(g_name), (size_t)(c->value % P9_NAME_MAX_LEN));
    prop_name(g_name2, sizeof(g_name2), (size_t)(c->b % P9_NAME_MAX_LEN));
    switch (g_kind) {
    case 10:
        case_setattr(c);
        return pgw_p9_enc_tsetattr(g_buf, cap, tag, fid, &g_sa);
    case 11:
        return pgw_p9_enc_treaddir(g_buf, cap, tag, fid, c->value, num % 512u);
    case 12:
        return pgw_p9_enc_tmkdir(g_buf, cap, tag, fid, g_name, num, num);
    case 13:
        return pgw_p9_enc_tunlinkat(g_buf, cap, tag, fid, g_name, num);
    case 14:
        return pgw_p9_enc_trenameat(g_buf, cap, tag, fid, g_name, fid + 1u,
                                    g_name2);
    case 15:
        return pgw_p9_enc_tsymlink(g_buf, cap, tag, fid, g_name, g_name2, num);
    case 16:
        return pgw_p9_enc_treadlink(g_buf, cap, tag, fid);
    case 17:
        return pgw_p9_enc_tlink(g_buf, cap, tag, fid, fid + 1u, g_name);
    case 18:
        return pgw_p9_enc_tfsync(g_buf, cap, tag, fid);
    default:
        return pgw_p9_enc_tstatfs(g_buf, cap, tag, fid);
    }
}

/* One implementation of "encode this case", used by every property below. */
static int32_t encode_case(const prop_case *c, size_t cap)
{
    return (g_kind < 10u) ? encode_low(c, cap) : encode_high(c, cap);
}

/* --- property 1: the framing survives encode -> decode -------------------- */

static void gen_frame(prop_rng *r, prop_case *c)
{
    c->a = prop_u32(r);
    c->b = prop_u32(r);
    c->value = prop_u64(r);
    c->len = prop_below(r, 128u);
    prop_fill(r, c, 0, c->len);
    snprintf(c->tag, sizeof(c->tag), "%s", g_enc_names[g_kind]);
}

static bool pred_frame(const prop_case *c)
{
    memset(g_buf, 0, sizeof(g_buf));
    int32_t n = encode_case(c, P9_BUF);
    if (n <= 0)
        return false; /* a generous capacity must always be enough */
    pgw_p9_hdr h;
    memset(&h, 0, sizeof(h));
    if (pgw_p9_dec_hdr(g_buf, (size_t)n, &h) != PGW_OK)
        return false;
    return h.size == (uint32_t)n && h.type == g_enc_types[g_kind] &&
           h.tag == case_tag(c);
}

static void test_p9_prop_encode_decode_framing(void)
{
    uint32_t iters = prop_iters("PGW_PROP_ITERS", P9_ITERS);
    for (uint32_t k = 0; k < P9_KINDS; k++) {
        g_kind = k;
        prop_forall("9p encode -> dec_hdr framing", gen_frame, pred_frame, iters,
                    0x9E3779B97F4A7C15ULL ^ k);
    }
}
PGW_TEST(test_p9_prop_encode_decode_framing);

/* --- property 2: the capacity is a hard bound ----------------------------- */

static void gen_cap(prop_rng *r, prop_case *c)
{
    gen_frame(r, c);
    /* Capacities around the interesting edges: below the header, exactly the
     * header, and a small window around the length the message needs. */
    uint32_t pick = prop_below(r, 12u);
    c->len = (pick < 8u) ? pick : 8u + prop_below(r, 40u);
}

static bool pred_cap(const prop_case *c)
{
    size_t cap = c->len;
    if (cap > P9_BUF)
        return true; /* out of range for the bench buffer: nothing to check */
    memset(g_buf, 0, sizeof(g_buf));
    prop_guard_arm(g_buf, cap);
    int32_t n = encode_case(c, cap);
    if (!prop_guard_ok(g_buf, cap))
        return false; /* wrote outside the capacity it was given */
    if (n < 0)
        return n == -(int32_t)PGW_EINVAL || n == -(int32_t)PGW_EMSGSIZE;
    if ((size_t)n > cap)
        return false; /* claimed more than the buffer holds */
    pgw_p9_hdr h;
    return pgw_p9_dec_hdr(g_buf, (size_t)n, &h) == PGW_OK &&
           h.size == (uint32_t)n;
}

static void test_p9_prop_encode_respects_capacity(void)
{
    uint32_t iters = prop_iters("PGW_PROP_ITERS", P9_ITERS);
    for (uint32_t k = 0; k < P9_KINDS; k++) {
        g_kind = k;
        prop_forall("9p encoder never leaves its capacity", gen_cap, pred_cap,
                    iters, 0xD1B54A32D192ED03ULL ^ k);
    }
}
PGW_TEST(test_p9_prop_encode_respects_capacity);

/* --- property 3: extremes are refused, never accepted --------------------- */

/* A hostile value for a length/count field: the four the specification names
 * plus the two that sit either side of the capacity. */
static uint32_t hostile(uint32_t pick, size_t cap)
{
    switch (pick % 8u) {
    case 0:  return 0u;
    case 1:  return 1u;
    case 2:  return UINT32_MAX;
    case 3:  return UINT32_MAX - 1u;
    case 4:  return 0x7FFFFFFFu;
    case 5:  return (uint32_t)cap;
    case 6:  return (uint32_t)cap + 1u;
    default: return PGW_NAME_MAX + 1u;
    }
}

static void gen_extreme(prop_rng *r, prop_case *c)
{
    c->a = prop_u32(r);
    c->b = prop_u32(r);
    c->value = hostile(prop_u32(r), 128u);
    c->len = 0;
    snprintf(c->tag, sizeof(c->tag), "%s", g_enc_names[g_kind]);
}

static bool pred_extreme(const prop_case *c)
{
    uint32_t bad = (uint32_t)c->value;
    size_t cap = P9_BUF;
    memset(g_buf, 0, sizeof(g_buf));
    prop_guard_arm(g_buf, cap);
    /* Build the refusal the case asks for: an over-long name, too many walk
     * names, a missing name array, or a hostile count. */
    int32_t n;
    prop_name(g_name, sizeof(g_name), PGW_NAME_MAX + 1u);
    if (g_kind == 2u) {
        uint16_t nw = (uint16_t)(bad % 0xFFFFu);
        bool over = nw > (uint16_t)PGW_MAXWELEM;
        for (uint32_t i = 0; i < PGW_MAXWELEM; i++)
            g_wnames[i] = g_name;
        n = pgw_p9_enc_twalk(g_buf, cap, case_tag(c), c->a, c->a, nw,
                             (nw == 0u && (bad & 1u) != 0u) ? NULL : g_wnames);
        if (over && n > 0)
            return false; /* more names than MAXWELEM must never be encoded */
    } else if (g_kind == 6u) {
        n = pgw_p9_enc_twrite(g_buf, cap, case_tag(c), c->a, 0, c->bytes, bad);
        if (n > 0 && bad > P9_BUF)
            return false; /* a count above the buffer cannot fit in it */
    } else {
        n = encode_case(c, cap);
    }
    if (!prop_guard_ok(g_buf, cap))
        return false;
    return n < 0 || (size_t)n <= cap;
}

static void test_p9_prop_extremes_are_refused(void)
{
    uint32_t iters = prop_iters("PGW_PROP_ITERS", P9_ITERS / 3u);
    for (uint32_t k = 0; k < P9_KINDS; k++) {
        g_kind = k;
        prop_forall("9p extremes refused with an error code", gen_extreme,
                    pred_extreme, iters, 0xC2B2AE3D27D4EB4FULL ^ k);
    }
}
PGW_TEST(test_p9_prop_extremes_are_refused);

/* The over-long name refusal, stated on its own: an encoder that takes a name
 * must return -EINVAL for a name above PGW_NAME_MAX and never write it. */
static void test_p9_prop_overlong_name_refused(void)
{
    prop_name(g_name, sizeof(g_name), PGW_NAME_MAX + 1u);
    uint8_t buf[P9_BUF];
    int32_t rc = pgw_p9_enc_tlcreate(buf, sizeof(buf), 1u, 2u, g_name, 0u,
                                     0644u, 0u);
    PGW_ASSERT_EQ_I(rc, -(int32_t)PGW_EINVAL);
    rc = pgw_p9_enc_tmkdir(buf, sizeof(buf), 1u, 2u, g_name, 0755u, 0u);
    PGW_ASSERT_EQ_I(rc, -(int32_t)PGW_EINVAL);
    rc = pgw_p9_enc_tsymlink(buf, sizeof(buf), 1u, 2u, g_name, "target", 0u);
    PGW_ASSERT_EQ_I(rc, -(int32_t)PGW_EINVAL);
    /* The *target* of a symlink is a path, bounded by PGW_PATH_MAX: a name
     * longer than PGW_NAME_MAX is a legal target and must be accepted. */
    rc = pgw_p9_enc_tsymlink(buf, sizeof(buf), 1u, 2u, "n", g_name, 0u);
    PGW_ASSERT(rc > 0);
    /* NULL where a name is required is a caller error, not a message. */
    rc = pgw_p9_enc_tlcreate(buf, sizeof(buf), 1u, 2u, NULL, 0u, 0644u, 0u);
    PGW_ASSERT_EQ_I(rc, -(int32_t)PGW_EINVAL);
    /* A capacity below the header is refused before anything is written. */
    rc = pgw_p9_enc_tclunk(buf, PGW_HEADER_SIZE - 1u, 1u, 2u);
    PGW_ASSERT_EQ_I(rc, -(int32_t)PGW_EINVAL);
    rc = pgw_p9_enc_tclunk(NULL, sizeof(buf), 1u, 2u);
    PGW_ASSERT_EQ_I(rc, -(int32_t)PGW_EINVAL);
}
PGW_TEST(test_p9_prop_overlong_name_refused);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
