/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/adapters/fuse_adapter.c - pgw_fs_backend_ops over a FUSE session.
 *
 * Implements requirement VFS-03/VFS-05 from the layer-3 side: every entry of
 * pgw_fs_backend_ops is translated into the corresponding pgw_fuse_* session
 * call, so that virtiofs reuses layer 3 unchanged and passes the same
 * conformance bench as the 9P client (FS-01..FS-15, VFS-05).
 *
 * FUSE is node based: a path is resolved component by component with LOOKUP
 * from the root node, and an open file is identified by the pair
 * (nodeid, fh). The layer-3 handle is an index into a small table holding
 * that pair, which is what makes read/write/release expressible with the
 * single 64-bit handle of the contract.
 *
 * This file is compiled only when the protofuse target exists (see
 * rmfs/CMakeLists.txt): the layer-3 core builds and tests without it.
 */
#include "pgw/rmfs.h"

#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/log.h"
#include "pgw/time.h"

#include "../src/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUSE_MAX_HANDLES 128

typedef struct fuse_handle {
    uint64_t nodeid;
    uint64_t fh;
    int is_dir;
    int used;
} fuse_handle;

typedef struct fuse_backend {
    pgw_fuse_session *session;
    fuse_handle handles[FUSE_MAX_HANDLES];
} fuse_backend;

/* --- conversions ---------------------------------------------------------- */

static void fuse_attr_to_fs(const pgw_fuse_attr *in, pgw_fs_attr *out)
{
    memset(out, 0, sizeof(*out));
    out->type = pgw_ftype_from_linux_mode(in->mode);
    out->mode = in->mode & PGW_S_PERM_MASK;
    out->uid = in->uid;
    out->gid = in->gid;
    out->ino = in->ino;
    out->size = in->size;
    out->nlink = in->nlink;
    out->atime_ns = pgw_ns_join(in->atime, in->atimensec);
    out->mtime_ns = pgw_ns_join(in->mtime, in->mtimensec);
    out->ctime_ns = pgw_ns_join(in->ctime, in->ctimensec);
}

static void fs_setattr_to_fuse(const pgw_fs_setattr *sa,
                               pgw_fuse_setattr_in *out)
{
    memset(out, 0, sizeof(*out));
    if (sa->set_mode) {
        out->valid |= PGW_FUSE_FATTR_MODE;
        out->mode = sa->mode & PGW_S_PERM_MASK;
    }
    if (sa->set_uid) {
        out->valid |= PGW_FUSE_FATTR_UID;
        out->uid = sa->uid;
    }
    if (sa->set_gid) {
        out->valid |= PGW_FUSE_FATTR_GID;
        out->gid = sa->gid;
    }
    if (sa->set_size) {
        out->valid |= PGW_FUSE_FATTR_SIZE;
        out->size = sa->size;
    }
    if (sa->set_atime) {
        out->valid |= PGW_FUSE_FATTR_ATIME;
        pgw_ns_split(sa->atime_ns, &out->atime, &out->atime_nsec);
    }
    if (sa->set_mtime) {
        out->valid |= PGW_FUSE_FATTR_MTIME;
        pgw_ns_split(sa->mtime_ns, &out->mtime, &out->mtime_nsec);
    }
}

/* Resolve a relative path to a node id, one LOOKUP per component (VFS-04:
 * every lookup takes a reference, released by the session on close). */
