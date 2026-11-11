/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/fake_serve.c - version, attach, walk and the data path.
 *
 * The handlers of the fake server; the metadata half is in fake_serve_meta.c
 * and the dispatch table in fake_dispatch.c. Each handler only touches the
 * simulated tree: no file system, no clock, no socket, which is what makes the
 * session tests deterministic on the PC (P9-10).
 *
 * Error replies carry *Linux* errno numbers, exactly like a real 9P2000.L
 * server, so the Linux -> QNX translation of P9-04 is exercised for real.
 */
#include "fake_serve.h"

#include "pgw/limits.h"

#include <stdio.h>
#include <string.h>

uint8_t fake_dirent_type(const fake_node *n)
{
    if (n->is_dir)
        return (uint8_t)PGW_DT_DIR;
    if (n->qt == (uint8_t)PGW_P9_QTSYMLINK)
        return (uint8_t)PGW_DT_LNK;
    return (uint8_t)PGW_DT_REG;
}

pgw_err fake_reply_qid(fake_srv *fs, uint32_t node, uint8_t type, uint16_t tag,
                       void *rx, size_t cap, size_t *rx_len)
{
    pgw_out w;
    if (!fake_begin(&w, rx, cap, type, tag))
        return PGW_EINVAL;
    pgw_p9_qid q = fake_node_qid(fs, node);
    pgw_put_u8(&w, q.type);
    pgw_put_u32(&w, q.version);
    pgw_put_u64(&w, q.path);
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_version(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                         size_t cap, size_t *rx_len)
{
    uint32_t msize = pgw_get_u32(req);
    char ver[32];
    size_t vlen = 0;
    if (pgw_get_str(req, ver, sizeof ver, &vlen) != PGW_STR_OK)
        return PGW_EINVAL;
    uint32_t got = msize < fs->msize_cap ? msize : fs->msize_cap;
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RVERSION, tag))
        return PGW_EINVAL;
    pgw_put_u32(&w, got);
    (void)pgw_put_str(&w, fs->version, strlen(fs->version));
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_attach(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                        size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    (void)pgw_get_u32(req); /* afid: we accept NOFID only */
    char uname[64];
    char aname[256];
    size_t len = 0;
    if (pgw_get_str(req, uname, sizeof uname, &len) != PGW_STR_OK)
        return PGW_EINVAL;
    if (pgw_get_str(req, aname, sizeof aname, &len) != PGW_STR_OK)
        return PGW_EINVAL;
    (void)pgw_get_u32(req); /* n_uname */
    if (!fake_fid_set(fs, fid, 0, 0))
        return fake_reply_error(rx, cap, rx_len, tag, L_EMFILE);
    return fake_reply_qid(fs, 0, PGW_P9_RATTACH, tag, rx, cap, rx_len);
}

pgw_err fake_h_walk(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                      size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    uint32_t newfid = pgw_get_u32(req);
    uint16_t n = pgw_get_u16(req);
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    if (n > (uint16_t)PGW_MAXWELEM)
        return fake_reply_error(rx, cap, rx_len, tag, L_EINVAL);
    /* Every name is read first: a request must be consumed in full even when
     * the walk stops early (the client sends them all). */
    char names[PGW_MAXWELEM][FAKE_NAME];
    size_t lens[PGW_MAXWELEM];
    for (uint16_t i = 0; i < n; i++) {
        if (pgw_get_str(req, names[i], sizeof names[i], &lens[i]) != PGW_STR_OK)
            return PGW_EINVAL;
    }
    pgw_p9_qid qids[PGW_MAXWELEM];
    uint16_t walked = 0;
    uint32_t cur = node;
    for (uint16_t i = 0; i < n; i++) {
        if (!fs->nodes[cur].is_dir)
            break;
        int32_t idx = fake_node_find(fs, cur, names[i], lens[i]);
        if (idx < 0)
            break;
        cur = (uint32_t)idx;
        qids[walked++] = fake_node_qid(fs, cur);
        if (fs->nodes[cur].qt == (uint8_t)PGW_P9_QTSYMLINK)
            break; /* a server does not cross a symlink during walk */
    }
    /* 9P: newfid only exists when every name was walked. */
    if (walked == n && !fake_fid_set(fs, newfid, cur, 0))
        return fake_reply_error(rx, cap, rx_len, tag, L_EMFILE);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RWALK, tag))
        return PGW_EINVAL;
    pgw_put_u16(&w, walked);
    for (uint16_t i = 0; i < walked; i++) {
        pgw_put_u8(&w, qids[i].type);
        pgw_put_u32(&w, qids[i].version);
        pgw_put_u64(&w, qids[i].path);
    }
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_clunk(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    fake_fid_clear(fs, fid);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RCLUNK, tag))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

pgw_err fake_h_flush(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len)
{
    (void)fs;
    (void)pgw_get_u16(req); /* oldtag */
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RFLUSH, tag))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

/* --- data path ----------------------------------------------------------- */

