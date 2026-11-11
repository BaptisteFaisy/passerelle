/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_validation.c - the hostile-server bench (P9-09, ROB-06).
 *
 * Every decoder is fed:
 *   - the message truncated at each of its possible lengths (no crash, a
 *     clean pgw_err every time);
 *   - the message with one byte appended (trailing garbage is refused);
 *   - the message with the wrong tag and the wrong type;
 *   - messages whose internal lengths lie (a string longer than the message,
 *     nwqid larger than the qids present, a readdir count that does not match
 *     the records, a record whose name overruns the payload).
 *
 * The point is not that one of these fails, but that none of them ever makes
 * the library read outside the buffer or return a half-filled structure.
 */
#include "pgw/buf.h"
#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <string.h>

#define TAG 5u

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

static void put_qid(pgw_out *w, uint8_t type, uint64_t path)
{
    pgw_put_u8(w, type);
    pgw_put_u32(w, 0);
    pgw_put_u64(w, path);
}

/* Decode whatever `type` says, with the tag the builders use. Split in two so
 * that each helper stays short; `decode` is the single entry point. */
static pgw_err decode_meta(uint8_t type, const void *msg, size_t len,
                           uint16_t tag)
{
    switch (type) {
    case PGW_P9_RLERROR: {
        uint32_t e = 0;
        return pgw_p9_dec_rlerror(msg, len, tag, &e);
    }
    case PGW_P9_RVERSION: {
        uint32_t m = 0;
        char v[16];
        return pgw_p9_dec_rversion(msg, len, tag, &m, v, sizeof v);
    }
    case PGW_P9_RATTACH: {
        pgw_p9_qid q;
        return pgw_p9_dec_rattach(msg, len, tag, &q);
    }
    case PGW_P9_RWALK: {
        uint16_t n = 0;
        pgw_p9_qid q[PGW_MAXWELEM];
        return pgw_p9_dec_rwalk(msg, len, tag, &n, q, (uint16_t)PGW_MAXWELEM);
    }
    default:
        return PGW_EOPNOTSUPP; /* not this family */
    }
}

static pgw_err decode_data(uint8_t type, const void *msg, size_t len,
                           uint16_t tag)
{
    switch (type) {
    case PGW_P9_RLOPEN: {
        pgw_p9_qid q;
        uint32_t u = 0;
        return pgw_p9_dec_rlopen(msg, len, tag, &q, &u);
    }
    case PGW_P9_RREAD: {
        uint32_t c = 0;
        const uint8_t *d = NULL;
        return pgw_p9_dec_rread(msg, len, tag, &c, &d);
    }
    case PGW_P9_RWRITE: {
        uint32_t c = 0;
        return pgw_p9_dec_rwrite(msg, len, tag, &c);
    }
    case PGW_P9_RGETATTR: {
        pgw_p9_attr a;
        return pgw_p9_dec_rgetattr(msg, len, tag, &a);
    }
    case PGW_P9_RREADDIR: {
        pgw_p9_dirent e[8];
        size_t c = 0;
        return pgw_p9_dec_rreaddir_iter(msg, len, tag, e, 8, &c);
    }
    case PGW_P9_RREADLINK: {
        char t[64];
        return pgw_p9_dec_rreadlink(msg, len, tag, t, sizeof t);
    }
    case PGW_P9_RMKDIR: {
        pgw_p9_qid q;
        return pgw_p9_dec_rmkdir(msg, len, tag, &q);
    }
    case PGW_P9_RSTATFS: {
        pgw_p9_statfs_t st;
        return pgw_p9_dec_rstatfs(msg, len, tag, &st);
    }
    default:
        return PGW_EOPNOTSUPP;
    }
}

static pgw_err decode(uint8_t type, const void *msg, size_t len)
{
    uint16_t tag = (uint16_t)TAG;
    pgw_err e = decode_meta(type, msg, len, tag);
    if (e != PGW_EOPNOTSUPP)
        return e;
    e = decode_data(type, msg, len, tag);
    if (e != PGW_EOPNOTSUPP)
        return e;
    return pgw_p9_dec_empty(msg, len, type, tag);
}

/* --- one well-formed message of each reply type --------------------------- */

