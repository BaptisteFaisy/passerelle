/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/test_conformance_wire_ops.c - the bytes of the
 * operations diod describes.
 *
 * The companion of test_conformance_wire.c: the layouts of the 9P2000.L
 * operations (statfs, lopen, lcreate, getattr, setattr, readdir, mkdir,
 * symlink, readlink, link) are checked the same way - the message is read at
 * the offsets of diod's protocol.md with the independent accessors of
 * tests/ref9p/, never with a helper of the code under test.
 *
 * The replies the description shows are built by hand here, byte by byte,
 * so that the decoder of proto9p/ is checked against the description and not
 * against another buffer of ours.
 *
 * Covers P9-02, P9-03, P9-09 and gap 60.
 */
#define _POSIX_C_SOURCE 200809L

#include "conformance_table.h"

#include "ref9p_fields.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <string.h>

#define CONF_TAG 0x1234u

static uint8_t g_buf[512];

/* The rule: Rstatfs is size[4] Rstatfs tag[2] type[4] bsize[4] blocks[8]
 * bfree[8] bavail[8] files[8] ffree[8] fsid[8] namelen[4]. */
static void conf_wire_trstatfs_layout(void)
{
    int32_t n = pgw_p9_enc_tstatfs(g_buf, sizeof g_buf, CONF_TAG, 5u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TSTATFS, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 5u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u);

    /* The reply, built at the offsets of the description. */
    size_t len = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RSTATFS);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u32(g_buf, len, 0x0100u);   /* type[4] */
    ref9p_o_put_u32(g_buf, len + 4, 4096u); /* bsize[4] */
    ref9p_o_put_u64(g_buf, len + 8, 100u);  /* blocks[8] */
    ref9p_o_put_u64(g_buf, len + 16, 50u);  /* bfree[8] */
    ref9p_o_put_u64(g_buf, len + 24, 40u);  /* bavail[8] */
    ref9p_o_put_u64(g_buf, len + 32, 10u);  /* files[8] */
    ref9p_o_put_u64(g_buf, len + 40, 5u);   /* ffree[8] */
    ref9p_o_put_u64(g_buf, len + 48, 77u);  /* fsid[8] */
    ref9p_o_put_u32(g_buf, len + 56, 255u); /* namelen[4] */
    len += 60;
    ref9p_o_seal(g_buf, len);
    PGW_ASSERT_EQ_U(len, 67u);
    pgw_p9_statfs_t st;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rstatfs(g_buf, len, CONF_TAG, &st), PGW_OK);
    PGW_ASSERT_EQ_U(st.type, 0x0100u);
    PGW_ASSERT_EQ_U(st.bsize, 4096u);
    PGW_ASSERT_EQ_U(st.blocks, 100u);
    PGW_ASSERT_EQ_U(st.bfree, 50u);
    PGW_ASSERT_EQ_U(st.bavail, 40u);
    PGW_ASSERT_EQ_U(st.files, 10u);
    PGW_ASSERT_EQ_U(st.ffree, 5u);
    PGW_ASSERT_EQ_U(st.fsid, 77u);
    PGW_ASSERT_EQ_U(st.namelen, 255u);
}
PGW_TEST(conf_wire_trstatfs_layout);
PGW_CONF_CASE(conf_wire_trstatfs_layout);

/* The rule: Tlopen is size[4] Tlopen tag[2] fid[4] flags[4] and Rlopen is
 * size[4] Rlopen tag[2] qid[13] iounit[4]. */
static void conf_wire_tlopen_layout(void)
{
    int32_t n = pgw_p9_enc_tlopen(g_buf, sizeof g_buf, CONF_TAG, 6u, 0u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TLOPEN, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 11), 0u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 8u);

    size_t len = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RLOPEN);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u8(g_buf, len, PGW_P9_QTFILE);
    ref9p_o_put_u32(g_buf, len + 1, 0u);
    ref9p_o_put_u64(g_buf, len + 5, 42u);
    ref9p_o_put_u32(g_buf, len + 13, 4096u); /* iounit[4] */
    len += 17;
    ref9p_o_seal(g_buf, len);
    pgw_p9_qid qid;
    uint32_t iounit = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rlopen(g_buf, len, CONF_TAG, &qid, &iounit), PGW_OK);
    PGW_ASSERT_EQ_U(qid.path, 42u);
    PGW_ASSERT_EQ_U(iounit, 4096u);
}
PGW_TEST(conf_wire_tlopen_layout);
PGW_CONF_CASE(conf_wire_tlopen_layout);

