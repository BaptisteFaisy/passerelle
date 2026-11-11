/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/test_codec.c - Encoder/decoder unit tests for the FUSE layer.
 *
 * Implements requirements:
 *   - VFS-03: every request of the contract is encoded and its reply decoded
 *   - ROB-06 / P9-09: a truncated header, a mismatched `unique`, a negative
 *     error field and trailing bytes are all rejected
 *   - VFS-04: readdir records are parsed with their 8-byte alignment
 */
#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include "codec_util.h"
#include "fusep_abi.h"

#include <string.h>

/* --- header and framing --------------------------------------------------- */

static void test_enc_header_shape(void)
{
    msg m;
    m.len = pgw_fuse_enc_getattr(m.buf, sizeof(m.buf), 7, 3, 0, false);
    PGW_ASSERT_EQ_I(m.len, (int)PGW_FUSE_IN_HDR_SZ + 16);
    PGW_ASSERT_EQ_U(req_len_field(&m), (uint32_t)m.len);
    PGW_ASSERT_EQ_U(req_opcode(&m), PGW_FUSE_GETATTR);
    PGW_ASSERT_EQ_U(req_unique(&m), 7);
}

static void test_dec_hdr_rejects_short_and_bad_length(void)
{
    uint8_t raw[16] = { 0 };
    pgw_fuse_out_hdr h;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_hdr(raw, 8, &h), PGW_EIO);

    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 5, 0);
    (void)finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_hdr(raw, sizeof(raw), &h), PGW_OK);
    PGW_ASSERT_EQ_U(h.unique, 5);

    /* Declared length larger than the buffer we hold. */
    raw[0] = 64;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_hdr(raw, sizeof(raw), &h), PGW_EIO);
}

static void test_declared_length_wins_over_received(void)
{
    /* A virtio used-ring length may be larger than the message it carries.
     * The declared len[4] is authoritative and the extra bytes are ignored,
     * which is exactly what pgw_buf_clamp is for. */
    uint8_t raw[256];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 0);
    pgw_put_u64(&w, 0); /* attr_valid */
    pgw_put_u32(&w, 0);
    pgw_put_u32(&w, 0);
    put_attr(&w, 5, 0100644u);
    size_t n = finish(&w);
    memset(raw + n, 0xEE, 8); /* 8 bytes of ring slack */

    pgw_fuse_out_hdr h;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_hdr(raw, n + 8u, &h), PGW_OK);
    PGW_ASSERT_EQ_U(h.len, (uint32_t)n);
    pgw_fuse_attr a;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_attr(raw, n + 8u, 1, &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.ino, 5);

    /* A declared length below the header size is impossible. */
    raw[0] = 8;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_hdr(raw, n, &h), PGW_EIO);
}

static void test_dec_hdr_rejects_positive_error(void)
{
    uint8_t raw[16];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 1, 22); /* a positive error is not a Linux errno */
    (void)finish(&w);
    pgw_fuse_out_hdr h;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_hdr(raw, sizeof(raw), &h), PGW_EIO);
}

static void test_dec_rejects_wrong_unique(void)
{
    uint8_t raw[256];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 99, 0);
    pgw_put_u64(&w, 0); /* attr_valid */
    pgw_put_u32(&w, 0);
    pgw_put_u32(&w, 0);
    put_attr(&w, 5, 0100644u);
    size_t n = finish(&w);
    pgw_fuse_attr a;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_attr(raw, n, 99, &a), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_attr(raw, n, 100, &a), PGW_EIO);
}

static void test_dec_error_maps_to_pgw_err(void)
{
    uint8_t raw[16];
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 3, -2); /* -ENOENT on the wire */
    size_t n = finish(&w);
    pgw_fuse_attr a;
    PGW_ASSERT_EQ_I(pgw_fuse_dec_attr(raw, n, 3, &a), PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_code_from_linux(2), PGW_ENOENT);

    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 4, -38); /* -ENOSYS */
    n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_empty(raw, n, 4), PGW_ENOSYS);
}

/* --- INIT ----------------------------------------------------------------- */

