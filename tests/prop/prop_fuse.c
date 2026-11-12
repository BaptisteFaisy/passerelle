/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/prop/prop_fuse.c - encode-side properties of the virtiofs codecs.
 *
 * Gap 61 of docs/gap-analysis.md. Requirements VFS-03 (the FUSE 7.x wire
 * layout, one request-header writer used by every encoder) and ROB-06 (nothing
 * is written outside the buffer the caller handed over).
 *
 * Three properties, each run over *every* encoder of pgw/fusep.h:
 *
 *   framing   encode, then read the 40-byte input header back with the same
 *             cursor the decoders use: len[4] equals the returned length, and
 *             opcode, unique and nodeid are the ones that were passed in;
 *   bounds    for every capacity - 0, 16 (the output header size), 40 (the
 *             input header alone) and one byte short - the encoder either
 *             returns a length that fits or -1, and the guard bytes past the
 *             capacity are never touched;
 *   extremes  a name above PGW_FUSE_NAME_WIRE_MAX, a NULL name, a NULL
 *             setattr and a capacity below the input header are refused with
 *             -1, never accepted.
 */
#include "prop.h"

#include "fusep_abi.h"

#include "pgw/buf.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

#define FU_BUF      2048u
#define FU_NAME_BUF 600u
#define FU_ITERS    3000u
#define FU_NAME_LEN 200u

/* The encoders under test. `is_dir` and `flags` are held at their plain value
 * so that each kind has exactly one expected opcode. */
enum {
    F_INIT = 0,
    F_LOOKUP,
    F_FORGET,
    F_GETATTR,
    F_SETATTR,
    F_OPEN,
    F_CREATE,
    F_RELEASE,
    F_READ,
    F_WRITE,
    F_MKDIR,
    F_UNLINK,
    F_RENAME,
    F_SYMLINK,
    F_LINK,
    F_READLINK,
    F_FSYNC,
    F_STATFS,
    F_INTERRUPT,
    F_KINDS
};

static const uint32_t g_op[F_KINDS] = {
    PGW_FUSE_INIT,     PGW_FUSE_LOOKUP,   PGW_FUSE_FORGET,  PGW_FUSE_GETATTR,
    PGW_FUSE_SETATTR,  PGW_FUSE_OPEN,     PGW_FUSE_CREATE,  PGW_FUSE_RELEASE,
    PGW_FUSE_READ,     PGW_FUSE_WRITE,    PGW_FUSE_MKDIR,   PGW_FUSE_UNLINK,
    PGW_FUSE_RENAME,   PGW_FUSE_SYMLINK,  PGW_FUSE_LINK,    PGW_FUSE_READLINK,
    PGW_FUSE_FSYNC,    PGW_FUSE_STATFS,   PGW_FUSE_INTERRUPT
};

static const char *const g_op_name[F_KINDS] = {
    "init",   "lookup", "forget",   "getattr", "setattr", "open",    "create",
    "release", "read",  "write",    "mkdir",   "unlink",  "rename",  "symlink",
    "link",   "readlink", "fsync",  "statfs",  "interrupt"
};

static uint32_t g_kind;
static uint8_t g_buf[FU_BUF + PROP_GUARD];
static char g_name[FU_NAME_BUF];
static char g_name2[FU_NAME_BUF];
static pgw_fuse_setattr_in g_sa;

static uint64_t case_unique(const prop_case *c)
{
    return c->value;
}

static void case_setattr(const prop_case *c)
{
    memset(&g_sa, 0, sizeof(g_sa));
    g_sa.valid = c->a & PGW_FUSE_FATTR_CTIME;
    g_sa.mode = c->a | 0600u;
    g_sa.uid = c->a;
    g_sa.gid = c->b;
    g_sa.size = c->value;
    g_sa.atime = c->value;
    g_sa.mtime_nsec = c->b;
}

