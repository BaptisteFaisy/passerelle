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
#include <string.h>

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

static inline void put_attr_from(pgw_out *w, const pgw_fuse_attr *a,
                                 uint32_t flags)
{
    pgw_put_u64(w, a->ino);
    pgw_put_u64(w, a->size);
    pgw_put_u64(w, a->blocks);
    pgw_put_u64(w, a->atime);
    pgw_put_u64(w, a->mtime);
    pgw_put_u64(w, a->ctime);
    pgw_put_u32(w, a->atimensec);
    pgw_put_u32(w, a->mtimensec);
    pgw_put_u32(w, a->ctimensec);
    pgw_put_u32(w, a->mode);
    pgw_put_u32(w, a->nlink);
    pgw_put_u32(w, a->uid);
    pgw_put_u32(w, a->gid);
    pgw_put_u32(w, a->rdev);
    pgw_put_u32(w, a->blksize);
    pgw_put_u32(w, flags); /* the 7.32 attr-flags word */
}

static inline void put_attr(pgw_out *w, uint64_t ino, uint32_t mode)
{
    pgw_fuse_attr a;
    memset(&a, 0, sizeof(a));
    a.ino = ino;
    a.size = 42;
    a.blocks = 1;
    a.atime = 7;
    a.mtime = 8;
    a.ctime = 9;
    a.atimensec = 11;
    a.mtimensec = 12;
    a.ctimensec = 13;
    a.mode = mode;
    a.nlink = 2;
    a.uid = 1000;
    a.gid = 1001;
    a.rdev = 0;
    a.blksize = 4096;
    put_attr_from(w, &a, 0u);
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
