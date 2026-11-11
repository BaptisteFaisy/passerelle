/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/fake_serve_meta.c - metadata and namespace handlers.
 *
 * Tgetattr, Tsetattr, Tmkdir, Tunlinkat, Trenameat, Tsymlink, Treadlink,
 * Tlink and Tstatfs, against the simulated tree. Kept apart from the data path
 * so that each file stays small; the dispatch table is in fake_dispatch.c.
 */
#include "fake_serve.h"

#include "pgw/limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pgw_err fake_h_getattr(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                         size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    (void)pgw_get_u64(req); /* request_mask */
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    pgw_p9_attr a;
    fake_node_attr(fs, node, &a);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RGETATTR, tag))
        return PGW_EINVAL;
    pgw_put_u64(&w, a.valid);
    pgw_put_u8(&w, a.qid.type);
    pgw_put_u32(&w, a.qid.version);
    pgw_put_u64(&w, a.qid.path);
    pgw_put_u32(&w, a.mode);
    pgw_put_u32(&w, a.uid);
    pgw_put_u32(&w, a.gid);
    pgw_put_u64(&w, a.nlink);
    pgw_put_u64(&w, a.rdev);
    pgw_put_u64(&w, a.size);
    pgw_put_u64(&w, a.blksize);
    pgw_put_u64(&w, a.blocks);
    pgw_put_u64(&w, a.atime_sec);
    pgw_put_u64(&w, a.atime_nsec);
    pgw_put_u64(&w, a.mtime_sec);
    pgw_put_u64(&w, a.mtime_nsec);
    pgw_put_u64(&w, a.ctime_sec);
    pgw_put_u64(&w, a.ctime_nsec);
    pgw_put_u64(&w, 0); /* btime_sec */
    pgw_put_u64(&w, 0); /* btime_nsec */
    pgw_put_u64(&w, 0); /* gen */
    pgw_put_u64(&w, 0); /* data_version */
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_setattr(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                         size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    uint32_t valid = pgw_get_u32(req);
    uint32_t mode = pgw_get_u32(req);
    (void)pgw_get_u32(req); /* uid */
    (void)pgw_get_u32(req); /* gid */
    uint64_t size = pgw_get_u64(req);
    (void)pgw_get_u64(req); /* atime_sec */
    (void)pgw_get_u64(req); /* atime_nsec */
    (void)pgw_get_u64(req); /* mtime_sec */
    (void)pgw_get_u64(req); /* mtime_nsec */
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    if ((valid & PGW_P9_SETATTR_MODE) != 0)
        fs->nodes[node].mode = mode;
    if ((valid & PGW_P9_SETATTR_SIZE) != 0)
        fs->nodes[node].size = (size_t)size;
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RSETATTR, tag))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

pgw_err fake_h_mkdir(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len)
{
    uint32_t dfid = pgw_get_u32(req);
    char name[FAKE_NAME];
    size_t nl = 0;
    if (pgw_get_str(req, name, sizeof name, &nl) != PGW_STR_OK)
        return PGW_EINVAL;
    uint32_t mode = pgw_get_u32(req);
    (void)pgw_get_u32(req); /* gid */
    uint32_t dir = 0;
    if (!fake_fid_ok(fs, dfid, &dir))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    if (fake_node_find(fs, dir, name, nl) >= 0)
        return fake_reply_error(rx, cap, rx_len, tag, L_EEXIST);
    uint32_t node = fake_node_add(fs, dir, name, true, mode);
    if (node == UINT32_MAX)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOSPC);
    return fake_reply_qid(fs, node, PGW_P9_RMKDIR, tag, rx, cap, rx_len);
}

pgw_err fake_h_unlinkat(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                          size_t cap, size_t *rx_len)
{
    uint32_t dfid = pgw_get_u32(req);
    char name[FAKE_NAME];
    size_t nl = 0;
    if (pgw_get_str(req, name, sizeof name, &nl) != PGW_STR_OK)
        return PGW_EINVAL;
    uint32_t flags = pgw_get_u32(req);
    uint32_t dir = 0;
    if (!fake_fid_ok(fs, dfid, &dir))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    int32_t idx = fake_node_find(fs, dir, name, nl);
    if (idx < 0)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOENT);
    bool want_dir = (flags & PGW_P9_AT_REMOVEDIR) != 0;
    if (fs->nodes[idx].is_dir != want_dir)
        return fake_reply_error(rx, cap, rx_len, tag,
                                want_dir ? L_ENOTDIR : L_EISDIR);
    for (uint32_t i = 1; i < FAKE_NODES; i++) {
        if (fs->nodes[i].used && fs->nodes[i].parent == (uint32_t)idx)
            return fake_reply_error(rx, cap, rx_len, tag, L_ENOTEMPTY);
    }
    free(fs->nodes[idx].data);
    memset(&fs->nodes[idx], 0, sizeof fs->nodes[idx]);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RUNLINKAT, tag))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

