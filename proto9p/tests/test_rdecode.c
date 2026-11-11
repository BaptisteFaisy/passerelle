/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_rdecode.c - every R-message decoder, field by field.
 *
 * The messages are built by hand with pgw_out, exactly as a server would put
 * them on the wire, and then decoded with the public entry points of the
 * contract. Each case checks both the values and that the decoder consumed the
 * whole message (P9-09: no trailing byte is ever accepted).
 */
#include "pgw/buf.h"
#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <string.h>

static uint8_t g_buf[2048];

static pgw_out begin(uint8_t type, uint16_t tag)
{
    pgw_out w;
    pgw_out_init(&w, g_buf, sizeof g_buf);
    pgw_put_u32(&w, 0);
    pgw_put_u8(&w, type);
    pgw_put_u16(&w, tag);
    return w;
}

static size_t finish(pgw_out *w)
{
    PGW_ASSERT(pgw_out_ok(w));
    PGW_ASSERT(pgw_patch_len_u32(w, 0));
    return pgw_written(w);
}

static void put_qid(pgw_out *w, uint8_t type, uint32_t version, uint64_t path)
{
    pgw_put_u8(w, type);
    pgw_put_u32(w, version);
    pgw_put_u64(w, path);
}

static void test_dec_rlerror(void)
{
    pgw_out w = begin(PGW_P9_RLERROR, 5);
    pgw_put_u32(&w, 2); /* ENOENT */
    size_t n = finish(&w);
    uint32_t ecode = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rlerror(g_buf, n, 5, &ecode), PGW_OK);
    PGW_ASSERT_EQ_U(ecode, 2u);
}
PGW_TEST(test_dec_rlerror);

static void test_dec_rversion(void)
{
    pgw_out w = begin(PGW_P9_RVERSION, PGW_TAG_NONE);
    pgw_put_u32(&w, 8192);
    PGW_ASSERT(pgw_put_str(&w, "9P2000.L", 8));
    size_t n = finish(&w);
    uint32_t msize = 0;
    char ver[16];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rversion(g_buf, n, PGW_TAG_NONE, &msize, ver,
                                        sizeof ver),
                    PGW_OK);
    PGW_ASSERT_EQ_U(msize, 8192u);
    PGW_ASSERT_STR_EQ(ver, "9P2000.L");

    /* A dialect string longer than the caller's buffer is an error, not a
     * silent truncation: it would hide a server speaking something else. */
    char tiny[4];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rversion(g_buf, n, PGW_TAG_NONE, &msize, tiny,
                                        sizeof tiny),
                    PGW_ENAMETOOLONG);
}
PGW_TEST(test_dec_rversion);

static void test_dec_rattach_and_rwalk(void)
{
    pgw_out w = begin(PGW_P9_RATTACH, 9);
    put_qid(&w, PGW_P9_QTDIR, 3, 0x1122334455667788ULL);
    size_t n = finish(&w);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rattach(g_buf, n, 9, &qid), PGW_OK);
    PGW_ASSERT_EQ_U(qid.type, PGW_P9_QTDIR);
    PGW_ASSERT_EQ_U(qid.version, 3u);
    PGW_ASSERT_EQ_U(qid.path, 0x1122334455667788ULL);

    w = begin(PGW_P9_RWALK, 10);
    pgw_put_u16(&w, 2);
    put_qid(&w, PGW_P9_QTDIR, 1, 100);
    put_qid(&w, PGW_P9_QTFILE, 2, 200);
    n = finish(&w);
    pgw_p9_qid qids[PGW_MAXWELEM];
    uint16_t nwqid = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rwalk(g_buf, n, 10, &nwqid, qids,
                                     (uint16_t)PGW_MAXWELEM),
                    PGW_OK);
    PGW_ASSERT_EQ_U(nwqid, 2);
    PGW_ASSERT_EQ_U(qids[0].path, 100u);
    PGW_ASSERT_EQ_U(qids[1].path, 200u);

    /* A zero-name Rwalk (a clone) is valid. */
    w = begin(PGW_P9_RWALK, 11);
    pgw_put_u16(&w, 0);
    n = finish(&w);
    nwqid = 99;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rwalk(g_buf, n, 11, &nwqid, qids,
                                     (uint16_t)PGW_MAXWELEM),
                    PGW_OK);
    PGW_ASSERT_EQ_U(nwqid, 0);
}
PGW_TEST(test_dec_rattach_and_rwalk);