pgw_err fake_h_lopen(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    uint32_t flags = pgw_get_u32(req);
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    fs->fids[fid].flags = flags;
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RLOPEN, tag))
        return PGW_EINVAL;
    pgw_p9_qid q = fake_node_qid(fs, node);
    pgw_put_u8(&w, q.type);
    pgw_put_u32(&w, q.version);
    pgw_put_u64(&w, q.path);
    pgw_put_u32(&w, 0); /* iounit: the client uses msize */
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_lcreate(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                         size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    char name[FAKE_NAME];
    size_t nl = 0;
    if (pgw_get_str(req, name, sizeof name, &nl) != PGW_STR_OK)
        return PGW_EINVAL;
    uint32_t flags = pgw_get_u32(req);
    uint32_t mode = pgw_get_u32(req);
    (void)pgw_get_u32(req); /* gid */
    uint32_t dir = 0;
    if (!fake_fid_ok(fs, fid, &dir))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    if (!fs->nodes[dir].is_dir)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOTDIR);
    if (fake_node_find(fs, dir, name, nl) >= 0)
        return fake_reply_error(rx, cap, rx_len, tag, L_EEXIST);
    uint32_t node = fake_node_add(fs, dir, name, false, mode);
    if (node == UINT32_MAX)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOSPC);
    /* Tlcreate turns fid into the new file. */
    fs->fids[fid].node = node;
    fs->fids[fid].flags = flags;
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RLCREATE, tag))
        return PGW_EINVAL;
    pgw_p9_qid q = fake_node_qid(fs, node);
    pgw_put_u8(&w, q.type);
    pgw_put_u32(&w, q.version);
    pgw_put_u64(&w, q.path);
    pgw_put_u32(&w, 0);
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_read(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                      size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    uint64_t off = pgw_get_u64(req);
    uint32_t count = pgw_get_u32(req);
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    const fake_node *n = &fs->nodes[node];
    if (n->is_dir)
        return fake_reply_error(rx, cap, rx_len, tag, L_EISDIR);
    size_t avail = (off < (uint64_t)n->size) ? n->size - (size_t)off : 0;
    uint32_t got = (uint32_t)(avail < (size_t)count ? avail : (size_t)count);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RREAD, tag))
        return PGW_EINVAL;
    pgw_put_u32(&w, got);
    if (got > 0)
        (void)pgw_put_data(&w, n->data + (size_t)off, got);
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

pgw_err fake_h_write(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    uint64_t off = pgw_get_u64(req);
    uint32_t count = pgw_get_u32(req);
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    const uint8_t *data = pgw_take(req, count);
    if (data == NULL)
        return PGW_EINVAL;
    if (off > (uint64_t)SIZE_MAX ||
        !fake_node_store(fs, node, (size_t)off, data, count))
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOSPC);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RWRITE, tag))
        return PGW_EINVAL;
    pgw_put_u32(&w, count);
    fake_end(&w, rx_len);
    return PGW_OK;
}

pgw_err fake_h_fsync(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len)
{
    (void)fs;
    (void)pgw_get_u32(req); /* fid */
    (void)pgw_get_u32(req); /* datasync */
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RFSYNC, tag))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

/* One readdir record: qid[13] offset[8] type[1] name[s]. */
static void put_dirent(pgw_out *w, const fake_node *n, uint64_t cookie)
{
    pgw_put_u8(w, n->qt);
    pgw_put_u32(w, 0);
    pgw_put_u64(w, n->path);
    pgw_put_u64(w, cookie);
    pgw_put_u8(w, fake_dirent_type(n));
    (void)pgw_put_str(w, n->name, strlen(n->name));
}

pgw_err fake_h_readdir(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                         size_t cap, size_t *rx_len)
{
    uint32_t fid = pgw_get_u32(req);
    uint64_t off = pgw_get_u64(req);
    uint32_t count = pgw_get_u32(req);
    uint32_t node = 0;
    if (!fake_fid_ok(fs, fid, &node))
        return fake_reply_error(rx, cap, rx_len, tag, L_EBADF);
    if (!fs->nodes[node].is_dir)
        return fake_reply_error(rx, cap, rx_len, tag, L_ENOTDIR);
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RREADDIR, tag))
        return PGW_EINVAL;
    size_t count_off = pgw_written(&w);
    pgw_put_u32(&w, 0);
    size_t payload = 0;
    uint64_t pos = 0;
    for (uint32_t i = 0; i < FAKE_NODES; i++) {
        const fake_node *n = &fs->nodes[i];
        if (!n->used || n->parent != node || i == 0)
            continue;
        if (pos++ < off)
            continue; /* already handed out in an earlier reply */
        size_t reclen = 13u + 8u + 1u + 2u + strlen(n->name);
        if (payload + reclen > (size_t)count)
            break; /* stop here: the client resumes from the last cookie */
        put_dirent(&w, n, pos);
        payload += reclen;
    }
    uint8_t *p = pgw_out_at(&w, count_off);
    if (p == NULL)
        return PGW_EINVAL;
    p[0] = (uint8_t)payload;
    p[1] = (uint8_t)(payload >> 8);
    p[2] = (uint8_t)(payload >> 16);
    p[3] = (uint8_t)(payload >> 24);
    fake_end(&w, rx_len);
    return pgw_out_ok(&w) ? PGW_OK : PGW_EINVAL;
}

/* --- metadata ------------------------------------------------------------ */
