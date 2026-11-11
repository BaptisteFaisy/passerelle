/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/fake_virtiofsd_ops.c - Simulated virtiofsd operations: reply
 * builders and the file/directory calls the client exercises.
 *
 * Implements requirements:
 *   - VFS-03: INIT negotiation, lookup, attributes, open, read, write, readdir,
 *     create, unlink, rename, links, statistics and synchronisation
 *   - ROB-06: the fake server answers with the exact ABI layout, including the
 *     padding a real server writes, so the client's strict decoding is tested
 */
#include "fake_virtiofsd_int.h"

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

#include <string.h>

void fv_rep_begin(pgw_out *w, void *buf, size_t cap, uint64_t unique,
                  int32_t error)
{
    pgw_out_init(w, buf, cap);
    pgw_put_u32(w, 0); /* len, patched at the end */
    pgw_put_u32(w, (uint32_t)error);
    pgw_put_u64(w, unique);
}

size_t fv_rep_finish(pgw_out *w)
{
    (void)pgw_patch_len_u32(w, 0);
    return pgw_written(w);
}

void fv_put_attr(pgw_out *w, const fv_node *n)
{
    pgw_ftype t = PGW_FT_REG;
    if (n->is_dir)
        t = PGW_FT_DIR;
    else if (n->is_link)
        t = PGW_FT_LNK;
    pgw_put_u64(w, n->nodeid);
    pgw_put_u64(w, n->size);
    pgw_put_u64(w, (n->size + 511u) / 512u);
    pgw_put_u64(w, 1000); /* atime */
    pgw_put_u64(w, 1000); /* mtime */
    pgw_put_u64(w, 1000); /* ctime */
    pgw_put_u32(w, 0);    /* atimensec */
    pgw_put_u32(w, 0);    /* mtimensec */
    pgw_put_u32(w, 0);    /* ctimensec */
    pgw_put_u32(w, pgw_linux_mode(t, n->mode));
    pgw_put_u32(w, n->is_dir ? 2u : 1u); /* nlink */
    pgw_put_u32(w, 0);                   /* uid */
    pgw_put_u32(w, 0);                   /* gid */
    pgw_put_u32(w, 0);                   /* rdev */
    pgw_put_u32(w, 4096);                /* blksize */
    pgw_put_u32(w, 0);                   /* attr flags (7.32+) */
}

void fv_put_entry(pgw_out *w, const fv_node *n)
{
    pgw_put_u64(w, n->nodeid);
    pgw_put_u64(w, 1); /* generation */
    pgw_put_u64(w, 0); /* entry_valid */
    pgw_put_u64(w, 0); /* attr_valid */
    pgw_put_u32(w, 0); /* entry_valid_nsec */
    pgw_put_u32(w, 0); /* attr_valid_nsec */
    fv_put_attr(w, n);
}

void fv_put_open_out(pgw_out *w, uint64_t fh)
{
    pgw_put_u64(w, fh);
    pgw_put_u32(w, 0); /* open_flags */
    pgw_put_u32(w, 0); /* padding */
}

/* Read one NUL-terminated name and leave the cursor just after the NUL.
 * Several requests carry more than one name (SYMLINK, RENAME), so the reader
 * must stop at the terminator instead of swallowing the whole body. */
static int32_t read_name(pgw_buf *r, char *dst, size_t cap)
{
    size_t left = pgw_left(r);
    if (left == 0 || cap == 0)
        return FV_ERR(PGW_EINVAL);
    size_t limit = (left < cap) ? left : cap;
    for (size_t i = 0; i < limit; i++) {
        uint8_t c = pgw_get_u8(r);
        if (!pgw_buf_ok(r))
            return FV_ERR(PGW_EINVAL);
        dst[i] = (char)c;
        if (c == 0)
            return 0;
    }
    return FV_ERR(PGW_ENAMETOOLONG);
}