static void test_dec_rlopen_rread_rwrite(void)
{
    pgw_out w = begin(PGW_P9_RLOPEN, 12);
    put_qid(&w, PGW_P9_QTFILE, 0, 42);
    pgw_put_u32(&w, 8192);
    size_t n = finish(&w);
    pgw_p9_qid qid;
    uint32_t iounit = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rlopen(g_buf, n, 12, &qid, &iounit), PGW_OK);
    PGW_ASSERT_EQ_U(qid.path, 42u);
    PGW_ASSERT_EQ_U(iounit, 8192u);

    w = begin(PGW_P9_RREAD, 13);
    pgw_put_u32(&w, 5);
    PGW_ASSERT_EQ_U(pgw_put_data(&w, "abcde", 5), 5u);
    n = finish(&w);
    uint32_t count = 0;
    const uint8_t *data = NULL;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, n, 13, &count, &data), PGW_OK);
    PGW_ASSERT_EQ_U(count, 5u);
    PGW_ASSERT_MEM_EQ(data, "abcde", 5);
    PGW_ASSERT_EQ_PTR(data, g_buf + PGW_HEADER_SIZE + 4); /* zero copy */

    /* An empty read is the end of file, not an error. */
    w = begin(PGW_P9_RREAD, 14);
    pgw_put_u32(&w, 0);
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, n, 14, &count, &data), PGW_OK);
    PGW_ASSERT_EQ_U(count, 0u);

    w = begin(PGW_P9_RWRITE, 15);
    pgw_put_u32(&w, 4096);
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_p9_dec_rwrite(g_buf, n, 15, &count), PGW_OK);
    PGW_ASSERT_EQ_U(count, 4096u);
}
PGW_TEST(test_dec_rlopen_rread_rwrite);

static void test_dec_rgetattr(void)
{
    pgw_out w = begin(PGW_P9_RGETATTR, 16);
    pgw_put_u64(&w, PGW_P9_GETATTR_BASIC);
    put_qid(&w, PGW_P9_QTDIR, 7, 0xABCDEF0123456789ULL);
    pgw_put_u32(&w, 040755u);
    pgw_put_u32(&w, 1000);
    pgw_put_u32(&w, 1001);
    pgw_put_u64(&w, 3);      /* nlink */
    pgw_put_u64(&w, 0);      /* rdev */
    pgw_put_u64(&w, 4096);   /* size */
    pgw_put_u64(&w, 512);    /* blksize */
    pgw_put_u64(&w, 8);      /* blocks */
    pgw_put_u64(&w, 11);     /* atime_sec */
    pgw_put_u64(&w, 12);     /* atime_nsec */
    pgw_put_u64(&w, 13);     /* mtime_sec */
    pgw_put_u64(&w, 14);     /* mtime_nsec */
    pgw_put_u64(&w, 15);     /* ctime_sec */
    pgw_put_u64(&w, 16);     /* ctime_nsec */
    pgw_put_u64(&w, 17);     /* btime_sec, reserved */
    pgw_put_u64(&w, 18);     /* btime_nsec, reserved */
    pgw_put_u64(&w, 19);     /* gen, reserved */
    pgw_put_u64(&w, 20);     /* data_version, reserved */
    size_t n = finish(&w);
    pgw_p9_attr a;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rgetattr(g_buf, n, 16, &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.valid, PGW_P9_GETATTR_BASIC);
    PGW_ASSERT_EQ_U(a.qid.path, 0xABCDEF0123456789ULL);
    PGW_ASSERT_EQ_U(a.mode, 040755u);
    PGW_ASSERT_EQ_U(a.uid, 1000u);
    PGW_ASSERT_EQ_U(a.gid, 1001u);
    PGW_ASSERT_EQ_U(a.nlink, 3u);
    PGW_ASSERT_EQ_U(a.size, 4096u);
    PGW_ASSERT_EQ_U(a.blksize, 512u);
    PGW_ASSERT_EQ_U(a.blocks, 8u);
    PGW_ASSERT_EQ_U(a.atime_sec, 11u);
    PGW_ASSERT_EQ_U(a.atime_nsec, 12u);
    PGW_ASSERT_EQ_U(a.mtime_sec, 13u);
    PGW_ASSERT_EQ_U(a.mtime_nsec, 14u);
    PGW_ASSERT_EQ_U(a.ctime_sec, 15u);
    PGW_ASSERT_EQ_U(a.ctime_nsec, 16u);
}
PGW_TEST(test_dec_rgetattr);

