/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/test_ref9p_fields.c - the server's replies, read at the offsets
 * of the normative 9P2000.L message table.
 *
 * Every case below drives the reference server with a request (built by our
 * encoders, so the server must also accept its output byte for byte), then
 * reads the reply twice through two independent paths:
 *
 *   - ref9p_o_* (the oracle, plain byte access at the documented offsets);
 *   - pgw_p9_dec_* (the decoder of the code under test).
 *
 * Any disagreement - a wrong message size, a wrong type or tag, a wrong number
 * of Twalk elements, a wrong string length, a wrong Tread count - fails the
 * case. This is the "no self-reference" half of P9-10 and the validation
 * surface of P9-09.
 */
#define _POSIX_C_SOURCE 200809L

#include "ref9p_fields.h"
#include "ref9p_truth.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void test_fields_rwalk_element_count(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    uint16_t nwqid = 0;
    pgw_p9_qid qids[PGW_MAXWELEM];

    const char *one[1] = {"sub"};
    int32_t req = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, 10u,
                                   1u, one);
    size_t len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 9u + 13u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_rx, 7), 1u); /* nwqid[2] */
    PGW_ASSERT_EQ_U(ref9p_o_u8(ref9p_rx, 9), PGW_P9_QTDIR);
    PGW_ASSERT_EQ_I(
            pgw_p9_dec_rwalk(ref9p_rx, len, REF9P_FIELD_TAG, &nwqid, qids, PGW_MAXWELEM),
            PGW_OK);
    PGW_ASSERT_EQ_U(nwqid, 1u);
    PGW_ASSERT_EQ_U(qids[0].path, ref9p_o_u64(ref9p_rx, 14));

    const char *two[2] = {"sub", "nested.txt"};
    req = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, 11u, 2u, two);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    PGW_ASSERT_EQ_U(len, 9u + 26u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_rx, 7), 2u);
    PGW_ASSERT_EQ_U(ref9p_o_u8(ref9p_rx, 9 + 13), 0u); /* qid[1] is a file */
    PGW_ASSERT_EQ_I(
            pgw_p9_dec_rwalk(ref9p_rx, len, REF9P_FIELD_TAG, &nwqid, qids, PGW_MAXWELEM),
            PGW_OK);
    PGW_ASSERT_EQ_U(nwqid, 2u);

    /* A missing name is a partial walk of zero elements, never an Rlerror. */
    const char *absent[1] = {"absent"};
    req = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, 12u, 1u,
                           absent);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    PGW_ASSERT_EQ_U(len, 9u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_rx, 7), 0u);
    PGW_ASSERT_EQ_I(
            pgw_p9_dec_rwalk(ref9p_rx, len, REF9P_FIELD_TAG, &nwqid, qids, PGW_MAXWELEM),
            PGW_OK);
    PGW_ASSERT_EQ_U(nwqid, 0u);
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rwalk_element_count);

static void test_fields_rread_count_and_data(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    size_t len = ref9p_walk_to(srv, 20u, "hello.txt");
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);
    ref9p_lopen(srv, 20u, 0u /* O_RDONLY */);

    int32_t req =
            pgw_p9_enc_tread(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 20u, 0u, 5u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RREAD, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 7), 5u); /* count[4] */
    PGW_ASSERT_EQ_U(len, 11u + 5u);                /* count + exactly the data */
    PGW_ASSERT_MEM_EQ(ref9p_rx + 11, "hello", 5);
    uint32_t count = 0;
    const uint8_t *data = NULL;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(ref9p_rx, len, REF9P_FIELD_TAG, &count, &data),
                    PGW_OK);
    PGW_ASSERT_EQ_U(count, 5u);
    PGW_ASSERT_MEM_EQ(data, "hello", 5);

    /* Reading past the end returns what is there, never more. */
    req = pgw_p9_enc_tread(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 20u, 0u, 4096u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 7), 12u);
    PGW_ASSERT_EQ_U(len, 11u + 12u);

    /* Reading at the end is an empty reply of exactly 11 bytes. */
    req = pgw_p9_enc_tread(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 20u, 12u, 64u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    PGW_ASSERT_EQ_U(len, 11u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 7), 0u);
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rread_count_and_data);

