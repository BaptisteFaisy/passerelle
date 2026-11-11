/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/src/p9_backend.c - layer 3 backend over a 9P2000.L session.
 *
 * This is the portable half of the 9P driver: it contains no QNX header, so the
 * PC build compiles and tests it, and the QNX resource manager only ever calls
 * these `pgw_fs_backend_ops`. Every call carries the caller's priority
 * (QNX-03) and lets layer 2 split a transfer that does not fit one message
 * (P9-08).
 *
 * Covered requirements: FS-01..FS-11, P9-02, P9-03, P9-07, P9-08, QNX-03.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/err.h"
#include "pgw/fs9p.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/p9.h"
#include "pgw/rmfs.h"
#include "pgw/time.h"

#include <string.h>

/* One backend instance per mount: the session plus the root fid. */
typedef struct p9_impl {
    pgw_p9_session *s;
    uint32_t root_fid;
} p9_impl;

static p9_impl g_p9_impl;

/* --- helpers -------------------------------------------------------------- */

/* The wire carries Linux mode bits; layer 3 speaks pgw_ftype (common/err.h). */
static void attr_from_p9(const pgw_p9_attr *in, pgw_fs_attr *out)
{
    memset(out, 0, sizeof *out);
    out->type = pgw_ftype_from_linux_mode(in->mode);
    out->mode = in->mode & PGW_S_PERM_MASK;
    out->uid = in->uid;
    out->gid = in->gid;
    out->ino = in->qid.path;
    out->size = in->size;
    out->nlink = in->nlink;
    out->atime_ns = pgw_ns_join(in->atime_sec, (uint32_t)in->atime_nsec);
    out->mtime_ns = pgw_ns_join(in->mtime_sec, (uint32_t)in->mtime_nsec);
    out->ctime_ns = pgw_ns_join(in->ctime_sec, (uint32_t)in->ctime_nsec);
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

/* Walk a path to a fresh fid. The caller owns it and must clunk it. */
static pgw_err walk_to(p9_impl *im, const char *path, uint32_t *fid,
                       pgw_p9_qid *qid, uint32_t prio)
{
    return pgw_p9_walk_path(im->s, im->root_fid, path, fid, qid, prio);
}

/* --- vtable --------------------------------------------------------------- */

static void p9_destroy(void *impl)
{
    (void)impl; /* the session outlives the backend: the driver closes it */
}

static pgw_err p9_lookup(void *impl, const char *relpath, pgw_fs_attr *out,
                         uint32_t prio)
{
    p9_impl *im = impl;
    uint32_t fid = 0;
    pgw_err e = walk_to(im, relpath, &fid, NULL, prio);
    if (e != PGW_OK)
        return e;
    pgw_p9_attr a;
    e = pgw_p9_getattr(im->s, fid, &a, prio);
    (void)pgw_p9_clunk(im->s, fid, prio); /* never leak a fid (P9-07) */
    if (e == PGW_OK)
        attr_from_p9(&a, out);
    return e;
}

static pgw_err p9_getattr(void *impl, const char *relpath, pgw_fs_attr *out,
                          uint32_t prio)
{
    return p9_lookup(impl, relpath, out, prio);
}

static pgw_err p9_setattr(void *impl, const char *relpath,
                          const pgw_fs_setattr *sa, uint32_t prio)
{
    p9_impl *im = impl;
    uint32_t fid = 0;
    pgw_err e = walk_to(im, relpath, &fid, NULL, prio);
    if (e != PGW_OK)
        return e;
    pgw_p9_setattr_t wire;
    memset(&wire, 0, sizeof wire);
    if (sa->set_mode) {
        wire.valid |= PGW_P9_SETATTR_MODE;
        wire.mode = pgw_linux_mode(PGW_FT_UNKNOWN, sa->mode) & PGW_S_PERM_MASK;
    }
    if (sa->set_uid) {
        wire.valid |= PGW_P9_SETATTR_UID;
        wire.uid = sa->uid;
    }
    if (sa->set_gid) {
        wire.valid |= PGW_P9_SETATTR_GID;
        wire.gid = sa->gid;
    }
    if (sa->set_size) {
        wire.valid |= PGW_P9_SETATTR_SIZE;
        wire.size = sa->size;
    }
    if (sa->set_atime) {
        wire.valid |= PGW_P9_SETATTR_ATIME | PGW_P9_SETATTR_ATIME_SET;
        uint32_t nsec = 0;
        pgw_ns_split(sa->atime_ns, &wire.atime_sec, &nsec);
        wire.atime_nsec = nsec;
    }
    if (sa->set_mtime) {
        wire.valid |= PGW_P9_SETATTR_MTIME | PGW_P9_SETATTR_MTIME_SET;
        uint32_t nsec = 0;
        pgw_ns_split(sa->mtime_ns, &wire.mtime_sec, &nsec);
        wire.mtime_nsec = nsec;
    }
    e = pgw_p9_setattr(im->s, fid, &wire, prio);
    (void)pgw_p9_clunk(im->s, fid, prio);
    return e;
}

static pgw_err p9_open(void *impl, const char *relpath, uint32_t pgw_oflags,
                       uint32_t create_mode, uint64_t *out_handle, uint32_t prio)
{
    p9_impl *im = impl;
    uint32_t fid = 0;
    pgw_err e;
    if ((pgw_oflags & PGW_O_CREAT) != 0u) {
        char dir[PGW_PATH_MAX];
        const char *name = NULL;
        e = split_last(relpath, dir, sizeof dir, &name);
        if (e != PGW_OK)
            return e;
        uint32_t dfid = 0;
        e = walk_to(im, dir, &dfid, NULL, prio);
        if (e != PGW_OK)
            return e;
        pgw_p9_qid qid;
        uint32_t flags = pgw_oflags & ~PGW_O_CREAT;
        e = pgw_p9_lcreate(im->s, dfid, name, flags, create_mode, 0u, &qid, prio);
        /* On success the directory fid becomes the new file's fid (P9-02). */
        if (e == PGW_OK)
            fid = dfid;
        else
            (void)pgw_p9_clunk(im->s, dfid, prio);
    } else {
        e = walk_to(im, relpath, &fid, NULL, prio);
        if (e == PGW_OK)
            e = pgw_p9_lopen(im->s, fid, pgw_oflags, prio);
    }
    if (e != PGW_OK) {
        if (fid != 0)
            (void)pgw_p9_clunk(im->s, fid, prio);
        return e;
    }
    *out_handle = (uint64_t)fid;
    return PGW_OK;
}

static pgw_err p9_close(void *impl, uint64_t handle, uint32_t prio)
{
    p9_impl *im = impl;
    return pgw_p9_clunk(im->s, (uint32_t)handle, prio);
}

static pgw_err p9_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                       size_t len, size_t *out_read, uint32_t prio)
{
    p9_impl *im = impl;
    return pgw_p9_read(im->s, (uint32_t)handle, off, buf, len, out_read, prio);
}

