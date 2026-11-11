/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/codec_util.h - Helpers shared by the codec test files.
 *
 * Implements requirements:
 *   - VFS-03: build replies exactly as a server writes them, so that the
 *     decoders are tested against the real wire layout
 */
#ifndef PGW_TEST_CODEC_UTIL_H
#define PGW_TEST_CODEC_UTIL_H

#include "pgw/buf.h"
#include "pgw/fusep.h"

#include <stddef.h>
#include <stdint.h>

typedef struct msg {
    uint8_t buf[1024];
    int32_t len;
} msg;

static inline void put_hdr(pgw_out *w, uint64_t unique, int32_t error)
{
    pgw_put_u32(w, 0); /* len */
    pgw_put_u32(w, (uint32_t)error);
    pgw_put_u64(w, unique);
}

static inline size_t finish(pgw_out *w)
{
    (void)pgw_patch_len_u32(w, 0);
    return pgw_written(w);
}

static inline void put_attr(pgw_out *w, uint64_t ino, uint32_t mode)
{
    pgw_put_u64(w, ino);   /* ino */
    pgw_put_u64(w, 42);    /* size */
    pgw_put_u64(w, 1);     /* blocks */
    pgw_put_u64(w, 7);     /* atime */
    pgw_put_u64(w, 8);     /* mtime */
    pgw_put_u64(w, 9);     /* ctime */
    pgw_put_u32(w, 11);    /* atimensec */
    pgw_put_u32(w, 12);    /* mtimensec */
    pgw_put_u32(w, 13);    /* ctimensec */
    pgw_put_u32(w, mode);
    pgw_put_u32(w, 2);     /* nlink */
    pgw_put_u32(w, 1000);  /* uid */
    pgw_put_u32(w, 1001);  /* gid */
    pgw_put_u32(w, 0);     /* rdev */
    pgw_put_u32(w, 4096);  /* blksize */
    pgw_put_u32(w, 0);     /* flags */
}

/* Read the opcode of an encoded request. */
static inline uint32_t req_opcode(const msg *m)
{
    pgw_buf r;
    pgw_buf_init(&r, m->buf, (size_t)m->len);
    (void)pgw_get_u32(&r); /* len */
    return pgw_get_u32(&r);
}

static inline uint32_t req_len_field(const msg *m)
{
    pgw_buf r;
    pgw_buf_init(&r, m->buf, (size_t)m->len);
    return pgw_get_u32(&r);
}

static inline uint64_t req_unique(const msg *m)
{
    pgw_buf r;
    pgw_buf_init(&r, m->buf, (size_t)m->len);
    (void)pgw_get_u32(&r);
    (void)pgw_get_u32(&r);
    return pgw_get_u64(&r);
}


#endif /* PGW_TEST_CODEC_UTIL_H */