static int32_t op_init(fake_virtiofsd *f, pgw_out *w, pgw_buf *r)
{
    (void)pgw_get_u32(r); /* major */
    (void)pgw_get_u32(r); /* minor */
    (void)pgw_get_u32(r); /* max_readahead */
    (void)pgw_get_u32(r); /* flags */
    if (!pgw_buf_ok(r))
        return FV_ERR(PGW_EINVAL);
    f->init_calls++;
    pgw_put_u32(w, f->major);
    pgw_put_u32(w, f->minor);
    pgw_put_u32(w, 65536u); /* max_readahead */
    pgw_put_u32(w, (uint32_t)(f->flags & 0xFFFFFFFFu));
    pgw_put_u16(w, 16); /* max_background */
    pgw_put_u16(w, 8);  /* congestion_threshold */
    pgw_put_u32(w, f->max_write);
    pgw_put_u32(w, 1); /* time_gran */
    pgw_put_u16(w, (uint16_t)f->max_pages);
    pgw_put_u16(w, 0); /* map_alignment */
    pgw_put_u32(w, (uint32_t)(f->flags >> 32));
    pgw_put_pad(w, 7u * sizeof(uint32_t)); /* unused[7] */
    return 0;
}

static int32_t op_lookup(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                         pgw_buf *r)
{
    char name[FV_NAME_MAX + 1u];
    int32_t rc = read_name(r, name, sizeof(name));
    if (rc < 0)
        return rc;
    fv_node *n = fv_child(f, nodeid, name);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    fv_put_entry(w, n);
    return 0;
}

static int32_t op_getattr(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                          pgw_buf *r)
{
    pgw_skip(r, 16u); /* getattr_flags, dummy, fh */
    if (!pgw_buf_ok(r))
        return FV_ERR(PGW_EINVAL);
    fv_node *n = fv_node_by_id(f, nodeid);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    pgw_put_u64(w, 0); /* attr_valid */
    pgw_put_u32(w, 0); /* attr_valid_nsec */
    pgw_put_u32(w, 0); /* dummy */
    fv_put_attr(w, n);
    return 0;
}

static int32_t op_setattr(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                          pgw_buf *r)
{
    uint32_t valid = pgw_get_u32(r);
    (void)pgw_get_u32(r);  /* padding */
    (void)pgw_get_u64(r);  /* fh */
    uint64_t size = pgw_get_u64(r);
    (void)pgw_get_u64(r);  /* lock_owner */
    (void)pgw_get_u64(r);  /* atime */
    (void)pgw_get_u64(r);  /* mtime */
    (void)pgw_get_u64(r);  /* ctime */
    (void)pgw_get_u32(r);  /* atimensec */
    (void)pgw_get_u32(r);  /* mtimensec */
    (void)pgw_get_u32(r);  /* ctimensec */
    uint32_t mode = pgw_get_u32(r);
    (void)pgw_get_u32(r);  /* unused4 */
    (void)pgw_get_u32(r);  /* uid */
    (void)pgw_get_u32(r);  /* gid */
    (void)pgw_get_u32(r);  /* unused5 */
    if (!pgw_buf_ok(r))
        return FV_ERR(PGW_EINVAL);
    fv_node *n = fv_node_by_id(f, nodeid);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    if ((valid & PGW_FUSE_FATTR_SIZE) != 0 && !fv_set_size(n, size))
        return FV_ERR(PGW_EINVAL);
    if ((valid & PGW_FUSE_FATTR_MODE) != 0)
        n->mode = mode & PGW_S_PERM_MASK;
    pgw_put_u64(w, 0); /* attr_valid */
    pgw_put_u32(w, 0); /* attr_valid_nsec */
    pgw_put_u32(w, 0); /* dummy */
    fv_put_attr(w, n);
    return 0;
}

static int32_t op_open(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                       pgw_buf *r, bool is_dir)
{
    pgw_skip(r, 8u); /* flags, open_flags */
    if (!pgw_buf_ok(r))
        return FV_ERR(PGW_EINVAL);
    fv_node *n = fv_node_by_id(f, nodeid);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    if (is_dir && !n->is_dir)
        return FV_ERR(PGW_ENOTDIR);
    if (!is_dir && n->is_dir)
        return FV_ERR(PGW_EISDIR);
    fv_put_open_out(w, ++f->next_fh);
    return 0;
}