pgw_err fake_h_renameat(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                          size_t cap, size_t *rx_len)
{
    uint32_t olddfid = pgw_get_u32(req);
    char oldname[FAKE_NAME];
    size_t ol = 0;
    if (pgw_get_str(req, oldname, sizeof oldname, &ol) != PGW_STR_OK)
        return PGW_EINVAL;
    uint32_t newdfid = pgw_get_u32(req);
    char newname[FAKE_NAME];
    size_t nl = 0;
    if (pgw_get_str(req, newname, sizeof newname, &nl) != PGW_STR_OK)
        return PGW_EINVAL;
    uint32_t olddir = 0;
    uint32_t newdir = 0;
    if (!fake_fid_ok(fs, olddfid, &olddir) || !fake_fid_ok(fs, newdfid, &newdir))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    int32_t idx = fake_node_find(fs, olddir, oldname, ol);
    if (idx < 0)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOENT);
    fs->nodes[idx].parent = newdir;
    snprintf(fs->nodes[idx].name, FAKE_NAME, "%.*s", (int)nl, newname);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RRENAMEAT, tag))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

pgw_err fake_h_symlink(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                         size_t cap, size_t *rx_len)
{
    uint32_t dfid = pgw_get_u32(req);
    char name[FAKE_NAME];
    size_t nl = 0;
    if (pgw_get_str(req, name, sizeof name, &nl) != PGW_STR_OK)
        return PGW_EINVAL;
    char target[FAKE_LINK];
    size_t tl = 0;
    if (pgw_get_str(req, target, sizeof target, &tl) != PGW_STR_OK)
        return PGW_EINVAL;
    (void)pgw_get_u32(req); /* gid */
    uint32_t dir = 0;
    if (!fake_fid_ok(fs, dfid, &dir))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    if (fake_node_find(fs, dir, name, nl) >= 0)
        return fake_reply_error(rx, cap, rx_len, tag, L_EEXIST);
    uint32_t node = fake_node_add(fs, dir, name, false, 0120777u);
    if (node == UINT32_MAX)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOSPC);
    fs->nodes[node].qt = (uint8_t)PGW_P9_QTSYMLINK;
    snprintf(fs->nodes[node].link, FAKE_LINK, "%.*s", (int)tl, target);
    return fake_reply_qid(fs, node, PGW_P9_RSYMLINK, tag, rx, cap, rx_len);
}

pgw_err fake_h_readlink(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                          size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    if (fs->nodes[node].qt != (uint8_t)PGW_P9_QTSYMLINK)
        return fake_reply_error(rx, cap, rx_len, tag, L_EINVAL);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RREADLINK, tag))
        return PGW_EINVAL;
    (void)pgw_put_str(&w, fs->nodes[node].link, strlen(fs->nodes[node].link));
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_link(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                      size_t cap, size_t *rx_len)
{
    uint32_t dfid = pgw_get_u32(req);
    uint32_t fid = pgw_get_u32(req);
    char name[FAKE_NAME];
    size_t nl = 0;
    if (pgw_get_str(req, name, sizeof name, &nl) != PGW_STR_OK)
        return PGW_EINVAL;
    uint32_t dir = 0;
    uint32_t node = 0;
    if (!fake_fid_ok(fs, dfid, &dir) || !fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    uint32_t copy = fake_node_add(fs, dir, name, false, fs->nodes[node].mode);
    if (copy == UINT32_MAX)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOSPC);
    if (!fake_node_store(fs, copy, 0, fs->nodes[node].data, fs->nodes[node].size))
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOSPC);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RLINK, tag))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

pgw_err fake_h_statfs(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                        size_t cap, size_t *rx_len)
{
    (void)fs;
    (void)pgw_get_u32(req); /* fid */
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RSTATFS, tag))
        return PGW_EINVAL;
    pgw_put_u32(&w, 0x01021997u); /* type: a made-up fs id */
    pgw_put_u32(&w, 4096u);
    pgw_put_u64(&w, 1000000u);
    pgw_put_u64(&w, 500000u);
    pgw_put_u64(&w, 400000u);
    pgw_put_u64(&w, 2000u);
    pgw_put_u64(&w, 1000u);
    pgw_put_u64(&w, 0x1234u);
    pgw_put_u32(&w, 255u);
    fake_end(&w, rx_len);
    return PGW_OK;
}

/* --- dispatch ------------------------------------------------------------ */
