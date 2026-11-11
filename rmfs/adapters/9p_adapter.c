/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/adapters/9p_adapter.c - pgw_fs_backend_ops over a 9P2000.L session.
 *
 * Implements requirement P9-02/P9-03 from the layer-3 side (the wire codecs
 * themselves live in proto9p): every entry of pgw_fs_backend_ops is translated
 * into the corresponding pgw_p9_* session call, so that the same layer 3 and
 * the same conformance bench serve the 9P client (FS-01..FS-15).
 *
 * Paths: 9P is fid based. A path is walked from the session root fid, and an
 * open file keeps its walked fid as the layer-3 handle. 9P has no "rename in
 * place" without a dfid pair, so the adapter splits each path into a parent
 * directory plus a name, exactly as Tmkdir/Tunlinkat/Tsymlink expect.
 *
 * This file is compiled only when the proto9p target exists (see
 * rmfs/CMakeLists.txt): the layer-3 core builds and tests without it.
 */
#include "pgw/rmfs.h"

#include "pgw/err.h"
#include "pgw/log.h"
#include "pgw/p9.h"
#include "pgw/time.h"

#include "../src/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct p9_backend {
    pgw_p9_session *session;
    uint32_t root_fid;
} p9_backend;

/* --- conversions ---------------------------------------------------------- */

static void p9_attr_to_fs(const pgw_p9_attr *in, pgw_fs_attr *out)
{
    memset(out, 0, sizeof(*out));
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

static void fs_setattr_to_p9(const pgw_fs_setattr *sa,
                             struct pgw_p9_setattr *out)
{
    memset(out, 0, sizeof(*out));
    if (sa->set_mode) {
        out->valid |= PGW_P9_SETATTR_MODE;
        out->mode = sa->mode & PGW_S_PERM_MASK;
    }
    if (sa->set_uid) {
        out->valid |= PGW_P9_SETATTR_UID;
        out->uid = sa->uid;
    }
    if (sa->set_gid) {
        out->valid |= PGW_P9_SETATTR_GID;
        out->gid = sa->gid;
    }
    if (sa->set_size) {
        out->valid |= PGW_P9_SETATTR_SIZE;
        out->size = sa->size;
    }
    if (sa->set_atime) {
        out->valid |= PGW_P9_SETATTR_ATIME | PGW_P9_SETATTR_ATIME_SET;
        pgw_ns_split(sa->atime_ns, &out->atime_sec, NULL);
        out->atime_nsec = sa->atime_ns % 1000000000ULL;
    }
    if (sa->set_mtime) {
        out->valid |= PGW_P9_SETATTR_MTIME | PGW_P9_SETATTR_MTIME_SET;
        pgw_ns_split(sa->mtime_ns, &out->mtime_sec, NULL);
        out->mtime_nsec = sa->mtime_ns % 1000000000ULL;
    }
}

/* Walk a relative path from the session root, returning a fresh fid. The
 * caller owns the fid and must clunk it. */
static pgw_err p9_walk_root(p9_backend *b, const char *rel, uint32_t *out_fid,
                            pgw_p9_qid *out_qid, uint32_t prio)
{
    return pgw_p9_walk_path(b->session, b->root_fid, rel, out_fid, out_qid,
                            prio);
}

/* Walk the parent directory of `rel` and hand back its fid plus the last
 * name: the shape every 9P "…at" message expects. */
static pgw_err p9_walk_parent(p9_backend *b, const char *rel, uint32_t *out_fid,
                              char *name, size_t name_cap, uint32_t prio)
{
    char dir[PGW_PATH_MAX];
    pgw_err err = rmfs_split_path(rel, dir, sizeof(dir), name, name_cap);
    if (err != PGW_OK)
        return err;
    return p9_walk_root(b, dir, out_fid, NULL, prio);
}

/* --- vtable --------------------------------------------------------------- */

static void p9_destroy(void *impl)
{
    free(impl); /* the session itself belongs to the caller */
}

static pgw_err p9_getattr_common(void *impl, const char *rel, pgw_fs_attr *out,
                                 uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    uint32_t fid = 0;
    pgw_err err = p9_walk_root(b, rel, &fid, NULL, prio);
    if (err != PGW_OK)
        return err;
    pgw_p9_attr pa;
    err = pgw_p9_getattr(b->session, fid, &pa, prio);
    pgw_p9_clunk(b->session, fid, prio);
    if (err == PGW_OK)
        p9_attr_to_fs(&pa, out);
    return err;
}

static pgw_err p9_lookup(void *impl, const char *rel, pgw_fs_attr *out,
                         uint32_t prio)
{
    /* Tlopen/Tgetattr do not follow a final symlink in 9P2000.L; the walk
     * does, so lstat is expressed as "walk without the last element, then
     * ask for the entry itself". */
    return p9_getattr_common(impl, rel, out, prio);
}

static pgw_err p9_getattr(void *impl, const char *rel, pgw_fs_attr *out,
                          uint32_t prio)
{
    return p9_getattr_common(impl, rel, out, prio);
}

static pgw_err p9_setattr(void *impl, const char *rel,
                          const pgw_fs_setattr *sa, uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    uint32_t fid = 0;
    pgw_err err = p9_walk_root(b, rel, &fid, NULL, prio);
    if (err != PGW_OK)
        return err;
    struct pgw_p9_setattr pa;
    fs_setattr_to_p9(sa, &pa);
    err = pgw_p9_setattr(b->session, fid, &pa, prio);
    pgw_p9_clunk(b->session, fid, prio);
    return err;
}

static pgw_err p9_open(void *impl, const char *rel, uint32_t pgw_oflags,
                       uint32_t create_mode, uint64_t *out_handle,
                       uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    if ((pgw_oflags & PGW_O_CREAT) != 0) {
        char name[PGW_NAME_MAX + 1];
        uint32_t dfid = 0;
        pgw_err err = p9_walk_parent(b, rel, &dfid, name, sizeof(name), prio);
        if (err != PGW_OK)
            return err;
        pgw_p9_qid qid;
        err = pgw_p9_lcreate(b->session, dfid, name, pgw_oflags, create_mode,
                             0, &qid, prio);
        if (err != PGW_OK) {
            pgw_p9_clunk(b->session, dfid, prio);
            return err;
        }
        /* Tlcreate turns dfid into the new file's fid. */
        *out_handle = (uint64_t)dfid;
        return PGW_OK;
    }
    uint32_t fid = 0;
    pgw_err err = p9_walk_root(b, rel, &fid, NULL, prio);
    if (err != PGW_OK)
        return err;
    err = pgw_p9_lopen(b->session, fid, pgw_oflags, prio);
    if (err != PGW_OK) {
        pgw_p9_clunk(b->session, fid, prio);
        return err;
    }
    *out_handle = (uint64_t)fid;
    return PGW_OK;
}

static pgw_err p9_close(void *impl, uint64_t handle, uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    return pgw_p9_clunk(b->session, (uint32_t)handle, prio);
}

static pgw_err p9_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                       size_t len, size_t *out_read, uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    return pgw_p9_read(b->session, (uint32_t)handle, off, buf, len, out_read,
                       prio);
}