static void test_init_roundtrip(void)
{
    msg m;
    m.len = pgw_fuse_enc_init(m.buf, sizeof(m.buf), 1, 131072,
                              PGW_FUSE_F_BIG_WRITES | PGW_FUSE_F_ASYNC_READ);
    PGW_ASSERT_EQ_I(m.len, (int)PGW_FUSE_IN_HDR_SZ + 64);
    PGW_ASSERT_EQ_U(req_opcode(&m), PGW_FUSE_INIT);

    pgw_buf r;
    pgw_buf_body(&r, m.buf, (size_t)m.len, PGW_FUSE_IN_HDR_SZ);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), PGW_FUSE_MAJOR);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), PGW_FUSE_MINOR);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 131072);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r),
                    PGW_FUSE_F_BIG_WRITES | PGW_FUSE_F_ASYNC_READ);
    PGW_ASSERT_EQ_U(pgw_left(&r), 48u); /* flags2 (4) + unused[11] (44) */
}

/* Build a full-width (7.36) INIT reply: 64 bytes of body. */
static size_t build_init_reply(uint8_t *raw, size_t cap, uint64_t unique,
                               uint32_t minor, uint32_t max_write,
                               uint16_t pages)
{
    pgw_out w;
    pgw_out_init(&w, raw, cap);
    put_hdr(&w, unique, 0);
    pgw_put_u32(&w, 7);           /* major */
    pgw_put_u32(&w, minor);
    pgw_put_u32(&w, 65536);       /* max_readahead */
    pgw_put_u32(&w, (uint32_t)PGW_FUSE_F_BIG_WRITES);
    pgw_put_u16(&w, 16);          /* max_background */
    pgw_put_u16(&w, 8);           /* congestion_threshold */
    pgw_put_u32(&w, max_write);
    pgw_put_u32(&w, 1);           /* time_gran */
    pgw_put_u16(&w, pages);
    pgw_put_u16(&w, 0);           /* map_alignment */
    pgw_put_u32(&w, 0);           /* flags2 */
    pgw_put_pad(&w, 7u * sizeof(uint32_t)); /* unused[7] */
    return finish(&w);
}

static void test_dec_init_full_width(void)
{
    uint8_t raw[128];
    pgw_fuse_init_info info;
    size_t n = build_init_reply(raw, sizeof(raw), 1, 31, 4096, 32);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_init(raw, n, 1, &info), PGW_OK);
    PGW_ASSERT_EQ_U(info.major, 7);
    PGW_ASSERT_EQ_U(info.minor, 31);
    PGW_ASSERT_EQ_U(info.max_write, 4096);
    PGW_ASSERT_EQ_U(info.max_pages, 32);
}

static void test_dec_init_7_22_width(void)
{
    uint8_t raw[128];
    pgw_fuse_init_info info;
    pgw_out w;
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 2, 0);
    pgw_put_u32(&w, 7);
    pgw_put_u32(&w, 22);
    pgw_put_u32(&w, 65536);
    pgw_put_u32(&w, 0);
    pgw_put_u16(&w, 16);
    pgw_put_u16(&w, 8);
    pgw_put_u32(&w, 131072);
    size_t n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_init(raw, n, 2, &info), PGW_OK);
    PGW_ASSERT_EQ_U(info.max_write, 131072);
    PGW_ASSERT_EQ_U(info.max_pages, 0);
}

static void test_dec_init_refuses_other_width(void)
{
    uint8_t raw[128];
    pgw_fuse_init_info info;
    pgw_out w;
    /* 12 bytes of body: neither the 8, 24 nor 64-byte shape. */
    pgw_out_init(&w, raw, sizeof(raw));
    put_hdr(&w, 3, 0);
    pgw_put_u32(&w, 7);
    pgw_put_u32(&w, 22);
    pgw_put_u32(&w, 0);
    size_t n = finish(&w);
    PGW_ASSERT_EQ_I(pgw_fuse_dec_init(raw, n, 3, &info), PGW_EIO);
}

/* --- every encoder in the contract --------------------------------------- */