static int32_t encode_low(const prop_case *c, size_t cap)
{
    uint64_t u = case_unique(c);
    uint64_t node = c->a;
    uint32_t num = c->b;
    prop_name(g_name, sizeof(g_name), (size_t)(c->value % FU_NAME_LEN));
    switch (g_kind) {
    case F_INIT:
        return pgw_fuse_enc_init(g_buf, cap, u, num, c->value);
    case F_LOOKUP:
        return pgw_fuse_enc_lookup(g_buf, cap, u, node, g_name);
    case F_FORGET:
        return pgw_fuse_enc_forget(g_buf, cap, u, node, c->value);
    case F_GETATTR:
        return pgw_fuse_enc_getattr(g_buf, cap, u, node, c->value,
                                    (c->b & 1u) != 0u);
    case F_SETATTR:
        case_setattr(c);
        return pgw_fuse_enc_setattr(g_buf, cap, u, node, &g_sa);
    case F_OPEN:
        return pgw_fuse_enc_open(g_buf, cap, u, node, num, false);
    case F_CREATE:
        return pgw_fuse_enc_create(g_buf, cap, u, node, g_name, num, 0644u,
                                   022u);
    case F_RELEASE:
        return pgw_fuse_enc_release(g_buf, cap, u, node, c->value, false);
    default:
        return pgw_fuse_enc_read(g_buf, cap, u, node, c->value, c->value >> 1,
                                 num % 512u, false);
    }
}

static int32_t encode_high(const prop_case *c, size_t cap)
{
    uint64_t u = case_unique(c);
    uint64_t node = c->a;
    uint32_t num = c->b;
    prop_name(g_name, sizeof(g_name), (size_t)(c->value % FU_NAME_LEN));
    prop_name(g_name2, sizeof(g_name2), (size_t)(c->b % FU_NAME_LEN));
    switch (g_kind) {
    case F_WRITE:
        return pgw_fuse_enc_write(g_buf, cap, u, node, c->value, c->value >> 1,
                                  c->bytes, num % 512u);
    case F_MKDIR:
        return pgw_fuse_enc_mkdir(g_buf, cap, u, node, g_name, 0755u, 022u);
    case F_UNLINK:
        return pgw_fuse_enc_name_op(g_buf, cap, PGW_FUSE_UNLINK, u, node,
                                    g_name);
    case F_RENAME:
        return pgw_fuse_enc_rename(g_buf, cap, u, node, g_name, node + 1u,
                                   g_name2, 0u);
    case F_SYMLINK:
        return pgw_fuse_enc_symlink(g_buf, cap, u, node, g_name, g_name2);
    case F_LINK:
        return pgw_fuse_enc_link(g_buf, cap, u, node, node + 1u, g_name);
    case F_READLINK:
        return pgw_fuse_enc_readlink(g_buf, cap, u, node);
    case F_FSYNC:
        return pgw_fuse_enc_fsync(g_buf, cap, u, node, c->value, false, false);
    case F_STATFS:
        return pgw_fuse_enc_statfs(g_buf, cap, u, node);
    default:
        return pgw_fuse_enc_interrupt(g_buf, cap, u, c->value);
    }
}

static int32_t encode_case(const prop_case *c, size_t cap)
{
    return (g_kind < F_WRITE) ? encode_low(c, cap) : encode_high(c, cap);
}

/* The nodeid the header must carry: INIT and INTERRUPT address no node. */
static uint64_t expected_nodeid(const prop_case *c)
{
    return (g_kind == F_INIT || g_kind == F_INTERRUPT) ? 0u : c->a;
}

/* --- property 1: the input header survives encode -> read back ------------ */

static void gen_frame(prop_rng *r, prop_case *c)
{
    c->a = prop_u32(r);
    c->b = prop_u32(r);
    c->value = prop_u64(r);
    c->len = prop_below(r, 512u);
    prop_fill(r, c, 0, c->len);
    snprintf(c->tag, sizeof(c->tag), "%s", g_op_name[g_kind]);
}

static bool pred_frame(const prop_case *c)
{
    memset(g_buf, 0, sizeof(g_buf));
    int32_t n = encode_case(c, FU_BUF);
    if (n < (int32_t)PGW_FUSE_IN_HDR_SZ)
        return false; /* a generous capacity must always be enough */
    pgw_buf r;
    pgw_buf_init(&r, g_buf, (size_t)n);
    uint32_t len = pgw_get_u32(&r);
    uint32_t op = pgw_get_u32(&r);
    uint64_t unique = pgw_get_u64(&r);
    uint64_t nodeid = pgw_get_u64(&r);
    return pgw_buf_ok(&r) && len == (uint32_t)n && op == g_op[g_kind] &&
           unique == case_unique(c) && nodeid == expected_nodeid(c);
}