static pgw_err fuse_resolve(fuse_backend *b, const char *rel, uint64_t *out_id,
                            pgw_fuse_entry *entry, uint32_t prio)
{
    uint64_t cur = PGW_FUSE_ROOT_ID;
    if (rel == NULL || rel[0] == '\0' || strcmp(rel, ".") == 0) {
        *out_id = cur;
        return PGW_OK;
    }
    char work[PGW_PATH_MAX];
    snprintf(work, sizeof(work), "%s", rel);
    char *save = work;
    while (save != NULL && *save != '\0') {
        char *slash = strchr(save, '/');
        if (slash != NULL)
            *slash = '\0';
        if (*save != '\0') {
            pgw_fuse_entry e;
            pgw_err err = pgw_fuse_lookup(b->session, cur, save, &e, prio);
            if (err != PGW_OK)
                return err;
            cur = e.nodeid;
            if (entry != NULL)
                *entry = e;
        }
        save = (slash != NULL) ? slash + 1 : NULL;
    }
    *out_id = cur;
    return PGW_OK;
}

/* Resolve the parent of `rel` and hand back its node id plus the last name. */
static pgw_err fuse_resolve_parent(fuse_backend *b, const char *rel,
                                   uint64_t *out_id, char *name,
                                   size_t name_cap, uint32_t prio)
{
    char dir[PGW_PATH_MAX];
    pgw_err err = rmfs_split_path(rel, dir, sizeof(dir), name, name_cap);
    if (err != PGW_OK)
        return err;
    return fuse_resolve(b, dir, out_id, NULL, prio);
}

/* --- handle table --------------------------------------------------------- */

static uint64_t handle_alloc(fuse_backend *b, uint64_t nodeid, uint64_t fh,
                             int is_dir)
{
    for (int i = 0; i < FUSE_MAX_HANDLES; i++) {
        if (b->handles[i].used)
            continue;
        b->handles[i].used = 1;
        b->handles[i].nodeid = nodeid;
        b->handles[i].fh = fh;
        b->handles[i].is_dir = is_dir;
        return (uint64_t)i + 1u;
    }
    return PGW_FH_NONE;
}

static fuse_handle *handle_get(fuse_backend *b, uint64_t handle)
{
    if (handle == PGW_FH_NONE || handle > FUSE_MAX_HANDLES)
        return NULL;
    fuse_handle *h = &b->handles[handle - 1u];
    return h->used ? h : NULL;
}

/* --- vtable --------------------------------------------------------------- */

static void fuse_destroy(void *impl)
{
    free(impl); /* the session belongs to the caller */
}

static pgw_err fuse_lookup(void *impl, const char *rel, pgw_fs_attr *out,
                           uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    char dir[PGW_PATH_MAX];
    char name[PGW_NAME_MAX + 1];
    pgw_err err = rmfs_split_path(rel, dir, sizeof(dir), name, sizeof(name));
    if (err != PGW_OK)
        return err;
    uint64_t parent = 0;
    err = fuse_resolve(b, dir, &parent, NULL, prio);
    if (err != PGW_OK)
        return err;
    pgw_fuse_entry e;
    err = pgw_fuse_lookup(b->session, parent, name, &e, prio);
    if (err == PGW_OK)
        fuse_attr_to_fs(&e.attr, out);
    return err;
}

static pgw_err fuse_getattr(void *impl, const char *rel, pgw_fs_attr *out,
                            uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    uint64_t nodeid = 0;
    pgw_err err = fuse_resolve(b, rel, &nodeid, NULL, prio);
    if (err != PGW_OK)
        return err;
    pgw_fuse_attr a;
    err = pgw_fuse_getattr(b->session, nodeid, &a, prio);
    if (err == PGW_OK)
        fuse_attr_to_fs(&a, out);
    return err;
}

static pgw_err fuse_setattr(void *impl, const char *rel,
                            const pgw_fs_setattr *sa, uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    uint64_t nodeid = 0;
    pgw_err err = fuse_resolve(b, rel, &nodeid, NULL, prio);
    if (err != PGW_OK)
        return err;
    pgw_fuse_setattr_in fa;
    fs_setattr_to_fuse(sa, &fa);
    pgw_fuse_attr out;
    return pgw_fuse_setattr(b->session, nodeid, &fa, &out, prio);
}