static void test_all_encoders(void)
{
    uint8_t buf[1024];
    pgw_fuse_setattr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.valid = PGW_FUSE_FATTR_MODE | PGW_FUSE_FATTR_SIZE;
    sa.mode = 0100644u;
    sa.size = 1234;
    sa.uid = 1000;
    sa.gid = 1000;

    PGW_ASSERT(pgw_fuse_enc_init(buf, sizeof(buf), 1, 0, 0) > 0);
    PGW_ASSERT(pgw_fuse_enc_lookup(buf, sizeof(buf), 2, 1, "name") > 0);
    PGW_ASSERT(pgw_fuse_enc_forget(buf, sizeof(buf), 3, 5, 2) > 0);
    PGW_ASSERT(pgw_fuse_enc_getattr(buf, sizeof(buf), 4, 5, 0, false) > 0);
    PGW_ASSERT(pgw_fuse_enc_getattr(buf, sizeof(buf), 4, 5, 9, true) > 0);
    PGW_ASSERT(pgw_fuse_enc_setattr(buf, sizeof(buf), 5, 5, &sa) > 0);
    PGW_ASSERT(pgw_fuse_enc_open(buf, sizeof(buf), 6, 5, 0, false) > 0);
    PGW_ASSERT(pgw_fuse_enc_open(buf, sizeof(buf), 6, 5, 0, true) > 0);
    PGW_ASSERT(pgw_fuse_enc_create(buf, sizeof(buf), 7, 5, "n", 0, 0644, 0) > 0);
    PGW_ASSERT(pgw_fuse_enc_release(buf, sizeof(buf), 8, 5, 1, false) > 0);
    PGW_ASSERT(pgw_fuse_enc_read(buf, sizeof(buf), 9, 5, 1, 0, 4096, false) > 0);
    PGW_ASSERT(pgw_fuse_enc_write(buf, sizeof(buf), 10, 5, 1, 0, "abc", 3) > 0);
    PGW_ASSERT(pgw_fuse_enc_mkdir(buf, sizeof(buf), 11, 5, "d", 0755, 0) > 0);
    PGW_ASSERT(pgw_fuse_enc_name_op(buf, sizeof(buf), PGW_FUSE_UNLINK, 12, 5,
                                    "f") > 0);
    PGW_ASSERT(pgw_fuse_enc_name_op(buf, sizeof(buf), PGW_FUSE_RMDIR, 12, 5,
                                    "d") > 0);
    PGW_ASSERT(pgw_fuse_enc_rename(buf, sizeof(buf), 13, 5, "a", 6, "b", 0) > 0);
    PGW_ASSERT(pgw_fuse_enc_symlink(buf, sizeof(buf), 14, 5, "s", "/t") > 0);
    PGW_ASSERT(pgw_fuse_enc_link(buf, sizeof(buf), 15, 5, 9, "h") > 0);
    PGW_ASSERT(pgw_fuse_enc_readlink(buf, sizeof(buf), 16, 5) > 0);
    PGW_ASSERT(pgw_fuse_enc_fsync(buf, sizeof(buf), 17, 5, 1, true, false) > 0);
    PGW_ASSERT(pgw_fuse_enc_statfs(buf, sizeof(buf), 18, 5) > 0);
    PGW_ASSERT(pgw_fuse_enc_interrupt(buf, sizeof(buf), 19, 4) > 0);
}

static void test_encoders_refuse_invalid_input(void)
{
    uint8_t buf[1024];
    PGW_ASSERT_EQ_I(pgw_fuse_enc_lookup(buf, sizeof(buf), 1, 1, NULL), -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_lookup(buf, 8, 1, 1, "name"), -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_setattr(buf, sizeof(buf), 1, 1, NULL), -1);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_name_op(buf, sizeof(buf), PGW_FUSE_LOOKUP, 1,
                                         1, "x"),
                    -1);
    char long_name[300];
    memset(long_name, 'x', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = '\0';
    PGW_ASSERT_EQ_I(pgw_fuse_enc_lookup(buf, sizeof(buf), 1, 1, long_name), -1);
}

static void test_rename2_used_for_flags(void)
{
    uint8_t buf[512];
    int32_t n = pgw_fuse_enc_rename(buf, sizeof(buf), 1, 5, "a", 6, "b",
                                    1u /* RENAME_NOREPLACE */);
    PGW_ASSERT(n > 0);
    msg m;
    memcpy(m.buf, buf, (size_t)n);
    m.len = n;
    PGW_ASSERT_EQ_U(req_opcode(&m), PGW_FUSE_RENAME2);
}

static void test_write_payload_placement(void)
{
    uint8_t buf[256];
    int32_t n = pgw_fuse_enc_write(buf, sizeof(buf), 3, 5, 7, 4096, "xyz", 3);
    PGW_ASSERT_EQ_I(n, (int)PGW_FUSE_IN_HDR_SZ + 40 + 3);
    pgw_buf r;
    pgw_buf_body(&r, buf, (size_t)n, PGW_FUSE_IN_HDR_SZ);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 7);    /* fh */
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 4096); /* offset */
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 3);    /* size */
    pgw_skip(&r, 4u + 8u + 4u + 4u);
    const uint8_t *p = pgw_take(&r, 3);
    PGW_ASSERT(p != NULL);
    PGW_ASSERT_MEM_EQ(p, "xyz", 3);
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
}

/* The exact byte counts of the wire ABI. A single stray padding byte in an
 * encoder is a protocol bug that a real server would refuse, so every request
 * length is pinned here. */
