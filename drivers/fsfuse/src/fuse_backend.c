/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/src/fuse_backend.c - layer 3 backend over a FUSE session.
 *
 * The portable half of the virtiofs driver: no QNX header, so the PC build
 * compiles and tests it, and the QNX resource manager only calls these
 * `pgw_fs_backend_ops`. Every call carries the caller's priority (QNX-03) and
 * asks the session to use the priority queue when the message is a FORGET or
 * an INTERRUPT (VFS-02).
 *
 * Covered requirements: FS-01..FS-11, VFS-03, VFS-04, VFS-05, QNX-03.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/err.h"
#include "pgw/fsfuse.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/rmfs.h"

#include <string.h>

typedef struct fuse_impl {
    pgw_fuse_session *s;
} fuse_impl;

static fuse_impl g_fuse_impl;

/* --- helpers -------------------------------------------------------------- */

static void attr_from_fuse(const pgw_fuse_attr *in, pgw_fs_attr *out)
{
    memset(out, 0, sizeof *out);
    out->type = pgw_ftype_from_linux_mode(in->mode);
    out->mode = in->mode & PGW_S_PERM_MASK;
    out->uid = in->uid;
    out->gid = in->gid;
    out->ino = in->ino;
    out->size = in->size;
    out->nlink = in->nlink;
    out->atime_ns = in->atime * 1000000000ull + in->atimensec;
    out->mtime_ns = in->mtime * 1000000000ull + in->mtimensec;
    out->ctime_ns = in->ctime * 1000000000ull + in->ctimensec;
}

/* Split "a/b/c" into the directory part and the last component. A path with no
 * separator lives in the share root, so the directory is ".". */
static pgw_err split_last(const char *path, char *dir, size_t dir_cap,
                          const char **name)
{
    size_t n = strnlen(path, PGW_PATH_MAX);
    if (n == 0 || n >= PGW_PATH_MAX)
        return n == 0 ? PGW_EINVAL : PGW_ENAMETOOLONG;
    const char *slash = NULL;
    for (size_t i = n; i > 0; i--) {
        if (path[i - 1u] == '/') {
            slash = path + i - 1u;
            break;
        }
    }
    *name = slash != NULL ? slash + 1 : path;
    if (**name == '\0')
        return PGW_EINVAL;
    size_t dlen = slash != NULL ? (size_t)(slash - path) : 1u;
    if (dlen >= dir_cap)
        return PGW_ENAMETOOLONG;
    if (slash == NULL) {
        dir[0] = '.';
    } else {
        memcpy(dir, path, dlen);
    }
    dir[dlen] = '\0';
    return PGW_OK;
}

/* Look a path up and return its node id; the caller releases the reference
 * with pgw_fuse_forget_node() (VFS-04). */
static pgw_err node_of(fuse_impl *im, const char *path, uint64_t parent,
                       uint64_t *out_node)
{
    pgw_fuse_entry e;
    pgw_err err = pgw_fuse_lookup(im->s, parent, path, &e, 0u);
    if (err != PGW_OK)
        return err;
    *out_node = e.nodeid;
    return PGW_OK;
}

/* --- vtable --------------------------------------------------------------- */

static void fuse_destroy(void *impl)
{
    (void)impl;
}

static pgw_err fuse_lookup(void *impl, const char *relpath, pgw_fs_attr *out,
                           uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = PGW_INO_ROOT;
    pgw_err e = node_of(im, relpath, PGW_FUSE_ROOT_ID, &node);
    if (e != PGW_OK)
        return e;
    pgw_fuse_attr a;
    e = pgw_fuse_getattr(im->s, node, &a, prio);
    (void)pgw_fuse_forget_node(im->s, node); /* give the reference back (VFS-04) */
    if (e == PGW_OK)
        attr_from_fuse(&a, out);
    return e;
}

static pgw_err fuse_getattr(void *impl, const char *relpath, pgw_fs_attr *out,
                            uint32_t prio)
{
    return fuse_lookup(impl, relpath, out, prio);
}

static pgw_err fuse_setattr(void *impl, const char *relpath,
                            const pgw_fs_setattr *sa, uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = PGW_INO_ROOT;
    pgw_err e = node_of(im, relpath, PGW_FUSE_ROOT_ID, &node);
    if (e != PGW_OK)
        return e;
    pgw_fuse_setattr_in wire;
    memset(&wire, 0, sizeof wire);
    if (sa->set_mode) {
        wire.valid |= PGW_FUSE_FATTR_MODE;
        wire.mode = sa->mode & PGW_S_PERM_MASK;
    }
    if (sa->set_uid) {
        wire.valid |= PGW_FUSE_FATTR_UID;
        wire.uid = sa->uid;
    }
    if (sa->set_gid) {
        wire.valid |= PGW_FUSE_FATTR_GID;
        wire.gid = sa->gid;
    }
    if (sa->set_size) {
        wire.valid |= PGW_FUSE_FATTR_SIZE;
        wire.size = sa->size;
    }
    if (sa->set_atime) {
        wire.valid |= PGW_FUSE_FATTR_ATIME;
        wire.atime = sa->atime_ns / 1000000000ull;
        wire.atime_nsec = (uint32_t)(sa->atime_ns % 1000000000ull);
    }
    if (sa->set_mtime) {
        wire.valid |= PGW_FUSE_FATTR_MTIME;
        wire.mtime = sa->mtime_ns / 1000000000ull;
        wire.mtime_nsec = (uint32_t)(sa->mtime_ns % 1000000000ull);
    }
    pgw_fuse_attr out;
    e = pgw_fuse_setattr(im->s, node, &wire, &out, prio);
    (void)pgw_fuse_forget_node(im->s, node);
    return e;
}