static size_t build_rlerror(void)
{
    pgw_out w = begin(PGW_P9_RLERROR, TAG);
    pgw_put_u32(&w, 2);
    return finish(&w);
}

static size_t build_rversion(void)
{
    pgw_out w = begin(PGW_P9_RVERSION, TAG);
    pgw_put_u32(&w, 8192);
    PGW_ASSERT(pgw_put_str(&w, "9P2000.L", 8));
    return finish(&w);
}

static size_t build_rattach(void)
{
    pgw_out w = begin(PGW_P9_RATTACH, TAG);
    put_qid(&w, PGW_P9_QTDIR, 1);
    return finish(&w);
}

static size_t build_rwalk(void)
{
    pgw_out w = begin(PGW_P9_RWALK, TAG);
    pgw_put_u16(&w, 1);
    put_qid(&w, PGW_P9_QTFILE, 2);
    return finish(&w);
}

static size_t build_rlopen(void)
{
    pgw_out w = begin(PGW_P9_RLOPEN, TAG);
    put_qid(&w, PGW_P9_QTFILE, 3);
    pgw_put_u32(&w, 0);
    return finish(&w);
}

static size_t build_rread(void)
{
    pgw_out w = begin(PGW_P9_RREAD, TAG);
    pgw_put_u32(&w, 4);
    PGW_ASSERT_EQ_U(pgw_put_data(&w, "data", 4), 4u);
    return finish(&w);
}

static size_t build_rwrite(void)
{
    pgw_out w = begin(PGW_P9_RWRITE, TAG);
    pgw_put_u32(&w, 4);
    return finish(&w);
}

static size_t build_rgetattr(void)
{
    pgw_out w = begin(PGW_P9_RGETATTR, TAG);
    pgw_put_u64(&w, PGW_P9_GETATTR_BASIC);
    put_qid(&w, PGW_P9_QTDIR, 4);
    for (int i = 0; i < 3; i++)
        pgw_put_u32(&w, 0); /* mode, uid, gid */
    for (int i = 0; i < 5; i++)
        pgw_put_u64(&w, 0); /* nlink, rdev, size, blksize, blocks */
    for (int i = 0; i < 10; i++)
        pgw_put_u64(&w, 0); /* atime..ctime, btime, gen, data_version */
    return finish(&w);
}

static size_t build_rreaddir(void)
{
    pgw_out w = begin(PGW_P9_RREADDIR, TAG);
    size_t count_off = pgw_written(&w);
    pgw_put_u32(&w, 0);
    size_t payload_off = pgw_written(&w);
    put_qid(&w, PGW_P9_QTFILE, 5);
    pgw_put_u64(&w, 1);
    pgw_put_u8(&w, (uint8_t)PGW_DT_REG);
    PGW_ASSERT(pgw_put_str(&w, "abc", 3));
    size_t payload = pgw_written(&w) - payload_off;
    uint8_t *p = pgw_out_at(&w, count_off);
    PGW_ASSERT(p != NULL);
    p[0] = (uint8_t)payload;
    p[1] = (uint8_t)(payload >> 8);
    p[2] = (uint8_t)(payload >> 16);
    p[3] = (uint8_t)(payload >> 24);
    return finish(&w);
}

static size_t build_rreadlink(void)
{
    pgw_out w = begin(PGW_P9_RREADLINK, TAG);
    PGW_ASSERT(pgw_put_str(&w, "tgt", 3));
    return finish(&w);
}

static size_t build_rmkdir(void)
{
    pgw_out w = begin(PGW_P9_RMKDIR, TAG);
    put_qid(&w, PGW_P9_QTDIR, 6);
    return finish(&w);
}

static size_t build_rstatfs(void)
{
    pgw_out w = begin(PGW_P9_RSTATFS, TAG);
    pgw_put_u32(&w, 1);
    pgw_put_u32(&w, 2);
    for (int i = 0; i < 6; i++)
        pgw_put_u64(&w, 0);
    pgw_put_u32(&w, 255);
    return finish(&w);
}

static size_t build_rclunk(void)
{
    pgw_out w = begin(PGW_P9_RCLUNK, TAG);
    return finish(&w);
}

typedef struct case_def {
    uint8_t type;
    size_t (*build)(void);
} case_def;