static void test_fuse_prop_encode_header_round_trip(void)
{
    uint32_t iters = prop_iters("PGW_PROP_ITERS", FU_ITERS);
    for (uint32_t k = 0; k < F_KINDS; k++) {
        g_kind = k;
        prop_forall("fuse encode -> input header round trip", gen_frame,
                    pred_frame, iters, 0x9E3779B912345678ULL ^ k);
    }
}
PGW_TEST(test_fuse_prop_encode_header_round_trip);

/* --- property 2: the capacity is a hard bound ----------------------------- */

static void gen_cap(prop_rng *r, prop_case *c)
{
    gen_frame(r, c);
    uint32_t pick = prop_below(r, 12u);
    c->len = (pick < 8u) ? pick : 8u + prop_below(r, 64u);
}

static bool pred_cap(const prop_case *c)
{
    size_t cap = c->len;
    if (cap > FU_BUF)
        return true;
    memset(g_buf, 0, sizeof(g_buf));
    prop_guard_arm(g_buf, cap);
    int32_t n = encode_case(c, cap);
    if (!prop_guard_ok(g_buf, cap))
        return false; /* wrote outside the capacity it was given */
    if (n < 0)
        return n == -1;
    if ((size_t)n > cap || (size_t)n < PGW_FUSE_IN_HDR_SZ)
        return false;
    pgw_buf r;
    pgw_buf_init(&r, g_buf, (size_t)n);
    return pgw_get_u32(&r) == (uint32_t)n && pgw_buf_ok(&r);
}

static void test_fuse_prop_encode_respects_capacity(void)
{
    uint32_t iters = prop_iters("PGW_PROP_ITERS", FU_ITERS);
    for (uint32_t k = 0; k < F_KINDS; k++) {
        g_kind = k;
        prop_forall("fuse encoder never leaves its capacity", gen_cap, pred_cap,
                    iters, 0xD1B54A32D192ED03ULL ^ k);
    }
}
PGW_TEST(test_fuse_prop_encode_respects_capacity);

/* --- property 3: extremes are refused, never accepted --------------------- */

static void test_fuse_prop_extremes_are_refused(void)
{
    uint8_t buf[FU_BUF];
    char name[FU_NAME_BUF];
    pgw_fuse_setattr_in sa;
    memset(&sa, 0, sizeof(sa));

    /* A name above the protocol limit is refused, and so is a missing one. */
    prop_name(name, sizeof(name), PGW_FUSE_NAME_WIRE_MAX + 1u);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_lookup(buf, sizeof(buf), 1u, 2u, name), -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_create(buf, sizeof(buf), 1u, 2u, name, 0u,
                                        0644u, 0u),
                    -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_mkdir(buf, sizeof(buf), 1u, 2u, name, 0755u,
                                       0u),
                    -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_symlink(buf, sizeof(buf), 1u, 2u, name, "t"),
                    -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_lookup(buf, sizeof(buf), 1u, 2u, NULL), -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_setattr(buf, sizeof(buf), 1u, 2u, NULL), -1);

    /* A capacity below the input header cannot hold any request. */
    for (size_t cap = 0; cap < PGW_FUSE_IN_HDR_SZ; cap++) {
        PGW_ASSERT_EQ_I(pgw_fuse_enc_statfs(buf, cap, 1u, 2u), -1);
        PGW_ASSERT_EQ_I(pgw_fuse_enc_readlink(buf, cap, 1u, 2u), -1);
    }
    /* The exact header size is enough for a request with no body. */
    PGW_ASSERT_EQ_I(pgw_fuse_enc_statfs(buf, PGW_FUSE_IN_HDR_SZ, 1u, 2u),
                    (int32_t)PGW_FUSE_IN_HDR_SZ);
    /* A NULL buffer is a caller error, not a message. */
    PGW_ASSERT_EQ_I(pgw_fuse_enc_statfs(NULL, sizeof(buf), 1u, 2u), -1);
}
PGW_TEST(test_fuse_prop_extremes_are_refused);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