static pgw_err fuse_open(void *impl, const char *relpath, uint32_t pgw_oflags,
                         uint32_t create_mode, uint64_t *out_handle, uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = PGW_INO_ROOT;
    pgw_err e;
    uint64_t fh = PGW_FH_NONE;
    if ((pgw_oflags & PGW_O_CREAT) != 0u) {
        char dir[PGW_PATH_MAX];
        const char *name = NULL;
        e = split_last(relpath, dir, sizeof dir, &name);
        if (e != PGW_OK)
            return e;
        uint64_t parent = PGW_INO_ROOT;
        e = node_of(im, dir, PGW_FUSE_ROOT_ID, &parent);
        if (e != PGW_OK)
            return e;
        pgw_fuse_entry entry;
        e = pgw_fuse_create(im->s, parent, name, pgw_oflags & ~PGW_O_CREAT,
                            create_mode, &entry, &fh, prio);
        if (e == PGW_OK)
            node = entry.nodeid;
        (void)pgw_fuse_forget_node(im->s, parent);
    } else {
        e = node_of(im, relpath, PGW_FUSE_ROOT_ID, &node);
        if (e == PGW_OK)
            e = pgw_fuse_open(im->s, node, pgw_oflags, false, &fh, prio);
    }
    if (e != PGW_OK)
        return e;
    /* The handle is the pair (node id, file handle) so the release can give
     * both back (VFS-04). */
    *out_handle = (node << 32) | (fh & 0xffffffffu);
    return PGW_OK;
}

static pgw_err fuse_close(void *impl, uint64_t handle, uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = handle >> 32;
    uint64_t fh = handle & 0xffffffffu;
    pgw_err e = pgw_fuse_release(im->s, node, fh, false, prio);
    (void)pgw_fuse_forget_node(im->s, node); /* the reference goes back (VFS-04) */
    return e;
}

static pgw_err fuse_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                         size_t len, size_t *out_read, uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = handle >> 32;
    uint64_t fh = handle & 0xffffffffu;
    return pgw_fuse_read(im->s, node, fh, off, buf, len, out_read, prio);
}

static pgw_err fuse_write(void *impl, uint64_t handle, uint64_t off,
                          const void *buf, size_t len, size_t *out_written,
                          uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = handle >> 32;
    uint64_t fh = handle & 0xffffffffu;
    return pgw_fuse_write(im->s, node, fh, off, buf, len, out_written, prio);
}

static pgw_err fuse_readdir(void *impl, uint64_t handle, uint64_t off,
                            pgw_fs_dirent *out, size_t max_entries,
                            size_t *out_count, uint64_t *next_off, uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = handle >> 32;
    uint64_t fh = handle & 0xffffffffu;
    pgw_fuse_dirent wire[PGW_MAXWELEM];
    if (max_entries == 0 || max_entries > PGW_MAXWELEM)
        return PGW_EINVAL;
    pgw_err e = pgw_fuse_readdir(im->s, node, fh, off, wire, max_entries,
                                 out_count, prio);
    if (e != PGW_OK)
        return e;
    for (size_t i = 0; i < *out_count; i++) {
        out[i].ino = wire[i].ino;
        out[i].next_off = wire[i].off;
        out[i].type = pgw_ftype_from_dt((uint8_t)wire[i].type);
        size_t n = wire[i].namelen;
        if (n >= sizeof out[i].name)
            n = sizeof out[i].name - 1u;
        memcpy(out[i].name, wire[i].name, n);
        out[i].name[n] = '\0';
    }
    /* The FUSE cookie of the last entry is the offset the next read starts
     * at (FS-03). */
    if (*out_count > 0)
        *next_off = wire[*out_count - 1u].off;
    return PGW_OK;
}