/* The rule: Tlcreate is size[4] Tlcreate tag[2] fid[4] name[s] flags[4]
 * mode[4] gid[4] and Rlcreate is size[4] Rlcreate tag[2] qid[13] iounit[4]. */
static void conf_wire_tlcreate_layout(void)
{
    int32_t n = pgw_p9_enc_tlcreate(g_buf, sizeof g_buf, CONF_TAG, 6u, "foo", 0x8241u,
                                    0100644u, 1000u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TLCREATE, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 11), 3u);
    PGW_ASSERT_MEM_EQ(g_buf + 13, "foo", 3);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 16), 0x8241u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 20), 0100644u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 24), 1000u);
    PGW_ASSERT_EQ_U((size_t)n, 28u);
}
PGW_TEST(conf_wire_tlcreate_layout);
PGW_CONF_CASE(conf_wire_tlcreate_layout);

/* The rule: Tgetattr is size[4] Tgetattr tag[2] fid[4] request_mask[8]. */
static void conf_wire_tgetattr_layout(void)
{
    int32_t n =
            pgw_p9_enc_tgetattr(g_buf, sizeof g_buf, CONF_TAG, 6u, PGW_P9_GETATTR_BASIC);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TGETATTR, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u64(g_buf, 11), 0x7FFull);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 8u);
}
PGW_TEST(conf_wire_tgetattr_layout);
PGW_CONF_CASE(conf_wire_tgetattr_layout);

/* The rule: Rgetattr is size[4] Rgetattr tag[2] valid[8] qid[13] mode[4]
 * uid[4] gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8] atime_sec[8]
 * atime_nsec[8] mtime_sec[8] mtime_nsec[8] ctime_sec[8] ctime_nsec[8]
 * btime_sec[8] btime_nsec[8] gen[8] data_version[8]. */
static void conf_wire_rgetattr_layout(void)
{
    size_t len = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RGETATTR);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u64(g_buf, len, 0x7FFull);        /* valid[8] */
    ref9p_o_put_u8(g_buf, len + 8, PGW_P9_QTDIR); /* qid[13] */
    ref9p_o_put_u32(g_buf, len + 9, 0u);
    ref9p_o_put_u64(g_buf, len + 13, 99u);
    ref9p_o_put_u32(g_buf, len + 21, 040755u); /* mode[4] */
    ref9p_o_put_u32(g_buf, len + 25, 500u);    /* uid[4] */
    ref9p_o_put_u32(g_buf, len + 29, 500u);    /* gid[4] */
    ref9p_o_put_u64(g_buf, len + 33, 2u);      /* nlink[8] */
    ref9p_o_put_u64(g_buf, len + 41, 0u);      /* rdev[8] */
    ref9p_o_put_u64(g_buf, len + 49, 4096u);   /* size[8] */
    ref9p_o_put_u64(g_buf, len + 57, 4096u);   /* blksize[8] */
    ref9p_o_put_u64(g_buf, len + 65, 8u);      /* blocks[8] */
    ref9p_o_put_u64(g_buf, len + 73, 11u);     /* atime_sec[8] */
    ref9p_o_put_u64(g_buf, len + 81, 12u);     /* atime_nsec[8] */
    ref9p_o_put_u64(g_buf, len + 89, 13u);     /* mtime_sec[8] */
    ref9p_o_put_u64(g_buf, len + 97, 14u);     /* mtime_nsec[8] */
    ref9p_o_put_u64(g_buf, len + 105, 15u);    /* ctime_sec[8] */
    ref9p_o_put_u64(g_buf, len + 113, 16u);    /* ctime_nsec[8] */
    ref9p_o_put_u64(g_buf, len + 121, 0u);     /* btime_sec[8] */
    ref9p_o_put_u64(g_buf, len + 129, 0u);     /* btime_nsec[8] */
    ref9p_o_put_u64(g_buf, len + 137, 0u);     /* gen[8] */
    ref9p_o_put_u64(g_buf, len + 145, 0u);     /* data_version[8] */
    len += 153;
    ref9p_o_seal(g_buf, len);
    PGW_ASSERT_EQ_U(len, 160u);
    pgw_p9_attr attr;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rgetattr(g_buf, len, CONF_TAG, &attr), PGW_OK);
    PGW_ASSERT_EQ_U(attr.valid, 0x7FFull);
    PGW_ASSERT_EQ_U(attr.mode, 040755u);
    PGW_ASSERT_EQ_U(attr.uid, 500u);
    PGW_ASSERT_EQ_U(attr.gid, 500u);
    PGW_ASSERT_EQ_U(attr.nlink, 2u);
    PGW_ASSERT_EQ_U(attr.size, 4096u);
    PGW_ASSERT_EQ_U(attr.blksize, 4096u);
    PGW_ASSERT_EQ_U(attr.blocks, 8u);
    PGW_ASSERT_EQ_U(attr.atime_sec, 11u);
    PGW_ASSERT_EQ_U(attr.atime_nsec, 12u);
    PGW_ASSERT_EQ_U(attr.mtime_sec, 13u);
    PGW_ASSERT_EQ_U(attr.ctime_nsec, 16u);
}
PGW_TEST(conf_wire_rgetattr_layout);
PGW_CONF_CASE(conf_wire_rgetattr_layout);

