/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/test_codec_reply.c - Reply decoding tests: entry, attr,
 * OPEN, CREATE, READ, WRITE, READDIR, READLINK, STATFS and empty replies.
 *
 * Implements requirements:
 *   - VFS-03: every reply shape of the contract is decoded
 *   - ROB-06 / P9-09: a wrong `unique`, a truncated body, a trailing byte, an
 *     impossible readdir record or a reply longer than announced is refused
 */
#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include "codec_util.h"

#include <string.h>

/* --- entry / attr --------------------------------------------------------- */

static void test_entry_roundtrip(void)
{
    uint8_t raw[256];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 11, 0);
    pgw_put_u64(&w, 77);  /* nodeid */
    pgw_put_u64(&w, 3);   /* generation */
    pgw_put_u64(&w, 1);   /* entry_valid */
    pgw_put_u64(&w, 2);   /* attr_valid */
    pgw_put_u32(&w, 4);   /* entry_valid_nsec */
    pgw_put_u32(&w, 5);   /* attr_valid_nsec */
    put_attr(&w, 77, 0040755u);
    size_t n = finish(&w);

    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_entry(raw, n, 11, &e), PGW_OK);
    PGW_ASSERT_EQ_U(e.nodeid, 77);
    PGW_ASSERT_EQ_U(e.generation, 3);
    PGW_ASSERT_EQ_U(e.entry_valid_nsec, 4);
    PGW_ASSERT_EQ_U(e.attr.ino, 77);
    PGW_ASSERT_EQ_U(e.attr.mode, 0040755u);
    PGW_ASSERT_EQ_U(e.attr.uid, 1000);
    PGW_ASSERT_EQ_U(e.attr.blksize, 4096);
    PGW_ASSERT_EQ_I((int)pgw_ftype_from_linux_mode(e.attr.mode), PGW_FT_DIR);
}

static void test_entry_rejects_trailing_byte(void)
{
    uint8_t raw[256];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 0);
    pgw_put_u64(&w, 2);
    pgw_put_u64(&w, 0);
    pgw_put_u64(&w, 0);
    pgw_put_u64(&w, 0);
    pgw_put_u32(&w, 0);
    pgw_put_u32(&w, 0);
    put_attr(&w, 2, 0100644u);
    pgw_put_u8(&w, 0xAA); /* one byte too many */
    size_t n = finish(&w);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_entry(raw, n, 1, &e), PGW_EIO);
}

static void test_entry_truncated(void)
{
    uint8_t raw[256];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 0);
    pgw_put_u64(&w, 2);
    pgw_put_u64(&w, 0);
    size_t n = finish(&w);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_entry(raw, n, 1, &e), PGW_EIO);
}

/* --- open / create / write ------------------------------------------------ */

static void test_open_and_create(void)
{
    uint8_t raw[256];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 5, 0);
    pgw_put_u64(&w, 0xFEED);
    pgw_put_u32(&w, 0);
    pgw_put_u32(&w, 0);
    size_t n = finish(&w);
    uint64_t fh = 0;
    uint32_t oflags = 1;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_open(raw, n, 5, &fh, &oflags), PGW_OK);
    PGW_ASSERT_EQ_U(fh, 0xFEED);
    PGW_ASSERT_EQ_U(oflags, 0);

    /* OPEN reply must be exactly 16 body bytes. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 6, 0);
    pgw_put_u64(&w, 1);
    pgw_put_u32(&w, 0);
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_open(raw, n, 6, &fh, &oflags), PGW_EIO);

    /* CREATE reply: entry followed by open_out. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 7, 0);
    pgw_put_u64(&w, 9);
    pgw_put_u64(&w, 1);
    pgw_put_u64(&w, 0);
    pgw_put_u64(&w, 0);
    pgw_put_u32(&w, 0);
    pgw_put_u32(&w, 0);
    put_attr(&w, 9, 0100644u);
    pgw_put_u64(&w, 0xABCD);
    pgw_put_u32(&w, 0);
    pgw_put_u32(&w, 0);
    n = finish(&w);
    pgw_fuse_entry e;
    uint64_t cfh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_create(raw, n, 7, &e, &cfh), PGW_OK);
    PGW_ASSERT_EQ_U(e.nodeid, 9);
    PGW_ASSERT_EQ_U(cfh, 0xABCD);
}

static void test_write_and_read(void)
{
    uint8_t raw[256];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 2, 0);
    pgw_put_u32(&w, 512);
    pgw_put_u32(&w, 0);
    size_t n = finish(&w);
    uint32_t written = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_write(raw, n, 2, &written), PGW_OK);
    PGW_ASSERT_EQ_U(written, 512);

    /* READ returns a zero-copy view of the payload. */
    static const char payload[] = "hello virtiofs";
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 3, 0);
    pgw_put_data(&w, payload, sizeof(payload) - 1u);
    n = finish(&w);
    const uint8_t *data = NULL;
    size_t dlen = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_read(raw, n, 3, &data, &dlen), PGW_OK);
    PGW_ASSERT_EQ_U(dlen, sizeof(payload) - 1u);
    PGW_ASSERT_MEM_EQ(data, payload, dlen);

    /* An empty body is a legal end-of-file answer. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 4, 0);
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_read(raw, n, 4, &data, &dlen), PGW_OK);
    PGW_ASSERT_EQ_U(dlen, 0);
}

/* --- readdir -------------------------------------------------------------- */