static const case_def g_cases[] = {
    { PGW_P9_RLERROR, build_rlerror },
    { PGW_P9_RVERSION, build_rversion },
    { PGW_P9_RATTACH, build_rattach },
    { PGW_P9_RWALK, build_rwalk },
    { PGW_P9_RLOPEN, build_rlopen },
    { PGW_P9_RREAD, build_rread },
    { PGW_P9_RWRITE, build_rwrite },
    { PGW_P9_RGETATTR, build_rgetattr },
    { PGW_P9_RREADDIR, build_rreaddir },
    { PGW_P9_RREADLINK, build_rreadlink },
    { PGW_P9_RMKDIR, build_rmkdir },
    { PGW_P9_RSTATFS, build_rstatfs },
    { PGW_P9_RCLUNK, build_rclunk },
};

static void test_truncation_at_every_length(void)
{
    size_t nc = sizeof g_cases / sizeof g_cases[0];
    for (size_t c = 0; c < nc; c++) {
        size_t len = g_cases[c].build();
        PGW_ASSERT_EQ_I(decode(g_cases[c].type, g_buf, len), PGW_OK);
        for (size_t cut = 0; cut < len; cut++) {
            pgw_err e = decode(g_cases[c].type, g_buf, cut);
            if (e == PGW_OK)
                pgw_fail(__FILE__, __LINE__,
                         "type %u accepted a %zu/%zu byte truncation",
                         (unsigned)g_cases[c].type, cut, len);
        }
    }
}
PGW_TEST(test_truncation_at_every_length);

/* One byte appended *inside* the declared message: size[4] is raised by one so
 * that the extra byte is part of the message the server claims to have sent.
 * That is the case P9-09 forbids: the last field must end exactly at size[4].
 * (A transport may hand over more bytes than size[4] - a virtio used-ring
 * length, say - and those are outside the message; the decoders ignore them.) */
static void test_trailing_bytes_refused(void)
{
    size_t nc = sizeof g_cases / sizeof g_cases[0];
    for (size_t c = 0; c < nc; c++) {
        size_t len = g_cases[c].build();
        g_buf[len] = 0xAA;
        size_t bigger = len + 1;
        g_buf[0] = (uint8_t)bigger;
        g_buf[1] = (uint8_t)(bigger >> 8);
        g_buf[2] = (uint8_t)(bigger >> 16);
        g_buf[3] = (uint8_t)(bigger >> 24);
        pgw_err e = decode(g_cases[c].type, g_buf, bigger);
        if (e == PGW_OK)
            pgw_fail(__FILE__, __LINE__, "type %u accepted a trailing byte",
                     (unsigned)g_cases[c].type);
    }
}
PGW_TEST(test_trailing_bytes_refused);

static void test_wrong_tag_and_type_refused(void)
{
    size_t len = build_rread();
    uint32_t count = 0;
    const uint8_t *data = NULL;
    /* Right message, wrong tag. */
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, len, TAG + 1u, &count, &data),
                    PGW_EINVAL);
    /* Right message, wrong decoder. */
    pgw_p9_qid qid;
    uint32_t iounit = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rlopen(g_buf, len, TAG, &qid, &iounit),
                    PGW_EINVAL);
    /* An Rlerror is never accepted where a data reply is expected. */
    len = build_rlerror();
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, len, TAG, &count, &data),
                    PGW_EINVAL);
}
PGW_TEST(test_wrong_tag_and_type_refused);

static void test_lying_string_length(void)
{
    /* Rreadlink whose string claims 100 bytes but carries 3. */
    pgw_out w = begin(PGW_P9_RREADLINK, TAG);
    pgw_put_u16(&w, 100);
    PGW_ASSERT_EQ_U(pgw_put_data(&w, "abc", 3), 3u);
    size_t len = finish(&w);
    char target[128];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreadlink(g_buf, len, TAG, target, sizeof target),
                    PGW_EMSGSIZE);

    /* A name longer than the 255 bytes of pgw_p9_dirent.name is refused
     * instead of being silently cut. */
    w = begin(PGW_P9_RREADDIR, TAG);
    size_t count_off = pgw_written(&w);
    pgw_put_u32(&w, 0);
    size_t payload_off = pgw_written(&w);
    put_qid(&w, PGW_P9_QTFILE, 7);
    pgw_put_u64(&w, 1);
    pgw_put_u8(&w, (uint8_t)PGW_DT_REG);
    char longname[300];
    memset(longname, 'n', sizeof longname);
    PGW_ASSERT(pgw_put_str(&w, longname, sizeof longname));
    size_t payload = pgw_written(&w) - payload_off;
    uint8_t *p = pgw_out_at(&w, count_off);
    PGW_ASSERT(p != NULL);
    p[0] = (uint8_t)payload;
    p[1] = (uint8_t)(payload >> 8);
    p[2] = (uint8_t)(payload >> 16);
    p[3] = (uint8_t)(payload >> 24);
    len = finish(&w);
    pgw_p9_dirent ents[2];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, len, TAG, ents, 2, &got),
                    PGW_EINVAL);
}
PGW_TEST(test_lying_string_length);