/* The rule: Tsetattr is size[4] Tsetattr tag[2] fid[4] valid[4] mode[4]
 * uid[4] gid[4] size[8] atime_sec[8] atime_nsec[8] mtime_sec[8]
 * mtime_nsec[8]. */
static void conf_wire_tsetattr_layout(void)
{
    pgw_p9_setattr_t sa;
    memset(&sa, 0, sizeof sa);
    sa.valid = PGW_P9_SETATTR_MODE | PGW_P9_SETATTR_SIZE;
    sa.mode = 040000u;
    sa.size = 1234u;
    int32_t n = pgw_p9_enc_tsetattr(g_buf, sizeof g_buf, CONF_TAG, 6u, &sa);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TSETATTR, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 11), sa.valid);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 15), 040000u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 19), 0u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 23), 0u);
    PGW_ASSERT_EQ_U(ref9p_o_u64(g_buf, 27), 1234u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 16u + 40u);
}
PGW_TEST(conf_wire_tsetattr_layout);
PGW_CONF_CASE(conf_wire_tsetattr_layout);

/* The rule: Treaddir is size[4] Treaddir tag[2] fid[4] offset[8] count[4] and
 * Rreaddir is size[4] Rreaddir tag[2] count[4] data[count]. */
static void conf_wire_treaddir_layout(void)
{
    int32_t n = pgw_p9_enc_treaddir(g_buf, sizeof g_buf, CONF_TAG, 6u, 42u, 8192u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TREADDIR, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u64(g_buf, 11), 42u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 19), 8192u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u + 8u + 4u);
}
PGW_TEST(conf_wire_treaddir_layout);
PGW_CONF_CASE(conf_wire_treaddir_layout);

/* The rule: a directory entry is the variable-length record qid[13] offset[8]
 * type[1] name[s]. */
static void conf_wire_readdir_record_layout(void)
{
    /* One record, built at the offsets of the description. */
    static uint8_t rec_buf[64];
    size_t rec = 0;
    ref9p_o_put_u8(rec_buf, rec, PGW_P9_QTFILE); /* qid[13] */
    ref9p_o_put_u32(rec_buf, rec + 1, 0u);
    ref9p_o_put_u64(rec_buf, rec + 5, 7u);
    ref9p_o_put_u64(rec_buf, rec + 13, 99u);       /* offset[8] */
    ref9p_o_put_u8(rec_buf, rec + 21, PGW_DT_REG); /* type[1] */
    rec = ref9p_o_put_str(rec_buf, rec + 22, "hello.txt");
    PGW_ASSERT_EQ_U(rec, 33u);

    /* The reply wraps it: count[4] then the records. */
    size_t len = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RREADDIR);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    ref9p_o_put_u32(g_buf, len, (uint32_t)rec);
    memcpy(g_buf + len + 4, rec_buf, rec);
    len += 4 + rec;
    ref9p_o_seal(g_buf, len);
    pgw_p9_dirent ents[4];
    size_t count = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, len, CONF_TAG, ents, 4, &count),
                    PGW_OK);
    PGW_ASSERT_EQ_U(count, 1u);
    PGW_ASSERT_EQ_U(ents[0].offset, 99u);
    PGW_ASSERT_EQ_U(ents[0].type, PGW_DT_REG);
    PGW_ASSERT_STR_EQ(ents[0].name, "hello.txt");
    PGW_ASSERT_EQ_U(ents[0].qid.path, 7u);
}
PGW_TEST(conf_wire_readdir_record_layout);
PGW_CONF_CASE(conf_wire_readdir_record_layout);

