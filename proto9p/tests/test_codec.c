/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_codec.c - field-by-field checks of every T-message.
 *
 * There is no T-message decoder in the contract (the client only writes them),
 * so "round trip" here means: encode into a buffer, then walk the bytes with
 * the same pgw_buf cursor a server would use and check that each field is at
 * the offset and with the encoding 9P2000.L prescribes. The expected lengths
 * are written out by hand from the protocol description, which is what makes
 * this an independent check of the encoder rather than a tautology.
 */
#include "pgw/buf.h"
#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <string.h>

static uint8_t g_buf[1024];

/* Validate the envelope and return a reader positioned on the body. */
static pgw_buf body_of(const uint8_t *b, size_t len, uint8_t type, uint16_t tag)
{
    pgw_p9_hdr h;
    PGW_ASSERT_EQ_I(pgw_p9_dec_hdr(b, len, &h), PGW_OK);
    PGW_ASSERT_EQ_I(h.type, type);
    PGW_ASSERT_EQ_U(h.tag, tag);
    PGW_ASSERT_EQ_U(h.size, len);
    pgw_buf r;
    pgw_buf_body(&r, b, h.size, PGW_HEADER_SIZE);
    PGW_ASSERT(pgw_buf_ok(&r));
    return r;
}

static void expect_str(pgw_buf *r, const char *want)
{
    char tmp[128];
    size_t len = 0;
    PGW_ASSERT_EQ_I(pgw_get_str(r, tmp, sizeof tmp, &len), PGW_STR_OK);
    PGW_ASSERT_EQ_U(len, strlen(want));
    PGW_ASSERT_STR_EQ(tmp, want);
}

static void test_enc_tversion(void)
{
    int32_t n = pgw_p9_enc_tversion(g_buf, sizeof g_buf, 7, 8192, "9P2000.L");
    PGW_ASSERT_EQ_I(n, 7 + 4 + 2 + 8);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TVERSION, 7);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 8192);
    expect_str(&r, "9P2000.L");
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_tversion);

static void test_enc_tattach(void)
{
    int32_t n = pgw_p9_enc_tattach(g_buf, sizeof g_buf, 1, 10, PGW_FID_NONE,
                                   "nobody", "/share", 1000);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4 + 2 + 6 + 2 + 6 + 4);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TATTACH, 1);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 10);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), PGW_FID_NONE);
    expect_str(&r, "nobody");
    expect_str(&r, "/share");
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 1000);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_tattach);

static void test_enc_twalk(void)
{
    const char *names[3] = { "sub", "deeper", "leaf.txt" };
    int32_t n = pgw_p9_enc_twalk(g_buf, sizeof g_buf, 2, 3, 4, 3, names);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4 + 2 + (2 + 3) + (2 + 6) + (2 + 8));
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TWALK, 2);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 3);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 4);
    PGW_ASSERT_EQ_U(pgw_get_u16(&r), 3);
    expect_str(&r, "sub");
    expect_str(&r, "deeper");
    expect_str(&r, "leaf.txt");
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    /* A clone (nwname 0) is legal and carries no name at all. */
    n = pgw_p9_enc_twalk(g_buf, sizeof g_buf, 3, 5, 6, 0, NULL);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4 + 2);
    r = body_of(g_buf, (size_t)n, PGW_P9_TWALK, 3);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 5);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 6);
    PGW_ASSERT_EQ_U(pgw_get_u16(&r), 0);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_twalk);

static void test_enc_open_and_create(void)
{
    int32_t n = pgw_p9_enc_tlopen(g_buf, sizeof g_buf, 4, 8, 2u);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TLOPEN, 4);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 8);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 2);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    n = pgw_p9_enc_tlcreate(g_buf, sizeof g_buf, 5, 9, "new.txt", 0x8241u,
                            0100644u, 1000);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 2 + 7 + 4 + 4 + 4);
    r = body_of(g_buf, (size_t)n, PGW_P9_TLCREATE, 5);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 9);
    expect_str(&r, "new.txt");
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 0x8241u);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 0100644u);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 1000);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_open_and_create);

static void test_enc_read_write(void)
{
    int32_t n = pgw_p9_enc_tread(g_buf, sizeof g_buf, 6, 11, 0x1122334455667788ULL,
                                 4096);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 8 + 4);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TREAD, 6);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 11);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 0x1122334455667788ULL);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 4096);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    n = pgw_p9_enc_twrite(g_buf, sizeof g_buf, 7, 12, 16, "abcdef", 6);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 8 + 4 + 6);
    r = body_of(g_buf, (size_t)n, PGW_P9_TWRITE, 7);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 12);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 16);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 6);
    PGW_ASSERT_MEM_EQ(pgw_take(&r, 6), "abcdef", 6);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_read_write);

static void test_enc_simple_ones(void)
{
    int32_t n = pgw_p9_enc_tclunk(g_buf, sizeof g_buf, 8, 13);
    PGW_ASSERT_EQ_I(n, 7 + 4);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TCLUNK, 8);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 13);

    n = pgw_p9_enc_tflush(g_buf, sizeof g_buf, 9, 13);
    PGW_ASSERT_EQ_I(n, 7 + 2);
    r = body_of(g_buf, (size_t)n, PGW_P9_TFLUSH, 9);
    PGW_ASSERT_EQ_U(pgw_get_u16(&r), 13);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    n = pgw_p9_enc_treadlink(g_buf, sizeof g_buf, 10, 14);
    PGW_ASSERT_EQ_I(n, 7 + 4);
    r = body_of(g_buf, (size_t)n, PGW_P9_TREADLINK, 10);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 14);

    n = pgw_p9_enc_tstatfs(g_buf, sizeof g_buf, 11, 15);
    PGW_ASSERT_EQ_I(n, 7 + 4);
    r = body_of(g_buf, (size_t)n, PGW_P9_TSTATFS, 11);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 15);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    n = pgw_p9_enc_tfsync(g_buf, sizeof g_buf, 12, 16);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4);
    r = body_of(g_buf, (size_t)n, PGW_P9_TFSYNC, 12);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 16);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 0);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_simple_ones);