static pgw_err p9_write(void *impl, uint64_t handle, uint64_t off,
                        const void *buf, size_t len, size_t *out_written,
                        uint32_t prio)
{
    p9_impl *im = impl;
    return pgw_p9_write(im->s, (uint32_t)handle, off, buf, len, out_written, prio);
}

static pgw_err p9_readdir(void *impl, uint64_t handle, uint64_t off,
                          pgw_fs_dirent *out, size_t max_entries,
                          size_t *out_count, uint64_t *next_off, uint32_t prio)
{
    p9_impl *im = impl;
    pgw_p9_dirent wire[PGW_MAXWELEM];
    if (max_entries == 0 || max_entries > PGW_MAXWELEM)
        return PGW_EINVAL;
    pgw_err e = pgw_p9_readdir(im->s, (uint32_t)handle, off, wire, max_entries,
                               out_count, next_off, prio);
    if (e != PGW_OK)
        return e;
    for (size_t i = 0; i < *out_count; i++) {
        out[i].ino = wire[i].qid.path;
        out[i].next_off = wire[i].offset;
        out[i].type = pgw_ftype_from_dt(wire[i].type);
        size_t n = wire[i].name_len;
        if (n >= sizeof out[i].name)
            n = sizeof out[i].name - 1u;
        memcpy(out[i].name, wire[i].name, n);
        out[i].name[n] = '\0';
    }
    return PGW_OK;
}