static int32_t op_create(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                         pgw_buf *r)
{
    (void)pgw_get_u32(r); /* flags */
    uint32_t mode = pgw_get_u32(r);
    (void)pgw_get_u32(r); /* umask */
    (void)pgw_get_u32(r); /* open_flags */
    char name[FV_NAME_MAX + 1u];
    int32_t rc = read_name(r, name, sizeof(name));
    if (rc < 0)
        return rc;
    fv_node *parent = fv_node_by_id(f, nodeid);
    if (parent == NULL)
        return FV_ERR(PGW_ENOENT);
    fv_node *n = fv_add(f, parent, name, false);
    if (n == NULL)
        return FV_ERR(PGW_EEXIST);
    n->mode = mode & PGW_S_PERM_MASK;
    fv_put_entry(w, n);
    fv_put_open_out(w, ++f->next_fh);
    return 0;
}

static int32_t op_mkdir(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                        pgw_buf *r)
{
    uint32_t mode = pgw_get_u32(r);
    (void)pgw_get_u32(r); /* umask */
    char name[FV_NAME_MAX + 1u];
    int32_t rc = read_name(r, name, sizeof(name));
    if (rc < 0)
        return rc;
    fv_node *parent = fv_node_by_id(f, nodeid);
    if (parent == NULL)
        return FV_ERR(PGW_ENOENT);
    fv_node *n = fv_add(f, parent, name, true);
    if (n == NULL)
        return FV_ERR(PGW_EEXIST);
    n->mode = mode & PGW_S_PERM_MASK;
    fv_put_entry(w, n);
    return 0;
}

static int32_t op_unlink(fake_virtiofsd *f, uint64_t nodeid, pgw_buf *r,
                         bool is_dir)
{
    char name[FV_NAME_MAX + 1u];
    int32_t rc = read_name(r, name, sizeof(name));
    if (rc < 0)
        return rc;
    fv_node *parent = fv_node_by_id(f, nodeid);
    if (parent == NULL)
        return FV_ERR(PGW_ENOENT);
    fv_node *victim = fv_child(f, nodeid, name);
    if (victim == NULL)
        return FV_ERR(PGW_ENOENT);
    if (is_dir && !victim->is_dir)
        return FV_ERR(PGW_ENOTDIR);
    if (!is_dir && victim->is_dir)
        return FV_ERR(PGW_EISDIR);
    if (!fv_remove(f, parent, name))
        return FV_ERR(PGW_EIO);
    return 0;
}

static int32_t op_rename(fake_virtiofsd *f, uint64_t olddir, pgw_buf *r,
                         bool has_flags)
{
    uint64_t newdir = pgw_get_u64(r);
    if (has_flags) {
        (void)pgw_get_u32(r); /* flags */
        (void)pgw_get_u32(r); /* padding */
    }
    char oldname[FV_NAME_MAX + 1u];
    char newname[FV_NAME_MAX + 1u];
    int32_t rc = read_name(r, oldname, sizeof(oldname));
    if (rc < 0)
        return rc;
    rc = read_name(r, newname, sizeof(newname));
    if (rc < 0)
        return rc;
    fv_node *from = fv_node_by_id(f, olddir);
    fv_node *to = fv_node_by_id(f, newdir);
    if (from == NULL || to == NULL)
        return FV_ERR(PGW_ENOENT);
    if (fv_child(f, olddir, oldname) == NULL)
        return FV_ERR(PGW_ENOENT);
    (void)fv_remove(f, to, newname); /* replace an existing target */
    fv_node *victim = fv_detach(from, oldname);
    if (victim == NULL)
        return FV_ERR(PGW_EIO);
    size_t len = strlen(newname);
    memcpy(victim->name, newname, len + 1u);
    victim->parent = to;
    victim->sibling = to->child;
    to->child = victim;
    return 0;
}