static pgw_err p9_write(void *impl, uint64_t handle, uint64_t off,
                        const void *buf, size_t len, size_t *out_written,
                        uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    return pgw_p9_write(b->session, (uint32_t)handle, off, buf, len,
                        out_written, prio);
}

static pgw_err p9_readdir(void *impl, uint64_t handle, uint64_t off,
                          pgw_fs_dirent *out, size_t max_entries,
                          size_t *out_count, uint64_t *next_off, uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    pgw_p9_dirent *ents = (pgw_p9_dirent *)calloc(max_entries,
                                                  sizeof(pgw_p9_dirent));
    if (ents == NULL)
        return PGW_ENOMEM;
    pgw_err err = pgw_p9_readdir(b->session, (uint32_t)handle, off, ents,
                                 max_entries, out_count, next_off, prio);
    if (err == PGW_OK) {
        for (size_t i = 0; i < *out_count; i++) {
            out[i].ino = ents[i].qid.path;
            out[i].type = pgw_ftype_from_dt(ents[i].type);
            out[i].next_off = ents[i].offset;
            snprintf(out[i].name, sizeof(out[i].name), "%s", ents[i].name);
        }
    }
    free(ents);
    return err;
}

static pgw_err p9_mkdir(void *impl, const char *rel, uint32_t mode,
                        uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint32_t dfid = 0;
    pgw_err err = p9_walk_parent(b, rel, &dfid, name, sizeof(name), prio);
    if (err != PGW_OK)
        return err;
    err = pgw_p9_mkdir(b->session, dfid, name, mode, 0, NULL, prio);
    pgw_p9_clunk(b->session, dfid, prio);
    return err;
}