/* The rule: Tmkdir is size[4] Tmkdir tag[2] dfid[4] name[s] mode[4] gid[4]
 * and Rmkdir is size[4] Rmkdir tag[2] qid[13]. */
static void conf_wire_tmkdir_layout(void)
{
    int32_t n = pgw_p9_enc_tmkdir(g_buf, sizeof g_buf, CONF_TAG, 6u, "sub", 0755u, 1000u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TMKDIR, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 11), 3u);
    PGW_ASSERT_MEM_EQ(g_buf + 13, "sub", 3);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 16), 0755u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 20), 1000u);
    PGW_ASSERT_EQ_U((size_t)n, 24u);
}
PGW_TEST(conf_wire_tmkdir_layout);
PGW_CONF_CASE(conf_wire_tmkdir_layout);

/* The rule: Tsymlink is size[4] Tsymlink tag[2] fid[4] name[s] symtgt[s]
 * gid[4] and Rsymlink is size[4] Rsymlink tag[2] qid[13]. */
static void conf_wire_tsymlink_layout(void)
{
    int32_t n = pgw_p9_enc_tsymlink(g_buf, sizeof g_buf, CONF_TAG, 6u, "alias",
                                    "/tmp/9/newdir", 1000u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TSYMLINK, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 11), 5u);
    PGW_ASSERT_MEM_EQ(g_buf + 13, "alias", 5);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 18), 13u);
    PGW_ASSERT_MEM_EQ(g_buf + 20, "/tmp/9/newdir", 13);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 33), 1000u);
    PGW_ASSERT_EQ_U((size_t)n, 37u);
}
PGW_TEST(conf_wire_tsymlink_layout);
PGW_CONF_CASE(conf_wire_tsymlink_layout);

/* The rule: Treadlink is size[4] Treadlink tag[2] fid[4] and Rreadlink is
 * size[4] Rreadlink tag[2] target[s]. */
static void conf_wire_treadlink_layout(void)
{
    int32_t n = pgw_p9_enc_treadlink(g_buf, sizeof g_buf, CONF_TAG, 6u);
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TREADLINK, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U((size_t)n, (size_t)PGW_HEADER_SIZE + 4u);

    size_t len = 7;
    ref9p_o_put_u8(g_buf, 4, PGW_P9_RREADLINK);
    ref9p_o_put_u16(g_buf, 5, CONF_TAG);
    len = ref9p_o_put_str(g_buf, len, "/tmp/9/newdir");
    ref9p_o_seal(g_buf, len);
    char target[64];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreadlink(g_buf, len, CONF_TAG, target, sizeof target),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, "/tmp/9/newdir");
}
PGW_TEST(conf_wire_treadlink_layout);
PGW_CONF_CASE(conf_wire_treadlink_layout);

/* The rule: Tlink is size[4] Tlink tag[2] dfid[4] fid[4] name[s] and Rlink is
 * size[4] Rlink tag[2]. */
static void conf_wire_tlink_layout(void)
{
    int32_t n = pgw_p9_enc_tlink(g_buf, sizeof g_buf, CONF_TAG, 6u, 7u, "hard");
    PGW_ASSERT(n > 0);
    ref9p_expect_header(g_buf, (size_t)n, PGW_P9_TLINK, CONF_TAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 7), 6u);
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_buf, 11), 7u);
    PGW_ASSERT_EQ_U(ref9p_o_u16(g_buf, 15), 4u);
    PGW_ASSERT_MEM_EQ(g_buf + 17, "hard", 4);
    PGW_ASSERT_EQ_U((size_t)n, 21u);
}
PGW_TEST(conf_wire_tlink_layout);
PGW_CONF_CASE(conf_wire_tlink_layout);