static pgw_err fuse_open(void *impl, const char *rel, uint32_t pgw_oflags,
                         uint32_t create_mode, uint64_t *out_handle,
                         uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    if ((pgw_oflags & PGW_O_CREAT) != 0) {
        char name[PGW_NAME_MAX + 1];
        uint64_t parent = 0;
        pgw_err err = fuse_resolve_parent(b, rel, &parent, name, sizeof(name),
                                          prio);
        if (err != PGW_OK)
            return err;
        pgw_fuse_entry e;
        uint64_t fh = 0;
        err = pgw_fuse_create(b->session, parent, name, pgw_oflags,
                              create_mode, &e, &fh, prio);
        if (err != PGW_OK)
            return err;
        *out_handle = handle_alloc(b, e.nodeid, fh, 0);
        return (*out_handle == PGW_FH_NONE) ? PGW_EMFILE : PGW_OK;
    }
    uint64_t nodeid = 0;
    pgw_err err = fuse_resolve(b, rel, &nodeid, NULL, prio);
    if (err != PGW_OK)
        return err;
    int is_dir = (pgw_oflags & PGW_O_DIRECTORY) != 0;
    uint64_t fh = 0;
    err = pgw_fuse_open(b->session, nodeid, pgw_oflags, is_dir, &fh, prio);
    if (err != PGW_OK)
        return err;
    *out_handle = handle_alloc(b, nodeid, fh, is_dir);
    return (*out_handle == PGW_FH_NONE) ? PGW_EMFILE : PGW_OK;
}

static pgw_err fuse_close(void *impl, uint64_t handle, uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    fuse_handle *h = handle_get(b, handle);
    if (h == NULL)
        return PGW_EBADF;
    h->used = 0;
    return pgw_fuse_release(b->session, h->nodeid, h->fh, h->is_dir != 0, prio);
}

static pgw_err fuse_read(void *impl, uint64_t handle, uint64_t off, void *buf,
                         size_t len, size_t *out_read, uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    fuse_handle *h = handle_get(b, handle);
    if (h == NULL)
        return PGW_EBADF;
    return pgw_fuse_read(b->session, h->nodeid, h->fh, off, buf, len, out_read,
                         prio);
}

static pgw_err fuse_write(void *impl, uint64_t handle, uint64_t off,
                          const void *buf, size_t len, size_t *out_written,
                          uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    fuse_handle *h = handle_get(b, handle);
    if (h == NULL)
        return PGW_EBADF;
    return pgw_fuse_write(b->session, h->nodeid, h->fh, off, buf, len,
                          out_written, prio);
}

static pgw_err fuse_readdir(void *impl, uint64_t handle, uint64_t off,
                            pgw_fs_dirent *out, size_t max_entries,
                            size_t *out_count, uint64_t *next_off,
                            uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    fuse_handle *h = handle_get(b, handle);
    if (h == NULL)
        return PGW_EBADF;
    pgw_fuse_dirent *ents = (pgw_fuse_dirent *)calloc(max_entries,
                                                      sizeof(pgw_fuse_dirent));
    if (ents == NULL)
        return PGW_ENOMEM;
    pgw_err err = pgw_fuse_readdir(b->session, h->nodeid, h->fh, off, ents,
                                   max_entries, out_count, prio);
    *next_off = off;
    if (err == PGW_OK) {
        for (size_t i = 0; i < *out_count; i++) {
            out[i].ino = ents[i].ino;
            out[i].type = pgw_ftype_from_dt((uint8_t)ents[i].type);
            out[i].next_off = ents[i].off;
            snprintf(out[i].name, sizeof(out[i].name), "%s", ents[i].name);
            if (ents[i].off > *next_off)
                *next_off = ents[i].off;
        }
    }
    free(ents);
    return err;
}

static pgw_err fuse_mkdir(void *impl, const char *rel, uint32_t mode,
                          uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint64_t parent = 0;
    pgw_err err = fuse_resolve_parent(b, rel, &parent, name, sizeof(name),
                                      prio);
    if (err != PGW_OK)
        return err;
    pgw_fuse_entry e;
    return pgw_fuse_mkdir(b->session, parent, name, mode, &e, prio);
}