static pgw_err p9_unlink(void *impl, const char *rel, bool is_dir,
                         uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint32_t dfid = 0;
    pgw_err err = p9_walk_parent(b, rel, &dfid, name, sizeof(name), prio);
    if (err != PGW_OK)
        return err;
    err = pgw_p9_unlinkat(b->session, dfid, name, is_dir, prio);
    pgw_p9_clunk(b->session, dfid, prio);
    return err;
}

static pgw_err p9_rename(void *impl, const char *oldrel, const char *newrel,
                         uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    char oname[PGW_NAME_MAX + 1];
    char nname[PGW_NAME_MAX + 1];
    uint32_t odfid = 0;
    uint32_t ndfid = 0;
    pgw_err err = p9_walk_parent(b, oldrel, &odfid, oname, sizeof(oname), prio);
    if (err != PGW_OK)
        return err;
    err = p9_walk_parent(b, newrel, &ndfid, nname, sizeof(nname), prio);
    if (err == PGW_OK)
        err = pgw_p9_renameat(b->session, odfid, oname, ndfid, nname, prio);
    pgw_p9_clunk(b->session, odfid, prio);
    if (ndfid != 0)
        pgw_p9_clunk(b->session, ndfid, prio);
    return err;
}

static pgw_err p9_symlink(void *impl, const char *linkrel, const char *target,
                          uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint32_t dfid = 0;
    pgw_err err = p9_walk_parent(b, linkrel, &dfid, name, sizeof(name), prio);
    if (err != PGW_OK)
        return err;
    err = pgw_p9_symlink(b->session, dfid, name, target, 0, NULL, prio);
    pgw_p9_clunk(b->session, dfid, prio);
    return err;
}

static pgw_err p9_readlink(void *impl, const char *linkrel, char *target,
                           size_t cap, uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    uint32_t fid = 0;
    pgw_err err = p9_walk_root(b, linkrel, &fid, NULL, prio);
    if (err != PGW_OK)
        return err;
    err = pgw_p9_readlink(b->session, fid, target, cap, prio);
    pgw_p9_clunk(b->session, fid, prio);
    return err;
}

static pgw_err p9_link(void *impl, const char *existrel, const char *newrel,
                       uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint32_t dfid = 0;
    uint32_t fid = 0;
    pgw_err err = p9_walk_parent(b, newrel, &dfid, name, sizeof(name), prio);
    if (err != PGW_OK)
        return err;
    err = p9_walk_root(b, existrel, &fid, NULL, prio);
    if (err == PGW_OK)
        err = pgw_p9_link(b->session, dfid, fid, name, prio);
    if (fid != 0)
        pgw_p9_clunk(b->session, fid, prio);
    pgw_p9_clunk(b->session, dfid, prio);
    return err;
}

static pgw_err p9_fsync(void *impl, uint64_t handle, uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    return pgw_p9_fsync(b->session, (uint32_t)handle, prio);
}

static pgw_err p9_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio)
{
    p9_backend *b = (p9_backend *)impl;
    struct pgw_p9_statfs st;
    pgw_err err = pgw_p9_statfs(b->session, b->root_fid, &st, prio);
    if (err != PGW_OK)
        return err;
    out->bsize = st.bsize;
    out->namelen = st.namelen;
    out->blocks = st.blocks;
    out->bfree = st.bfree;
    out->bavail = st.bavail;
    out->files = st.files;
    out->ffree = st.ffree;
    return PGW_OK;
}

/* --- public entry point --------------------------------------------------- */

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

pgw_rmfs *pgw_rmfs_from_9p(void *p9_session, const pgw_fs_opts *opts)
{
    if (p9_session == NULL)
        return NULL;
    p9_backend *b = (p9_backend *)calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    b->session = (pgw_p9_session *)p9_session;
    b->root_fid = pgw_p9_session_root_fid(b->session);
    PGW_DBG("rmfs: 9P adapter attached (root fid %u, msize %u)", b->root_fid,
            pgw_p9_session_msize(b->session));
    pgw_rmfs *fs = pgw_rmfs_create(&g_p9_ops, b, opts);
    if (fs == NULL)
        free(b);
    return fs;
}