static pgw_err fuse_mkdir(void *impl, const char *relpath, uint32_t mode,
                          uint32_t prio)
{
    fuse_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(relpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint64_t parent = PGW_INO_ROOT;
    e = node_of(im, dir, PGW_FUSE_ROOT_ID, &parent);
    if (e != PGW_OK)
        return e;
    pgw_fuse_entry entry;
    e = pgw_fuse_mkdir(im->s, parent, name, mode, &entry, prio);
    (void)pgw_fuse_forget_node(im->s, parent);
    return e;
}

static pgw_err fuse_unlink(void *impl, const char *relpath, bool is_dir,
                           uint32_t prio)
{
    fuse_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(relpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint64_t parent = PGW_INO_ROOT;
    e = node_of(im, dir, PGW_FUSE_ROOT_ID, &parent);
    if (e != PGW_OK)
        return e;
    e = pgw_fuse_unlink(im->s, parent, name, is_dir, prio);
    (void)pgw_fuse_forget_node(im->s, parent);
    return e;
}

static pgw_err fuse_rename(void *impl, const char *oldpath, const char *newpath,
                           uint32_t prio)
{
    fuse_impl *im = impl;
    char odir[PGW_PATH_MAX];
    char ndir[PGW_PATH_MAX];
    const char *oname = NULL;
    const char *nname = NULL;
    pgw_err e = split_last(oldpath, odir, sizeof odir, &oname);
    if (e == PGW_OK)
        e = split_last(newpath, ndir, sizeof ndir, &nname);
    if (e != PGW_OK)
        return e;
    uint64_t olddir = PGW_INO_ROOT;
    uint64_t newdir = PGW_INO_ROOT;
    e = node_of(im, odir, PGW_FUSE_ROOT_ID, &olddir);
    if (e == PGW_OK)
        e = node_of(im, ndir, PGW_FUSE_ROOT_ID, &newdir);
    if (e == PGW_OK)
        e = pgw_fuse_rename(im->s, olddir, oname, newdir, nname, prio);
    (void)pgw_fuse_forget_node(im->s, olddir);
    (void)pgw_fuse_forget_node(im->s, newdir);
    return e;
}

static pgw_err fuse_symlink(void *impl, const char *linkpath, const char *target,
                            uint32_t prio)
{
    fuse_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(linkpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint64_t parent = PGW_INO_ROOT;
    e = node_of(im, dir, PGW_FUSE_ROOT_ID, &parent);
    if (e != PGW_OK)
        return e;
    pgw_fuse_entry entry;
    e = pgw_fuse_symlink(im->s, parent, name, target, &entry, prio);
    (void)pgw_fuse_forget_node(im->s, parent);
    return e;
}

static pgw_err fuse_readlink(void *impl, const char *linkpath, char *target,
                             size_t cap, uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = PGW_INO_ROOT;
    pgw_err e = node_of(im, linkpath, PGW_FUSE_ROOT_ID, &node);
    if (e != PGW_OK)
        return e;
    e = pgw_fuse_readlink(im->s, node, target, cap, prio);
    (void)pgw_fuse_forget_node(im->s, node);
    return e;
}

static pgw_err fuse_link(void *impl, const char *existing, const char *newpath,
                         uint32_t prio)
{
    fuse_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(newpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint64_t parent = PGW_INO_ROOT;
    uint64_t oldnode = PGW_INO_ROOT;
    e = node_of(im, dir, PGW_FUSE_ROOT_ID, &parent);
    if (e == PGW_OK)
        e = node_of(im, existing, PGW_FUSE_ROOT_ID, &oldnode);
    if (e == PGW_OK) {
        pgw_fuse_entry entry;
        e = pgw_fuse_link(im->s, parent, oldnode, name, &entry, prio);
    }
    (void)pgw_fuse_forget_node(im->s, parent);
    (void)pgw_fuse_forget_node(im->s, oldnode);
    return e;
}

static pgw_err fuse_fsync(void *impl, uint64_t handle, uint32_t prio)
{
    fuse_impl *im = impl;
    uint64_t node = handle >> 32;
    uint64_t fh = handle & 0xffffffffu;
    return pgw_fuse_fsync(im->s, node, fh, false, prio);
}

static pgw_err fuse_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio)
{
    fuse_impl *im = impl;
    pgw_fuse_kstatfs st;
    pgw_err e = pgw_fuse_statfs(im->s, PGW_FUSE_ROOT_ID, &st, prio);
    if (e != PGW_OK)
        return e;
    memset(out, 0, sizeof *out);
    out->bsize = st.bsize;
    out->namelen = st.namelen;
    out->blocks = st.blocks;
    out->bfree = st.bfree;
    out->bavail = st.bavail;
    out->files = st.files;
    out->ffree = st.ffree;
    return PGW_OK;
}

static const pgw_fs_backend_ops g_fuse_ops = {
    .destroy = fuse_destroy,
    .lookup = fuse_lookup,
    .getattr = fuse_getattr,
    .setattr = fuse_setattr,
    .open = fuse_open,
    .close = fuse_close,
    .read = fuse_read,
    .write = fuse_write,
    .readdir = fuse_readdir,
    .mkdir = fuse_mkdir,
    .unlink = fuse_unlink,
    .rename = fuse_rename,
    .symlink = fuse_symlink,
    .readlink = fuse_readlink,
    .link = fuse_link,
    .fsync = fuse_fsync,
    .statfs = fuse_statfs,
};

const pgw_fs_backend_ops *fsfuse_backend_ops(void)
{
    return &g_fuse_ops;
}

void *fsfuse_backend_init(pgw_fuse_session *s)
{
    if (s == NULL)
        return NULL;
    g_fuse_impl.s = s;
    return &g_fuse_impl;
}
