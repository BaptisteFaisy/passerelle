/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/test_conformance_wire.c - the bytes of diod's rules.
 *
 * Every case here checks a rule of diod's protocol description that is a
 * statement about bytes. The assertions read the message at the offsets the
 * description gives, with the independent accessors of tests/ref9p/
 * (ref9p_o_u16, ref9p_o_u32, ...), never with a helper of the code under
 * test: a rule that is checked with the same function it is about proves
 * nothing.
 *
 * The rules and their sources are in conformance_table.c; the excerpt of the
 * description they come from is diod-protocol-excerpt.md.
 *
 * Covers P9-02 (the message layouts), P9-09 (the strict decoding) and gap 60.
 */
#define _POSIX_C_SOURCE 200809L

#include "conformance_table.h"

#include "ref9p_fields.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <string.h>

/* The bytes a message of the description starts with: size[4] type[1] tag[2]. */
#define CONF_TAG 0x1234u

static uint8_t g_buf[512];

/* The rule: the integers of the description travel little-endian. */
static void conf_wire_integers_are_little_endian(void)
{
    int32_t n =
            pgw_p9_enc_tversion(g_buf, sizeof g_buf, CONF_TAG, 0x01020304u, "9P2000.L");
    PGW_ASSERT(n > 0);
    /* msize[4] sits at offset 7: the least significant byte comes first. */
    PGW_ASSERT_EQ_U(g_buf[7], 0x04u);
    PGW_ASSERT_EQ_U(g_buf[8], 0x03u);
    PGW_ASSERT_EQ_U(g_buf[9], 0x02u);
    PGW_ASSERT_EQ_U(g_buf[10], 0x01u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 0x01020304u);
    /* tag[2] is little-endian too. */
    PGW_ASSERT_EQ_U(g_buf[5], 0x34u);
    PGW_ASSERT_EQ_U(g_buf[6], 0x12u);
}
PGW_TEST(conf_wire_integers_are_little_endian);
PGW_CONF_CASE(conf_wire_integers_are_little_endian);

/* The rule: a string is length[2] and its bytes, with no terminating NULL. */
static void conf_wire_string_has_no_terminating_nul(void)
{
    int32_t n = pgw_p9_enc_tversion(g_buf, sizeof g_buf, CONF_TAG, 8192u, "9P2000.L");
    PGW_ASSERT(n > 0);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 11), 8u);
    PGW_ASSERT_MEM_EQ(g_buf + 13, "9P2000.L", 8);
    /* The message stops right after the last byte of the string: no NUL. */
    PGW_ASSERT_EQ_U((size_t)n, 21u);
    PGW_ASSERT_EQ_U(g_buf[20], (uint8_t)'L');
}
PGW_TEST(conf_wire_string_has_no_terminating_nul);
PGW_CONF_CASE(conf_wire_string_has_no_terminating_nul);

/* The rule: a qid is 13 bytes, type[1] version[4] path[8]. */
static void conf_wire_qid_is_thirteen_bytes(void)
{
    /* An Rattach built by hand, at the offsets of the description. */
    size_t n = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RATTACH);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u8(g_buf, n, PGW_P9_QTDIR);
    ref9p_o_put_u32(g_buf, n + 1, 0x11223344u);
    ref9p_o_put_u64(g_buf, n + 5, 0x0102030405060708ull);
    n += 13;
    ref9p_o_seal(g_buf, n);
    PGW_ASSERT_EQ_U(n, 20u);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rattach(g_buf, n, CONF_TAG, &qid), PGW_OK);
    PGW_ASSERT_EQ_U(qid.type, PGW_P9_QTDIR);
    PGW_ASSERT_EQ_U(qid.version, 0x11223344u);
    PGW_ASSERT_EQ_U(qid.path, 0x0102030405060708ull);
    /* The qid of the description is 13 bytes: 7 + 13. */
    PGW_ASSERT_EQ_U(n, (size_t)PGW_HEADER_SIZE + 13u);
}
PGW_TEST(conf_wire_qid_is_thirteen_bytes);
PGW_CONF_CASE(conf_wire_qid_is_thirteen_bytes);