static void test_fields_rgetattr_offsets(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    size_t len = ref9p_walk_to(srv, 21u, "hello.txt");
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);

    int32_t req = pgw_p9_enc_tgetattr(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 21u,
                                      PGW_P9_GETATTR_BASIC);
    PGW_ASSERT_EQ_U((size_t)req, 19u); /* fid[4] request_mask[8] */
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RGETATTR, REF9P_FIELD_TAG);
    /* valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8]
     * blksize[8] blocks[8], six timestamp halves, then the four reserved
     * fields: 8 + 13 + 12 + 40 + 48 + 32 = 153 bytes of body. */
    PGW_ASSERT_EQ_U(len, 7u + 153u);
    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(ref9p_root(srv), "hello.txt", &st));
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_rx, 7), PGW_P9_GETATTR_BASIC);
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_rx, 20), (uint64_t)st.st_ino);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 28), (uint32_t)st.st_mode);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 32), (uint32_t)st.st_uid);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 36), (uint32_t)st.st_gid);
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_rx, 40), (uint64_t)st.st_nlink);
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_rx, 56), (uint64_t)st.st_size);
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_rx, 80), (uint64_t)st.st_atim.tv_sec);
    PGW_ASSERT_EQ_U(ref9p_o_u64(ref9p_rx, 96), (uint64_t)st.st_mtim.tv_sec);

    pgw_p9_attr attr;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rgetattr(ref9p_rx, len, REF9P_FIELD_TAG, &attr), PGW_OK);
    PGW_ASSERT_EQ_U(attr.mode, ref9p_o_u32(ref9p_rx, 28));
    PGW_ASSERT_EQ_U(attr.size, ref9p_o_u64(ref9p_rx, 56));
    PGW_ASSERT_EQ_U(attr.nlink, ref9p_o_u64(ref9p_rx, 40));
    PGW_ASSERT_EQ_U(attr.qid.path, ref9p_o_u64(ref9p_rx, 20));
    PGW_ASSERT_EQ_U(attr.atime_sec, ref9p_o_u64(ref9p_rx, 80));
    PGW_ASSERT_EQ_U(attr.mtime_sec, ref9p_o_u64(ref9p_rx, 96));
    PGW_ASSERT_EQ_U(attr.ctime_sec, ref9p_o_u64(ref9p_rx, 112));
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rgetattr_offsets);

static void test_fields_rreaddir_records(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    int32_t req = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, 30u,
                                   0u, NULL);
    size_t len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 9u); /* a clone: nwqid[2] = 0, nothing else */

    req = pgw_p9_enc_treaddir(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 30u, 0u, 4096u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RREADDIR, REF9P_FIELD_TAG);
    uint32_t count = ref9p_o_u32(ref9p_rx, 7);
    PGW_ASSERT_EQ_U(len, 11u + count);

    /* Each record is qid[13] offset[8] type[1] name[s]; their lengths must
     * add up to exactly count. */
    size_t off = 11u;
    size_t seen = 0;
    size_t sum = 0;
    while (off < 11u + count) {
        uint16_t nlen = ref9p_o_u16(ref9p_rx, off + 13u + 8u + 1u);
        size_t record = 13u + 8u + 1u + 2u + (size_t)nlen;
        off += record;
        sum += record;
        seen++;
    }
    PGW_ASSERT_EQ_U(sum, count);
    PGW_ASSERT_EQ_U(off, 11u + count);
    PGW_ASSERT(seen >= 3u); /* hello.txt, sub, alias */

    pgw_p9_dirent ents[16];
    size_t nents = 0;
    PGW_ASSERT_EQ_I(
            pgw_p9_dec_rreaddir_iter(ref9p_rx, len, REF9P_FIELD_TAG, ents, 16, &nents),
            PGW_OK);
    PGW_ASSERT_EQ_U(nents, seen);
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rreaddir_records);

static void test_fields_rlerror_layout(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    int32_t req = pgw_p9_enc_tgetattr(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 9999u,
                                      PGW_P9_GETATTR_BASIC);
    size_t len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RLERROR, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 11u);                     /* header + ecode[4] */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 7), 9u); /* EBADF on Linux */
    uint32_t code = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rlerror(ref9p_rx, len, REF9P_FIELD_TAG, &code), PGW_OK);
    PGW_ASSERT_EQ_U(code, 9u);
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rlerror_layout);