static void test_dec_rreaddir(void)
{
    pgw_out w = begin(PGW_P9_RREADDIR, 17);
    size_t count_off = pgw_written(&w);
    pgw_put_u32(&w, 0);
    size_t payload_off = pgw_written(&w);
    put_qid(&w, PGW_P9_QTDIR, 0, 1);
    pgw_put_u64(&w, 1);
    pgw_put_u8(&w, (uint8_t)PGW_DT_DIR);
    PGW_ASSERT(pgw_put_str(&w, "..", 2));
    put_qid(&w, PGW_P9_QTFILE, 0, 2);
    pgw_put_u64(&w, 2);
    pgw_put_u8(&w, (uint8_t)PGW_DT_REG);
    PGW_ASSERT(pgw_put_str(&w, "hello.txt", 9));
    size_t payload = pgw_written(&w) - payload_off;
    uint8_t *p = pgw_out_at(&w, count_off);
    PGW_ASSERT(p != NULL);
    p[0] = (uint8_t)payload;
    p[1] = (uint8_t)(payload >> 8);
    p[2] = (uint8_t)(payload >> 16);
    p[3] = (uint8_t)(payload >> 24);
    size_t n = finish(&w);

    pgw_p9_dirent ents[8];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, n, 17, ents, 8, &got),
                    PGW_OK);
    PGW_ASSERT_EQ_U(got, 2u);
    PGW_ASSERT_STR_EQ(ents[0].name, "..");
    PGW_ASSERT_EQ_U(ents[0].offset, 1u);
    PGW_ASSERT_EQ_U(ents[0].type, PGW_DT_DIR);
    PGW_ASSERT_STR_EQ(ents[1].name, "hello.txt");
    PGW_ASSERT_EQ_U(ents[1].name_len, 9u);
    PGW_ASSERT_EQ_U(ents[1].qid.path, 2u);

    /* An array too small for the reply is an explicit error. */
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, n, 17, ents, 1, &got),
                    PGW_ENOMEM);

    /* An empty reply is the end of the directory. */
    w = begin(PGW_P9_RREADDIR, 18);
    pgw_put_u32(&w, 0);
    n = finish(&w);
    got = 77;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, n, 18, ents, 8, &got),
                    PGW_OK);
    PGW_ASSERT_EQ_U(got, 0u);
}
PGW_TEST(test_dec_rreaddir);

static void test_dec_readlink_rmkdir_rstatfs(void)
{
    pgw_out w = begin(PGW_P9_RREADLINK, 19);
    PGW_ASSERT(pgw_put_str(&w, "hello.txt", 9));
    size_t n = finish(&w);
    char target[64];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreadlink(g_buf, n, 19, target, sizeof target),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, "hello.txt");
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreadlink(g_buf, n, 19, target, 4),
                    PGW_ENAMETOOLONG);

    w = begin(PGW_P9_RMKDIR, 20);
    put_qid(&w, PGW_P9_QTDIR, 0, 99);
    n = finish(&w);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rmkdir(g_buf, n, 20, &qid), PGW_OK);
    PGW_ASSERT_EQ_U(qid.path, 99u);

    w = begin(PGW_P9_RSTATFS, 21);
    pgw_put_u32(&w, 0x01021997u);
    pgw_put_u32(&w, 4096);
    pgw_put_u64(&w, 1);
    pgw_put_u64(&w, 2);
    pgw_put_u64(&w, 3);
    pgw_put_u64(&w, 4);
    pgw_put_u64(&w, 5);
    pgw_put_u64(&w, 6);
    pgw_put_u32(&w, 255);
    n = finish(&w);
    pgw_p9_statfs_t st;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rstatfs(g_buf, n, 21, &st), PGW_OK);
    PGW_ASSERT_EQ_U(st.type, 0x01021997u);
    PGW_ASSERT_EQ_U(st.bsize, 4096u);
    PGW_ASSERT_EQ_U(st.blocks, 1u);
    PGW_ASSERT_EQ_U(st.bavail, 3u);
    PGW_ASSERT_EQ_U(st.fsid, 6u);
    PGW_ASSERT_EQ_U(st.namelen, 255u);
}
PGW_TEST(test_dec_readlink_rmkdir_rstatfs);

static void test_dec_empty_replies(void)
{
    static const uint8_t types[] = { PGW_P9_RCLUNK, PGW_P9_RSETATTR,
                                     PGW_P9_RFSYNC, PGW_P9_RLINK,
                                     PGW_P9_RRENAMEAT, PGW_P9_RUNLINKAT,
                                     PGW_P9_RFLUSH };
    for (size_t i = 0; i < sizeof types; i++) {
        pgw_out w = begin(types[i], 22);
        size_t n = finish(&w);
        PGW_ASSERT_EQ_I(pgw_p9_dec_empty(g_buf, n, types[i], 22), PGW_OK);
        /* One byte too many is a protocol violation. */
        pgw_put_u8(&w, 0);
        n = finish(&w);
        PGW_ASSERT_EQ_I(pgw_p9_dec_empty(g_buf, n, types[i], 22), PGW_EINVAL);
    }
}
PGW_TEST(test_dec_empty_replies);

static void test_dec_header_rules(void)
{
    pgw_p9_hdr h;
    /* Too short for a header. */
    PGW_ASSERT_EQ_I(pgw_p9_dec_hdr(g_buf, 6, &h), PGW_EMSGSIZE);
    /* size[4] below the header length. */
    pgw_out w = begin(PGW_P9_RCLUNK, 1);
    size_t n = finish(&w);
    uint8_t *p = pgw_out_at(&w, 0);
    PGW_ASSERT(p != NULL);
    p[0] = 6;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_hdr(g_buf, n, &h), PGW_EINVAL);
    /* size[4] larger than what was received. */
    p[0] = 40;
    PGW_ASSERT_EQ_I(pgw_p9_dec_hdr(g_buf, n, &h), PGW_EMSGSIZE);
    /* NULL argument. */
    PGW_ASSERT_EQ_I(pgw_p9_dec_hdr(NULL, 0, &h), PGW_EINVAL);
}
PGW_TEST(test_dec_header_rules);