/* The rule: Tversion is size[4] Tversion tag[2] msize[4] version[s], and the
 * version string of a 9P2000.L client is exactly "9P2000.L". */
static void conf_wire_tversion_layout(void)
{
    int32_t n =
            pgw_p9_enc_tversion(g_buf, sizeof g_buf, CONF_TAG, 0x0000BEEFu, "9P2000.L");
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TVERSION, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 0x0000BEEFu);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 11), 8u);
    PGW_ASSERT_MEM_EQ(g_buf + 13, "9P2000.L", 8);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 2u + 8u);
}
PGW_TEST(conf_wire_tversion_layout);
PGW_CONF_CASE(conf_wire_tversion_layout);

/* The rule: size[4] counts the whole message, the size value included. */
static void conf_wire_size_field_is_the_message_length(void)
{
    int32_t n = pgw_p9_enc_tclunk(g_buf, sizeof g_buf, CONF_TAG, 7u);
    PGW_ASSERT(n > 0);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 0), (uint32_t)n);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u);
    /* The same rule on the other side: a decoder rejects a size that does
     * not describe the bytes it received. */
    uint8_t bad[16];
    memcpy(bad, g_buf, (size_t)n);
    ref9p_o_put_u32(bad, 0, (uint32_t)n + 4u);
    pgw_p9_hdr hdr;
    PGW_ASSERT_EQ_I(pgw_p9_dec_hdr(bad, (size_t)n, &hdr), PGW_EMSGSIZE);
}
PGW_TEST(conf_wire_size_field_is_the_message_length);
PGW_CONF_CASE(conf_wire_size_field_is_the_message_length);

/* The rule: Tflush is size[4] Tflush tag[2] oldtag[2]. */
static void conf_wire_tflush_layout(void)
{
    int32_t n = pgw_p9_enc_tflush(g_buf, sizeof g_buf, CONF_TAG, 0x4242u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TFLUSH, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 7), 0x4242u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 2u);
}
PGW_TEST(conf_wire_tflush_layout);
PGW_CONF_CASE(conf_wire_tflush_layout);

/* The rule: Twalk is size[4] Twalk tag[2] fid[4] newfid[4] nwname[2]
 * nwname*(wname[s]). */
static void conf_wire_twalk_layout(void)
{
    const char *names[2] = {"sub", "nested.txt"};
    int32_t n = pgw_p9_enc_twalk(g_buf, sizeof g_buf, CONF_TAG, 3u, 4u, 2u, names);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TWALK, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 3u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 11), 4u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 15), 2u);
    /* The first name: length[2] then the bytes, no NUL. */
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 17), 3u);
    PGW_ASSERT_MEM_EQ(g_buf + 19, "sub", 3);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 22), 10u);
    PGW_ASSERT_MEM_EQ(g_buf + 24, "nested.txt", 10);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 4u + 2u + 5u + 12u);
}
PGW_TEST(conf_wire_twalk_layout);
PGW_CONF_CASE(conf_wire_twalk_layout);

/* The rule: Tread is size[4] Tread tag[2] fid[4] offset[8] count[4]. */
static void conf_wire_tread_layout(void)
{
    int32_t n = pgw_p9_enc_tread(g_buf, sizeof g_buf, CONF_TAG, 9u, 0x0102030405060708ull,
                                 4096u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TREAD, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 9u);
    PGW_ASSERT_EQ_U(ref9p_o_u64(g_buf, 11), 0x0102030405060708ull);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 19), 4096u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 8u + 4u);
}
PGW_TEST(conf_wire_tread_layout);
PGW_CONF_CASE(conf_wire_tread_layout);

/* The rule: Twrite is size[4] Twrite tag[2] fid[4] offset[8] count[4]
 * data[count]. */
