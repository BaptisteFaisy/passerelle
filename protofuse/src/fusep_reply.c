/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_reply.c - Completion callbacks for the reply shapes.
 *
 * Implements requirements:
 *   - VFS-03: decoding of entry, attribute, open, create, read, write,
 *     readlink, statfs and empty replies
 *   - ROB-06: all of them go through the strict decoders, so a mismatched
 *     `unique`, a truncated body or a trailing byte is reported as an error
 */
#include "fusep_reply.h"

#include "pgw/fusep.h"

pgw_err pgw_fusep_done_entry(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep1 *c = (pgw_fusep_rep1 *)ctx;
    return pgw_fuse_dec_entry(rx, len, c->unique, (pgw_fuse_entry *)c->a);
}

pgw_err pgw_fusep_done_attr(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep1 *c = (pgw_fusep_rep1 *)ctx;
    return pgw_fuse_dec_attr(rx, len, c->unique, (pgw_fuse_attr *)c->a);
}

pgw_err pgw_fusep_done_statfs(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep1 *c = (pgw_fusep_rep1 *)ctx;
    return pgw_fuse_dec_statfs(rx, len, c->unique, (pgw_fuse_kstatfs *)c->a);
}

pgw_err pgw_fusep_done_open(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep2 *c = (pgw_fusep_rep2 *)ctx;
    return pgw_fuse_dec_open(rx, len, c->unique, (uint64_t *)c->a,
                             (uint32_t *)c->b);
}

pgw_err pgw_fusep_done_create(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep2 *c = (pgw_fusep_rep2 *)ctx;
    return pgw_fuse_dec_create(rx, len, c->unique, (pgw_fuse_entry *)c->a,
                               (uint64_t *)c->b);
}

pgw_err pgw_fusep_done_read(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep2 *c = (pgw_fusep_rep2 *)ctx;
    return pgw_fuse_dec_read(rx, len, c->unique, (const uint8_t **)c->a,
                             (size_t *)c->b);
}

pgw_err pgw_fusep_done_write(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep1 *c = (pgw_fusep_rep1 *)ctx;
    return pgw_fuse_dec_write(rx, len, c->unique, (uint32_t *)c->a);
}

pgw_err pgw_fusep_done_readlink(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep3 *c = (pgw_fusep_rep3 *)ctx;
    return pgw_fuse_dec_readlink(rx, len, c->unique, (char *)c->a, c->cap);
}

pgw_err pgw_fusep_done_empty(void *ctx, const void *rx, size_t len)
{
    pgw_fusep_rep1 *c = (pgw_fusep_rep1 *)ctx;
    return pgw_fuse_dec_empty(rx, len, c->unique);
}