static void test_enc_attr_messages(void)
{
    int32_t n = pgw_p9_enc_tgetattr(g_buf, sizeof g_buf, 13, 17,
                                    PGW_P9_GETATTR_BASIC);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 8);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TGETATTR, 13);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 17);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), PGW_P9_GETATTR_BASIC);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    pgw_p9_setattr_t sa;
    memset(&sa, 0, sizeof sa);
    sa.valid = PGW_P9_SETATTR_MODE | PGW_P9_SETATTR_SIZE;
    sa.mode = 0100640u;
    sa.size = 4096;
    sa.atime_sec = 11;
    sa.atime_nsec = 12;
    sa.mtime_sec = 13;
    sa.mtime_nsec = 14;
    n = pgw_p9_enc_tsetattr(g_buf, sizeof g_buf, 14, 18, &sa);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4 + 4 + 4 + 4 + 8 * 5);
    r = body_of(g_buf, (size_t)n, PGW_P9_TSETATTR, 14);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 18);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), sa.valid);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), sa.mode);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 0);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 0);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 4096);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 11);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 12);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 13);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 14);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_attr_messages);

static void test_enc_namespace_messages(void)
{
    int32_t n = pgw_p9_enc_treaddir(g_buf, sizeof g_buf, 15, 19, 5, 8192);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 8 + 4);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TREADDIR, 15);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 19);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 5);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 8192);

    n = pgw_p9_enc_tmkdir(g_buf, sizeof g_buf, 16, 20, "d", 040755u, 7);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 3 + 4 + 4);
    r = body_of(g_buf, (size_t)n, PGW_P9_TMKDIR, 16);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 20);
    expect_str(&r, "d");
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 040755u);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 7);

    n = pgw_p9_enc_tunlinkat(g_buf, sizeof g_buf, 17, 21, "f", PGW_P9_AT_REMOVEDIR);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 3 + 4);
    r = body_of(g_buf, (size_t)n, PGW_P9_TUNLINKAT, 17);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 21);
    expect_str(&r, "f");
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), PGW_P9_AT_REMOVEDIR);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    n = pgw_p9_enc_trenameat(g_buf, sizeof g_buf, 18, 22, "a", 23, "b");
    PGW_ASSERT_EQ_I(n, 7 + 4 + 3 + 4 + 3);
    r = body_of(g_buf, (size_t)n, PGW_P9_TRENAMEAT, 18);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 22);
    expect_str(&r, "a");
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 23);
    expect_str(&r, "b");
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_namespace_messages);

static void test_enc_links(void)
{
    int32_t n = pgw_p9_enc_tsymlink(g_buf, sizeof g_buf, 19, 24, "ln", "tgt", 5);
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4 + 5 + 4);
    pgw_buf r = body_of(g_buf, (size_t)n, PGW_P9_TSYMLINK, 19);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 24);
    expect_str(&r, "ln");
    expect_str(&r, "tgt");
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 5);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);

    n = pgw_p9_enc_tlink(g_buf, sizeof g_buf, 20, 25, 26, "hard");
    PGW_ASSERT_EQ_I(n, 7 + 4 + 4 + 6);
    r = body_of(g_buf, (size_t)n, PGW_P9_TLINK, 20);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 25);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 26);
    expect_str(&r, "hard");
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}
PGW_TEST(test_enc_links);

static void test_enc_rejects_bad_input(void)
{
    const char *too_many[17];
    for (size_t i = 0; i < 17; i++)
        too_many[i] = "x";
    /* 17 names is more than PGW_MAXWELEM (16). */
    PGW_ASSERT(pgw_p9_enc_twalk(g_buf, sizeof g_buf, 1, 2, 3, 17, too_many) < 0);
    /* NULL where a string is required. */
    PGW_ASSERT(pgw_p9_enc_tversion(g_buf, sizeof g_buf, 1, 4096, NULL) < 0);
    PGW_ASSERT(pgw_p9_enc_tattach(g_buf, sizeof g_buf, 1, 2, 3, NULL, "", 0) < 0);
    PGW_ASSERT(pgw_p9_enc_tmkdir(g_buf, sizeof g_buf, 1, 2, NULL, 0755, 0) < 0);
    PGW_ASSERT(pgw_p9_enc_twrite(g_buf, sizeof g_buf, 1, 2, 0, NULL, 4) < 0);
    pgw_p9_setattr_t sa;
    memset(&sa, 0, sizeof sa);
    PGW_ASSERT(pgw_p9_enc_tsetattr(g_buf, sizeof g_buf, 1, 2, NULL) < 0);
    /* A buffer that is too small must fail, never truncate. */
    PGW_ASSERT(pgw_p9_enc_tversion(g_buf, 5, 1, 4096, "9P2000.L") < 0);
    PGW_ASSERT(pgw_p9_enc_twrite(g_buf, 20, 1, 2, 0, "abcdefgh", 8) < 0);
    /* A name longer than PGW_NAME_MAX is refused, not cut. */
    char longname[PGW_NAME_MAX + 2];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    PGW_ASSERT(pgw_p9_enc_tmkdir(g_buf, sizeof g_buf, 1, 2, longname, 0755, 0) < 0);
}
PGW_TEST(test_enc_rejects_bad_input);