static void conf_wire_twrite_layout(void)
{
    const char data[5] = {'h', 'e', 'l', 'l', 'o'};
    int32_t n =
            pgw_p9_enc_twrite(g_buf, sizeof g_buf, CONF_TAG, 9u, 6u, data, sizeof data);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TWRITE, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 9u);
    PGW_ASSERT_EQ_U(ref9p_o_u64(g_buf, 11), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 19), (uint32_t)sizeof data);
    PGW_ASSERT_MEM_EQ(g_buf + 23, data, sizeof data);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 8u + 4u + sizeof data);
}
PGW_TEST(conf_wire_twrite_layout);
PGW_CONF_CASE(conf_wire_twrite_layout);

/* The rule: Rread is size[4] Rread tag[2] count[4] data[count] and Rwrite is
 * size[4] Rwrite tag[2] count[4]. */
static void conf_wire_rread_and_rwrite_layout(void)
{
    /* An Rread built by hand at the offsets of the description. */
    size_t n = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RREAD);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u32(g_buf, n, 3u);
    memcpy(g_buf + n + 4, "abc", 3);
    n += 4 + 3;
    ref9p_o_seal(g_buf, n);
    uint32_t count = 0;
    const uint8_t *data = NULL;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, n, CONF_TAG, &count, &data), PGW_OK);
    PGW_ASSERT_EQ_U(count, 3u);
    PGW_ASSERT_MEM_EQ(data, "abc", 3);

    /* An Rwrite carries the number of bytes written, nothing else. */
    n = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RWRITE);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u32(g_buf, n, 3u);
    n += 4;
    ref9p_o_seal(g_buf, n);
    PGW_ASSERT_EQ_I(pgw_p9_dec_rwrite(g_buf, n, CONF_TAG, &count), PGW_OK);
    PGW_ASSERT_EQ_U(count, 3u);
    PGW_ASSERT_EQ_U(n, (size_t)PGW_HEADER_SIZE + 4u);
}
PGW_TEST(conf_wire_rread_and_rwrite_layout);
PGW_CONF_CASE(conf_wire_rread_and_rwrite_layout);

/* The rule: Tclunk is size[4] Tclunk tag[2] fid[4]. */
static void conf_wire_tclunk_layout(void)
{
    int32_t n = pgw_p9_enc_tclunk(g_buf, sizeof g_buf, CONF_TAG, 0x2Au);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TCLUNK, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 0x2Au);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u);
}
PGW_TEST(conf_wire_tclunk_layout);
PGW_CONF_CASE(conf_wire_tclunk_layout);

/* The rules: Tattach is size[4] Tattach tag[2] fid[4] afid[4] uname[s]
 * aname[s] n_uname[4]; afid is P9_NOFID when there is no authentication; and
 * n_uname carries the uid of the user. */
static void conf_wire_tattach_layout(void)
{
    int32_t n = pgw_p9_enc_tattach(g_buf, sizeof g_buf, CONF_TAG, 0u, PGW_FID_NONE,
                                   "baptiste", "/export", 1000u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TATTACH, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 0u);
    /* afid[4] is ~0: P9_NOFID. */
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 11), 0xFFFFFFFFu);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 15), 8u);
    PGW_ASSERT_MEM_EQ(g_buf + 17, "baptiste", 8);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 25), 7u);
    PGW_ASSERT_MEM_EQ(g_buf + 27, "/export", 7);
    /* n_uname[4] is the uid of the user. */
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 34), 1000u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 4u + 10u + 9u + 4u);
}
PGW_TEST(conf_wire_tattach_layout);
PGW_CONF_CASE(conf_wire_tattach_layout);

/* The rule: Rlerror is size[4] Rlerror tag[2] ecode[4], and ecode is a Linux
 * errno. */
static void conf_wire_rlerror_carries_a_linux_errno(void)
{
    size_t n = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RLERROR);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u32(g_buf, n, 2u); /* ENOENT */
    n += 4;
    ref9p_o_seal(g_buf, n);
    uint32_t ecode = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rlerror(g_buf, n, CONF_TAG, &ecode), PGW_OK);
    PGW_ASSERT_EQ_U(ecode, 2u);
    PGW_ASSERT_EQ_U(n, (size_t)PGW_HEADER_SIZE + 4u);
}
PGW_TEST(conf_wire_rlerror_carries_a_linux_errno);
PGW_CONF_CASE(conf_wire_rlerror_carries_a_linux_errno);