static void test_fields_rstatfs_and_empty_replies(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    int32_t req = pgw_p9_enc_tstatfs(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u);
    size_t len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RSTATFS, REF9P_FIELD_TAG);
    /* type[4] bsize[4] blocks[8] bfree[8] bavail[8] files[8] ffree[8]
     * fsid[8] namelen[4]. */
    PGW_ASSERT_EQ_U(len, 67u);
    pgw_p9_statfs_t st;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rstatfs(ref9p_rx, len, REF9P_FIELD_TAG, &st), PGW_OK);
    PGW_ASSERT_EQ_U(st.type, ref9p_o_u32(ref9p_rx, 7));
    PGW_ASSERT_EQ_U(st.bsize, ref9p_o_u32(ref9p_rx, 11));
    PGW_ASSERT_EQ_U(st.blocks, ref9p_o_u64(ref9p_rx, 15));
    PGW_ASSERT_EQ_U(st.namelen, ref9p_o_u32(ref9p_rx, 63));

    /* Tclunk over a cloned fid answers with a bare header, nothing else. */
    req = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, 31u, 0u, NULL);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    req = pgw_p9_enc_tclunk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 31u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RCLUNK, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 7u);

    /* Tflush also answers with a bare header. */
    req = pgw_p9_enc_tflush(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RFLUSH, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 7u);
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rstatfs_and_empty_replies);

static void test_fields_rreadlink_and_rwrite(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    size_t len = ref9p_walk_to(srv, 50u, "alias");
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);

    int32_t req = pgw_p9_enc_treadlink(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 50u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RREADLINK, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_rx, 7), 9u); /* strlen("hello.txt") */
    PGW_ASSERT_EQ_U(len, 7u + 2u + 9u);
    PGW_ASSERT_MEM_EQ(ref9p_rx + 9, "hello.txt", 9);
    char target[64];
    PGW_ASSERT_EQ_I(
            pgw_p9_dec_rreadlink(ref9p_rx, len, REF9P_FIELD_TAG, target, sizeof target),
            PGW_OK);
    PGW_ASSERT_STR_EQ(target, "hello.txt");

    len = ref9p_walk_to(srv, 51u, "hello.txt");
    ref9p_lopen(srv, 51u, 1u /* O_WRONLY */);
    static const char payload[] = "abc";
    req = pgw_p9_enc_twrite(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 51u, 0u, payload,
                            3u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWRITE, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 11u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 7), 3u);
    uint32_t count = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rwrite(ref9p_rx, len, REF9P_FIELD_TAG, &count), PGW_OK);
    PGW_ASSERT_EQ_U(count, 3u);
    char back[16];
    size_t n = 0;
    PGW_ASSERT(ref9p_truth_read(ref9p_root(srv), "hello.txt", back, sizeof back, &n));
    /* pwrite at offset 0 replaces the first three bytes without truncating. */
    PGW_ASSERT(n >= 3u);
    PGW_ASSERT_MEM_EQ(back, "abc", 3);
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rreadlink_and_rwrite);

static void test_fields_rcreate_replies(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    /* Rmkdir and Rsymlink carry one qid and nothing else. */
    int32_t req = pgw_p9_enc_tmkdir(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, "d1",
                                    0755u, 0u);
    size_t len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RMKDIR, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 20u);
    PGW_ASSERT_EQ_U(ref9p_o_u8(ref9p_rx, 7), PGW_P9_QTDIR);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rmkdir(ref9p_rx, len, REF9P_FIELD_TAG, &qid), PGW_OK);
    PGW_ASSERT_EQ_U(qid.type, PGW_P9_QTDIR);

    req = pgw_p9_enc_tsymlink(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, "l1",
                              "hello.txt", 0u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RSYMLINK, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 20u);
    PGW_ASSERT_EQ_U(ref9p_o_u8(ref9p_rx, 7), PGW_P9_QTSYMLINK);

    /* Rlcreate carries qid[13] then iounit[4]. */
    req = pgw_p9_enc_tlcreate(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, "f1", 0x42u,
                              0644u, 0u);
    len = ref9p_call(srv, ref9p_tx, (size_t)req);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RLCREATE, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, 24u);
    PGW_ASSERT_EQ_U(ref9p_o_u8(ref9p_rx, 7), 0u);   /* qid type: a file */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 20), 0u); /* iounit[4] */
    ref9p_stop(srv);
}
PGW_TEST(test_fields_rcreate_replies);
