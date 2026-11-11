/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/fake_virtiofsd_io.c - Data path of the in-memory virtiofs
 * server: READ, WRITE, READDIR and STATFS.
 *
 * Implements requirements:
 *   - VFS-03: the fake server answers reads, writes and directory listings with
 *     the exact wire layout, including the 8-byte aligned fuse_dirent records
 *   - ROB-06: the answers are built through pgw_out, never by casting a struct
 */
#include "fake_virtiofsd_int.h"

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

#include <string.h>

int32_t op_statfs(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                  pgw_buf *r)
{
    (void)r;
    if (fv_node_by_id(f, nodeid) == NULL)
        return FV_ERR(PGW_ENOENT);
    pgw_put_u64(w, 1000); /* blocks */
    pgw_put_u64(w, 500);  /* bfree */
    pgw_put_u64(w, 400);  /* bavail */
    pgw_put_u64(w, 64);   /* files */
    pgw_put_u64(w, 32);   /* ffree */
    pgw_put_u32(w, 4096); /* bsize */
    pgw_put_u32(w, 255);  /* namelen */
    pgw_put_u32(w, 4096); /* frsize */
    pgw_put_u32(w, 0);    /* padding */
    pgw_put_pad(w, 6u * sizeof(uint32_t)); /* spare[6] */
    return 0;
}

int32_t op_read(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                       pgw_buf *r)
{
    (void)pgw_get_u64(r); /* fh */
    uint64_t off = pgw_get_u64(r);
    uint32_t size = pgw_get_u32(r);
    pgw_skip(r, 4u + 8u + 4u + 4u); /* read_flags, lock_owner, flags, padding */
    if (!pgw_buf_ok(r))
        return FV_ERR(PGW_EINVAL);
    fv_node *n = fv_node_by_id(f, nodeid);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    if (n->is_dir)
        return FV_ERR(PGW_EISDIR);
    if (off >= n->size)
        return 0; /* end of file: an empty body */
    uint64_t avail = n->size - off;
    size_t len = (avail < size) ? (size_t)avail : (size_t)size;
    pgw_put_data(w, n->data + (size_t)off, len);
    return 0;
}

int32_t op_write(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                        pgw_buf *r)
{
    (void)pgw_get_u64(r); /* fh */
    uint64_t off = pgw_get_u64(r);
    uint32_t size = pgw_get_u32(r);
    pgw_skip(r, 4u + 8u + 4u + 4u); /* write_flags, lock_owner, flags, pad */
    if (!pgw_buf_ok(r) || pgw_left(r) != (size_t)size)
        return FV_ERR(PGW_EINVAL);
    fv_node *n = fv_node_by_id(f, nodeid);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    if (n->is_dir)
        return FV_ERR(PGW_EISDIR);
    if (!fv_set_size(n, off + size))
        return FV_ERR(PGW_ENOMEM);
    const uint8_t *src = pgw_take(r, (size_t)size);
    if (src == NULL)
        return FV_ERR(PGW_EINVAL);
    if (size > 0)
        memcpy(n->data + (size_t)off, src, (size_t)size);
    pgw_put_u32(w, size);
    pgw_put_u32(w, 0); /* padding */
    return 0;
}

int32_t op_readdir(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                          pgw_buf *r)
{
    (void)pgw_get_u64(r); /* fh */
    uint64_t off = pgw_get_u64(r);
    uint32_t size = pgw_get_u32(r);
    pgw_skip(r, 4u + 8u + 4u + 4u);
    if (!pgw_buf_ok(r))
        return FV_ERR(PGW_EINVAL);
    fv_node *n = fv_node_by_id(f, nodeid);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    if (!n->is_dir)
        return FV_ERR(PGW_ENOTDIR);

    size_t budget = (size_t)size;
    uint64_t idx = 0;
    for (fv_node *c = n->child; c != NULL; c = c->sibling, idx++) {
        if (idx < off)
            continue;
        size_t namelen = strlen(c->name);
        size_t reclen = (24u + namelen + 7u) & ~(size_t)7u;
        if (reclen > budget)
            break; /* the client asked for a smaller buffer than we need */
        pgw_put_u64(w, c->nodeid);
        pgw_put_u64(w, idx + 1u); /* offset to resume at */
        pgw_put_u32(w, (uint32_t)namelen);
        pgw_put_u32(w, pgw_dt_from_ftype(c->is_dir ? PGW_FT_DIR : PGW_FT_REG));
        pgw_put_data(w, c->name, namelen);
        pgw_put_pad(w, reclen - 24u - namelen);
        budget -= reclen;
    }
    return 0;
}