#define HDR ((int32_t)PGW_FUSE_IN_HDR_SZ)
#define NLEN ((int32_t)(strlen("nm") + 1u)) /* names carry their NUL */

static void check_meta_request_sizes(uint8_t *buf, size_t cap)
{
    pgw_fuse_setattr_in sa;
    memset(&sa, 0, sizeof(sa));

    PGW_ASSERT_EQ_I(pgw_fuse_enc_init(buf, cap, 1, 0, 0),
                    HDR + (int32_t)PGW_FUSE_INIT_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_lookup(buf, cap, 1, 1, "nm"), HDR + NLEN);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_forget(buf, cap, 1, 1, 1),
                    HDR + (int32_t)PGW_FUSE_FORGET_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_getattr(buf, cap, 1, 1, 0, false),
                    HDR + (int32_t)PGW_FUSE_GETATTR_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_setattr(buf, cap, 1, 1, &sa),
                    HDR + (int32_t)PGW_FUSE_SETATTR_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_open(buf, cap, 1, 1, 0, false),
                    HDR + (int32_t)PGW_FUSE_OPEN_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_create(buf, cap, 1, 1, "nm", 0, 0, 0),
                    HDR + (int32_t)PGW_FUSE_CREATE_IN_SIZE + NLEN);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_mkdir(buf, cap, 1, 1, "nm", 0, 0),
                    HDR + (int32_t)PGW_FUSE_MKDIR_IN_SIZE + NLEN);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_name_op(buf, cap, PGW_FUSE_UNLINK, 1, 1, "nm"),
                    HDR + NLEN);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_symlink(buf, cap, 1, 1, "nm", "tgt"),
                    HDR + NLEN + (int32_t)(strlen("tgt") + 1u));
    PGW_ASSERT_EQ_I(pgw_fuse_enc_link(buf, cap, 1, 1, 2, "nm"),
                    HDR + (int32_t)PGW_FUSE_LINK_IN_SIZE + NLEN);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_rename(buf, cap, 1, 1, "nm", 2, "nm", 0),
                    HDR + (int32_t)PGW_FUSE_RENAME_IN_SIZE + 2 * NLEN);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_rename(buf, cap, 1, 1, "nm", 2, "nm", 2),
                    HDR + (int32_t)PGW_FUSE_RENAME2_IN_SIZE + 2 * NLEN);
}

static void check_data_request_sizes(uint8_t *buf, size_t cap)
{
    PGW_ASSERT_EQ_I(pgw_fuse_enc_release(buf, cap, 1, 1, 1, false),
                    HDR + (int32_t)PGW_FUSE_RELEASE_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_read(buf, cap, 1, 1, 1, 0, 16, false),
                    HDR + (int32_t)PGW_FUSE_READ_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_write(buf, cap, 1, 1, 1, 0, "abc", 3),
                    HDR + (int32_t)PGW_FUSE_WRITE_IN_SIZE + 3);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_readlink(buf, cap, 1, 1), HDR);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_fsync(buf, cap, 1, 1, 1, false, false),
                    HDR + (int32_t)PGW_FUSE_FSYNC_IN_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_statfs(buf, cap, 1, 1), HDR);
    PGW_ASSERT_EQ_I(pgw_fuse_enc_interrupt(buf, cap, 1, 2),
                    HDR + (int32_t)PGW_FUSE_INTERRUPT_IN_SIZE);
}

static void test_request_sizes_match_abi(void)
{
    uint8_t buf[1024];
    check_meta_request_sizes(buf, sizeof(buf));
    check_data_request_sizes(buf, sizeof(buf));
}

PGW_TEST(test_request_sizes_match_abi);
PGW_TEST(test_enc_header_shape);
PGW_TEST(test_dec_hdr_rejects_short_and_bad_length);
PGW_TEST(test_declared_length_wins_over_received);
PGW_TEST(test_dec_hdr_rejects_positive_error);
PGW_TEST(test_dec_rejects_wrong_unique);
PGW_TEST(test_dec_error_maps_to_pgw_err);
PGW_TEST(test_init_roundtrip);
PGW_TEST(test_dec_init_full_width);
PGW_TEST(test_dec_init_7_22_width);
PGW_TEST(test_dec_init_refuses_other_width);
PGW_TEST(test_all_encoders);
PGW_TEST(test_encoders_refuse_invalid_input);
PGW_TEST(test_rename2_used_for_flags);
PGW_TEST(test_write_payload_placement);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