static pgw_err p9_mkdir(void *impl, const char *relpath, uint32_t mode,
                        uint32_t prio)
{
    p9_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(relpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint32_t dfid = 0;
    e = walk_to(im, dir, &dfid, NULL, prio);
    if (e != PGW_OK)
        return e;
    pgw_p9_qid qid;
    e = pgw_p9_mkdir(im->s, dfid, name, mode, 0u, &qid, prio);
    (void)pgw_p9_clunk(im->s, dfid, prio);
    return e;
}

static pgw_err p9_unlink(void *impl, const char *relpath, bool is_dir,
                         uint32_t prio)
{
    p9_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(relpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint32_t dfid = 0;
    e = walk_to(im, dir, &dfid, NULL, prio);
    if (e != PGW_OK)
        return e;
    e = pgw_p9_unlinkat(im->s, dfid, name, is_dir, prio);
    (void)pgw_p9_clunk(im->s, dfid, prio);
    return e;
}

static pgw_err p9_rename(void *impl, const char *oldpath, const char *newpath,
                         uint32_t prio)
{
    p9_impl *im = impl;
    char odir[PGW_PATH_MAX];
    char ndir[PGW_PATH_MAX];
    const char *oname = NULL;
    const char *nname = NULL;
    pgw_err e = split_last(oldpath, odir, sizeof odir, &oname);
    if (e == PGW_OK)
        e = split_last(newpath, ndir, sizeof ndir, &nname);
    if (e != PGW_OK)
        return e;
    uint32_t ofid = 0;
    uint32_t nfid = 0;
    e = walk_to(im, odir, &ofid, NULL, prio);
    if (e == PGW_OK)
        e = walk_to(im, ndir, &nfid, NULL, prio);
    if (e == PGW_OK)
        e = pgw_p9_renameat(im->s, ofid, oname, nfid, nname, prio);
    if (ofid != 0)
        (void)pgw_p9_clunk(im->s, ofid, prio);
    if (nfid != 0)
        (void)pgw_p9_clunk(im->s, nfid, prio);
    return e;
}

static pgw_err p9_symlink(void *impl, const char *linkpath, const char *target,
                          uint32_t prio)
{
    p9_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(linkpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint32_t dfid = 0;
    e = walk_to(im, dir, &dfid, NULL, prio);
    if (e != PGW_OK)
        return e;
    pgw_p9_qid qid;
    e = pgw_p9_symlink(im->s, dfid, name, target, 0u, &qid, prio);
    (void)pgw_p9_clunk(im->s, dfid, prio);
    return e;
}

static pgw_err p9_readlink(void *impl, const char *linkpath, char *target,
                           size_t cap, uint32_t prio)
{
    p9_impl *im = impl;
    uint32_t fid = 0;
    pgw_err e = walk_to(im, linkpath, &fid, NULL, prio);
    if (e != PGW_OK)
        return e;
    e = pgw_p9_readlink(im->s, fid, target, cap, prio);
    (void)pgw_p9_clunk(im->s, fid, prio);
    return e;
}

static pgw_err p9_link(void *impl, const char *existing, const char *newpath,
                       uint32_t prio)
{
    p9_impl *im = impl;
    char dir[PGW_PATH_MAX];
    const char *name = NULL;
    pgw_err e = split_last(newpath, dir, sizeof dir, &name);
    if (e != PGW_OK)
        return e;
    uint32_t dfid = 0;
    uint32_t src = 0;
    e = walk_to(im, dir, &dfid, NULL, prio);
    if (e == PGW_OK)
        e = walk_to(im, existing, &src, NULL, prio);
    if (e == PGW_OK)
        e = pgw_p9_link(im->s, dfid, src, name, prio);
    if (dfid != 0)
        (void)pgw_p9_clunk(im->s, dfid, prio);
    if (src != 0)
        (void)pgw_p9_clunk(im->s, src, prio);
    return e;
}

static pgw_err p9_fsync(void *impl, uint64_t handle, uint32_t prio)
{
    p9_impl *im = impl;
    return pgw_p9_fsync(im->s, (uint32_t)handle, prio);
}

static pgw_err p9_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio)
{
    p9_impl *im = impl;
    pgw_p9_statfs_t st;
    pgw_err e = pgw_p9_statfs(im->s, im->root_fid, &st, prio);
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

static const pgw_fs_backend_ops g_p9_ops = {
    .destroy = p9_destroy,
    .lookup = p9_lookup,
    .getattr = p9_getattr,
    .setattr = p9_setattr,
    .open = p9_open,
    .close = p9_close,
    .read = p9_read,
    .write = p9_write,
    .readdir = p9_readdir,
    .mkdir = p9_mkdir,
    .unlink = p9_unlink,
    .rename = p9_rename,
    .symlink = p9_symlink,
    .readlink = p9_readlink,
    .link = p9_link,
    .fsync = p9_fsync,
    .statfs = p9_statfs,
};

const pgw_fs_backend_ops *fs9p_backend_ops(void)
{
    return &g_p9_ops;
}

void *fs9p_backend_init(pgw_p9_session *s)
{
    if (s == NULL)
        return NULL;
    g_p9_impl.s = s;
    g_p9_impl.root_fid = pgw_p9_session_root_fid(s);
    return &g_p9_impl;
}