static pgw_err fuse_unlink(void *impl, const char *rel, bool is_dir,
                           uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint64_t parent = 0;
    pgw_err err = fuse_resolve_parent(b, rel, &parent, name, sizeof(name),
                                      prio);
    if (err != PGW_OK)
        return err;
    return pgw_fuse_unlink(b->session, parent, name, is_dir, prio);
}

static pgw_err fuse_rename(void *impl, const char *oldrel, const char *newrel,
                           uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    char oname[PGW_NAME_MAX + 1];
    char nname[PGW_NAME_MAX + 1];
    uint64_t oparent = 0;
    uint64_t nparent = 0;
    pgw_err err = fuse_resolve_parent(b, oldrel, &oparent, oname, sizeof(oname),
                                      prio);
    if (err != PGW_OK)
        return err;
    err = fuse_resolve_parent(b, newrel, &nparent, nname, sizeof(nname), prio);
    if (err != PGW_OK)
        return err;
    return pgw_fuse_rename(b->session, oparent, oname, nparent, nname, prio);
}

static pgw_err fuse_symlink(void *impl, const char *linkrel, const char *target,
                            uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint64_t parent = 0;
    pgw_err err = fuse_resolve_parent(b, linkrel, &parent, name, sizeof(name),
                                      prio);
    if (err != PGW_OK)
        return err;
    pgw_fuse_entry e;
    return pgw_fuse_symlink(b->session, parent, name, target, &e, prio);
}

static pgw_err fuse_readlink(void *impl, const char *linkrel, char *target,
                             size_t cap, uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    uint64_t nodeid = 0;
    pgw_err err = fuse_resolve(b, linkrel, &nodeid, NULL, prio);
    if (err != PGW_OK)
        return err;
    return pgw_fuse_readlink(b->session, nodeid, target, cap, prio);
}

static pgw_err fuse_link(void *impl, const char *existrel, const char *newrel,
                         uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    char name[PGW_NAME_MAX + 1];
    uint64_t parent = 0;
    uint64_t oldnode = 0;
    pgw_err err = fuse_resolve_parent(b, newrel, &parent, name, sizeof(name),
                                      prio);
    if (err != PGW_OK)
        return err;
    err = fuse_resolve(b, existrel, &oldnode, NULL, prio);
    if (err != PGW_OK)
        return err;
    pgw_fuse_entry e;
    return pgw_fuse_link(b->session, parent, oldnode, name, &e, prio);
}

static pgw_err fuse_fsync(void *impl, uint64_t handle, uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    fuse_handle *h = handle_get(b, handle);
    if (h == NULL)
        return PGW_EBADF;
    return pgw_fuse_fsync(b->session, h->nodeid, h->fh, h->is_dir != 0, prio);
}

static pgw_err fuse_statfs(void *impl, pgw_fs_statvfs *out, uint32_t prio)
{
    fuse_backend *b = (fuse_backend *)impl;
    pgw_fuse_kstatfs st;
    pgw_err err = pgw_fuse_statfs(b->session, PGW_FUSE_ROOT_ID, &st, prio);
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

pgw_rmfs *pgw_rmfs_from_fuse(void *fuse_session, const pgw_fs_opts *opts)
{
    if (fuse_session == NULL)
        return NULL;
    fuse_backend *b = (fuse_backend *)calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    b->session = (pgw_fuse_session *)fuse_session;
    PGW_DBG("rmfs: virtiofs adapter attached (%u nodes known)",
            (unsigned)pgw_fuse_ref_count_nodes(b->session));
    pgw_rmfs *fs = pgw_rmfs_create(&g_fuse_ops, b, opts);
    if (fs == NULL)
        free(b);
    return fs;
}