static void put_dirent(pgw_out *w, uint64_t ino, uint64_t off, uint32_t type,
                       const char *name)
{
    size_t namelen = strlen(name);
    size_t reclen = (24u + namelen + 7u) & ~(size_t)7u;
    pgw_put_u64(w, ino);
    pgw_put_u64(w, off);
    pgw_put_u32(w, (uint32_t)namelen);
    pgw_put_u32(w, type);
    pgw_put_data(w, name, namelen);
    pgw_put_pad(w, reclen - 24u - namelen);
}

static void test_readdir_varied_sizes(void)
{
    uint8_t raw[512];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 8, 0);
    put_dirent(&w, 2, 1, PGW_DT_DIR, ".");
    put_dirent(&w, 3, 2, PGW_DT_REG, "a");
    put_dirent(&w, 4, 3, PGW_DT_LNK, "a-very-long-name-indeed.txt");
    put_dirent(&w, 5, 4, PGW_DT_REG,
               "0123456789012345678901234567890123456789"); /* forces padding */
    size_t n = finish(&w);

    pgw_fuse_dirent out[8];
    size_t count = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_readdir(raw, n, 8, out, 8, &count), PGW_OK);
    PGW_ASSERT_EQ_U(count, 4);
    PGW_ASSERT_STR_EQ(out[0].name, ".");
    PGW_ASSERT_EQ_U(out[0].namelen, 1);
    PGW_ASSERT_EQ_U(out[0].type, PGW_DT_DIR);
    PGW_ASSERT_STR_EQ(out[1].name, "a");
    PGW_ASSERT_STR_EQ(out[2].name, "a-very-long-name-indeed.txt");
    PGW_ASSERT_EQ_U(out[2].namelen, strlen("a-very-long-name-indeed.txt"));
    PGW_ASSERT_STR_EQ(out[3].name,
                      "0123456789012345678901234567890123456789");
    PGW_ASSERT_EQ_U(out[3].off, 4);
}

static void test_readdir_rejects_bad_records(void)
{
    uint8_t raw[256];
    pgw_out w;
    pgw_fuse_dirent out[4];
    size_t count = 0;

    /* namelen 0 is impossible. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 0);
    pgw_put_u64(&w, 2);
    pgw_put_u64(&w, 1);
    pgw_put_u32(&w, 0);
    pgw_put_u32(&w, PGW_DT_REG);
    pgw_put_pad(&w, 24); /* room for the record, but the name is empty */
    size_t n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_readdir(raw, n, 1, out, 4, &count), PGW_EIO);

    /* reclen not aligned: the padding of a 3-byte name is missing. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 2, 0);
    pgw_put_u64(&w, 2);
    pgw_put_u64(&w, 1);
    pgw_put_u32(&w, 3);
    pgw_put_u32(&w, PGW_DT_REG);
    pgw_put_data(&w, "abc", 3); /* no padding at all */
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_readdir(raw, n, 2, out, 4, &count), PGW_EIO);

    /* A record shorter than its header. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 3, 0);
    pgw_put_u64(&w, 2);
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_readdir(raw, n, 3, out, 4, &count), PGW_EIO);

    /* More entries than the caller can hold. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 4, 0);
    put_dirent(&w, 2, 1, PGW_DT_REG, "a");
    put_dirent(&w, 3, 2, PGW_DT_REG, "b");
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_readdir(raw, n, 4, out, 1, &count), PGW_E2BIG);
}

/* --- readlink / statfs / empty ------------------------------------------- */

static void test_readlink(void)
{
    uint8_t raw[128];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 0);
    pgw_put_data(&w, "/etc/hosts", 10);
    size_t n = finish(&w);
    char target[32];
    PGW_ASSERT_EQ_I(pgw_fuse_dec_readlink(raw, n, 1, target, sizeof(target)),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, "/etc/hosts");

    char tiny[4];
    PGW_ASSERT_EQ_I(pgw_fuse_dec_readlink(raw, n, 1, tiny, sizeof(tiny)),
                    PGW_ENAMETOOLONG);
}

static void test_statfs(void)
{
    uint8_t raw[128];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 0);
    pgw_put_u64(&w, 100);
    pgw_put_u64(&w, 50);
    pgw_put_u64(&w, 40);
    pgw_put_u64(&w, 10);
    pgw_put_u64(&w, 5);
    pgw_put_u32(&w, 4096);
    pgw_put_u32(&w, 255);
    pgw_put_u32(&w, 4096);
    pgw_put_u32(&w, 0);
    pgw_put_pad(&w, 24);
    size_t n = finish(&w);
    pgw_fuse_kstatfs st;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_statfs(raw, n, 1, &st), PGW_OK);
    PGW_ASSERT_EQ_U(st.blocks, 100);
    PGW_ASSERT_EQ_U(st.ffree, 5);
    PGW_ASSERT_EQ_U(st.bsize, 4096);
    PGW_ASSERT_EQ_U(st.namelen, 255);
    PGW_ASSERT_EQ_U(st.frsize, 4096);
}

static void test_empty_rejects_body(void)
{
    uint8_t raw[64];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 0);
    size_t n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_empty(raw, n, 1), PGW_OK);

    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 2, 0);
    pgw_put_u8(&w, 0);
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_empty(raw, n, 2), PGW_EIO);
}

PGW_TEST(test_entry_roundtrip);
PGW_TEST(test_entry_rejects_trailing_byte);
PGW_TEST(test_entry_truncated);
PGW_TEST(test_open_and_create);
PGW_TEST(test_write_and_read);
PGW_TEST(test_readdir_varied_sizes);
PGW_TEST(test_readdir_rejects_bad_records);
PGW_TEST(test_readlink);
PGW_TEST(test_statfs);
PGW_TEST(test_empty_rejects_body);