static void test_lying_internal_counts(void)
{
    /* Rwalk announces 3 qids but carries 1. */
    pgw_out w = begin(PGW_P9_RWALK, TAG);
    pgw_put_u16(&w, 3);
    put_qid(&w, PGW_P9_QTFILE, 8);
    size_t len = finish(&w);
    uint16_t nwqid = 0;
    pgw_p9_qid qids[PGW_MAXWELEM];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rwalk(g_buf, len, TAG, &nwqid, qids,
                                     (uint16_t)PGW_MAXWELEM),
                    PGW_EINVAL);

    /* Rwalk announcing more qids than the caller can hold. */
    w = begin(PGW_P9_RWALK, TAG);
    pgw_put_u16(&w, 2);
    put_qid(&w, PGW_P9_QTFILE, 9);
    put_qid(&w, PGW_P9_QTFILE, 10);
    len = finish(&w);
    PGW_ASSERT_EQ_I(pgw_p9_dec_rwalk(g_buf, len, TAG, &nwqid, qids, 1),
                    PGW_EINVAL);
}
PGW_TEST(test_lying_internal_counts);

static void test_lying_payload_counts(void)
{
    /* Rread whose count is larger than the payload. */
    pgw_out w = begin(PGW_P9_RREAD, TAG);
    pgw_put_u32(&w, 100);
    PGW_ASSERT_EQ_U(pgw_put_data(&w, "abc", 3), 3u);
    size_t len = finish(&w);
    uint32_t count = 0;
    const uint8_t *data = NULL;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, len, TAG, &count, &data),
                    PGW_EMSGSIZE);

    /* Rreaddir whose count field is larger than the records it carries. */
    w = begin(PGW_P9_RREADDIR, TAG);
    pgw_put_u32(&w, 40);
    put_qid(&w, PGW_P9_QTFILE, 11);
    len = finish(&w);
    pgw_p9_dirent ents[2];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, len, TAG, ents, 2, &got),
                    PGW_EMSGSIZE);

    /* Rreaddir whose count field is smaller than the records: the bytes past
     * the count are trailing garbage and must be refused. */
    uint8_t *p;
    w = begin(PGW_P9_RREADDIR, TAG);
    size_t count_off = pgw_written(&w);
    pgw_put_u32(&w, 0);
    put_qid(&w, PGW_P9_QTFILE, 12);
    pgw_put_u64(&w, 1);
    pgw_put_u8(&w, (uint8_t)PGW_DT_REG);
    PGW_ASSERT(pgw_put_str(&w, "abc", 3));
    p = pgw_out_at(&w, count_off);
    PGW_ASSERT(p != NULL);
    p[0] = 4; /* one short: the rest becomes trailing bytes */
    len = finish(&w);
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, len, TAG, ents, 2, &got),
                    PGW_EINVAL);
}
PGW_TEST(test_lying_payload_counts);

static void test_null_arguments(void)
{
    uint32_t count = 0;
    const uint8_t *data = NULL;
    size_t len = build_rread();
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(NULL, len, TAG, &count, &data), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, len, TAG, NULL, &data), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_dec_rread(g_buf, len, TAG, &count, NULL), PGW_EINVAL);
    char t[8];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreadlink(g_buf, 0, TAG, t, 0), PGW_EINVAL);
    pgw_p9_dirent e[2];
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_buf, 0, TAG, e, 2, NULL),
                    PGW_EINVAL);
}
PGW_TEST(test_null_arguments);
