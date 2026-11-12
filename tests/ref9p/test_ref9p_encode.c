/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/test_ref9p_encode.c - our encoders, read at the offsets of the
 * normative 9P2000.L message table.
 *
 * The decoder half is tested in test_ref9p_fields.c. This file checks the
 * opposite direction: what pgw_p9_enc_* writes must match the documented field
 * order byte for byte. The independent oracle (ref9p_o_*) reads each field at
 * the offset the normative table gives, so a divergence between our encoders
 * and the described layout fails a case here.
 *
 * Covers P9-01, P9-02, P9-03 and the "no self-reference" half of P9-10.
 */
#define _POSIX_C_SOURCE 200809L

#include "ref9p_fields.h"
#include "ref9p_truth.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <string.h>

static void test_encode_version_and_walk(void)
{
    int32_t n = pgw_p9_enc_tversion(ref9p_tx, sizeof ref9p_tx, REF9P_NOTAG, 8192u,
                                    "9P2000.L");
    PGW_ASSERT(n > 0);
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 2u + 8u);
    ref9p_expect_header(ref9p_tx, (size_t)n, PGW_P9_TVERSION, REF9P_NOTAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 7), 8192u);   /* msize[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_tx, 11), 8u);     /* version length */
    PGW_ASSERT_MEM_EQ(ref9p_tx + 13, "9P2000.L", 8);

    const char *names[] = { "sub", "nested.txt" };
    n = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 3u, 4u, 2u,
                         names);
    PGW_ASSERT(n > 0);
    PGW_ASSERT_EQ_U((size_t)n, 34u);
    ref9p_expect_header(ref9p_tx, (size_t)n, PGW_P9_TWALK, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 7), 3u);      /* fid[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 11), 4u);     /* newfid[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_tx, 15), 2u);     /* nwname[2] */
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_tx, 17), 3u);     /* wname[0] length */
    PGW_ASSERT_MEM_EQ(ref9p_tx + 19, "sub", 3);
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_tx, 22), 10u);    /* wname[1] length */
    PGW_ASSERT_MEM_EQ(ref9p_tx + 24, "nested.txt", 10);

    /* Tattach has fid[4] afid[4] uname[s] aname[s] n_uname[4]. */
    n = pgw_p9_enc_tattach(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 9u,
                           PGW_FID_NONE, "u", "", 1000u);
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 4u + 3u + 2u + 4u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 7), 9u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 11), PGW_FID_NONE);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 20), 1000u);
}
PGW_TEST(test_encode_version_and_walk);

static void test_encode_io_and_metadata(void)
{
    int32_t n = pgw_p9_enc_tread(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 5u,
                                 0x1122334455667788ULL, 4096u);
    PGW_ASSERT_EQ_U((size_t)n, 23u); /* fid[4] offset[8] count[4] */
    ref9p_expect_header(ref9p_tx, (size_t)n, PGW_P9_TREAD, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 7), 5u);
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_tx, 11), 0x1122334455667788ULL);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 19), 4096u);

    static const uint8_t payload[6] = { 1, 2, 3, 4, 5, 6 };
    n = pgw_p9_enc_twrite(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 5u, 7u,
                          payload, 6u);
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 8u + 4u + 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 19), 6u); /* count[4] */
    PGW_ASSERT_MEM_EQ(ref9p_tx + 23, payload, 6);

    pgw_p9_setattr_t sa;
    memset(&sa, 0, sizeof sa);
    sa.valid = PGW_P9_SETATTR_MODE | PGW_P9_SETATTR_SIZE;
    sa.mode = 0600;
    sa.size = 42;
    n = pgw_p9_enc_tsetattr(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 5u, &sa);
    PGW_ASSERT_EQ_U((size_t)n, 67u);
    ref9p_expect_header(ref9p_tx, (size_t)n, PGW_P9_TSETATTR, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 7), 5u);    /* fid[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 11), sa.valid);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 15), 0600u);/* mode[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_tx, 27), 42u);  /* size[8] */

    n = pgw_p9_enc_tfsync(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 5u);
    PGW_ASSERT_EQ_U((size_t)n, 15u); /* fid[4] datasync[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 11), 0u);

    n = pgw_p9_enc_tclunk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 5u);
    PGW_ASSERT_EQ_U((size_t)n, 11u);
    n = pgw_p9_enc_tstatfs(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 5u);
    PGW_ASSERT_EQ_U((size_t)n, 11u);
    n = pgw_p9_enc_tlopen(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 5u, 0x42u);
    PGW_ASSERT_EQ_U((size_t)n, 15u); /* fid[4] flags[4] */
}
PGW_TEST(test_encode_io_and_metadata);

static void test_encode_namespace(void)
{
    int32_t n = pgw_p9_enc_tmkdir(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG,
                                  2u, "d", 0755u, 0u);
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 3u + 4u + 4u);
    ref9p_expect_header(ref9p_tx, (size_t)n, PGW_P9_TMKDIR, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 7), 2u);    /* dfid[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_tx, 11), 1u);   /* name[s] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 14), 0755u);/* mode[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 18), 0u);   /* gid[4] */

    n = pgw_p9_enc_tunlinkat(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 2u, "d",
                             PGW_P9_AT_REMOVEDIR);
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 3u + 4u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 14), PGW_P9_AT_REMOVEDIR);

    n = pgw_p9_enc_trenameat(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 2u,
                             "a", 3u, "b");
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 3u + 4u + 3u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 14), 3u);   /* newdirfid[4] */
    PGW_ASSERT_MEM_EQ(ref9p_tx + 20, "b", 1);

    n = pgw_p9_enc_tsymlink(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 2u, "l",
                            "t", 0u);
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 3u + 3u + 4u);
    ref9p_expect_header(ref9p_tx, (size_t)n, PGW_P9_TSYMLINK, REF9P_FIELD_TAG);

    n = pgw_p9_enc_tlink(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 2u, 6u, "h");
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 4u + 3u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 7), 2u);    /* dfid[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 11), 6u);   /* fid[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_tx, 15), 1u);   /* name[s] */

    n = pgw_p9_enc_tlcreate(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 2u, "f",
                            0x42u, 0644u, 0u);
    PGW_ASSERT_EQ_U((size_t)n, 7u + 4u + 3u + 4u + 4u + 4u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 14), 0x42u);/* flags[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_tx, 18), 0644u);/* mode[4] */
}
PGW_TEST(test_encode_namespace);