static int32_t op_symlink(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                          pgw_buf *r)
{
    char name[FV_NAME_MAX + 1u];
    int32_t rc = read_name(r, name, sizeof(name));
    if (rc < 0)
        return rc;
    char target[FV_NAME_MAX + 1u];
    rc = read_name(r, target, sizeof(target));
    if (rc < 0)
        return rc;
    fv_node *parent = fv_node_by_id(f, nodeid);
    if (parent == NULL)
        return FV_ERR(PGW_ENOENT);
    fv_node *n = fv_add(f, parent, name, false);
    if (n == NULL)
        return FV_ERR(PGW_EEXIST);
    n->is_link = true;
    memcpy(n->target, target, strlen(target) + 1u);
    n->size = strlen(target);
    fv_put_entry(w, n);
    return 0;
}

static int32_t op_link(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                       pgw_buf *r)
{
    uint64_t oldnodeid = pgw_get_u64(r);
    char name[FV_NAME_MAX + 1u];
    int32_t rc = read_name(r, name, sizeof(name));
    if (rc < 0)
        return rc;
    fv_node *parent = fv_node_by_id(f, nodeid);
    fv_node *old = fv_node_by_id(f, oldnodeid);
    if (parent == NULL || old == NULL)
        return FV_ERR(PGW_ENOENT);
    fv_node *n = fv_add(f, parent, name, old->is_dir);
    if (n == NULL)
        return FV_ERR(PGW_EEXIST);
    n->mode = old->mode;
    if (old->size > 0 && !fv_set_size(n, old->size))
        return FV_ERR(PGW_ENOMEM);
    if (old->size > 0 && n->data != NULL)
        memcpy(n->data, old->data, (size_t)old->size);
    fv_put_entry(w, n);
    return 0;
}

static int32_t op_readlink(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid,
                           pgw_buf *r)
{
    (void)r;
    fv_node *n = fv_node_by_id(f, nodeid);
    if (n == NULL)
        return FV_ERR(PGW_ENOENT);
    if (!n->is_link)
        return FV_ERR(PGW_EINVAL);
    size_t len = strlen(n->target);
    pgw_put_data(w, n->target, len);
    return 0;
}

int32_t fv_op_dispatch(fake_virtiofsd *f, pgw_out *w, uint32_t opcode,
                       uint64_t nodeid, pgw_buf *r)
{
    switch (opcode) {
    case PGW_FUSE_INIT:
        return op_init(f, w, r);
    case PGW_FUSE_LOOKUP:
        return op_lookup(f, w, nodeid, r);
    case PGW_FUSE_GETATTR:
        return op_getattr(f, w, nodeid, r);
    case PGW_FUSE_SETATTR:
        return op_setattr(f, w, nodeid, r);
    case PGW_FUSE_OPEN:
        return op_open(f, w, nodeid, r, false);
    case PGW_FUSE_OPENDIR:
        return op_open(f, w, nodeid, r, true);
    case PGW_FUSE_CREATE:
        return op_create(f, w, nodeid, r);
    case PGW_FUSE_MKDIR:
        return op_mkdir(f, w, nodeid, r);
    case PGW_FUSE_UNLINK:
        return op_unlink(f, nodeid, r, false);
    case PGW_FUSE_RMDIR:
        return op_unlink(f, nodeid, r, true);
    case PGW_FUSE_RENAME:
        return op_rename(f, nodeid, r, false);
    case PGW_FUSE_RENAME2:
        return op_rename(f, nodeid, r, true);
    case PGW_FUSE_SYMLINK:
        return op_symlink(f, w, nodeid, r);
    case PGW_FUSE_LINK:
        return op_link(f, w, nodeid, r);
    case PGW_FUSE_READLINK:
        return op_readlink(f, w, nodeid, r);
    case PGW_FUSE_READ:
        return op_read(f, w, nodeid, r);
    case PGW_FUSE_WRITE:
        return op_write(f, w, nodeid, r);
    case PGW_FUSE_READDIR:
        return op_readdir(f, w, nodeid, r);
    case PGW_FUSE_STATFS:
        return op_statfs(f, w, nodeid, r);
    case PGW_FUSE_RELEASE:
    case PGW_FUSE_RELEASEDIR:
    case PGW_FUSE_FSYNC:
    case PGW_FUSE_FSYNCDIR:
    case PGW_FUSE_FLUSH:
    case PGW_FUSE_DESTROY:
        return 0; /* header-only reply */
    default:
        return FV_ERR(PGW_ENOSYS);
    }
}
