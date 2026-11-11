/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/fake_server.c - state, tree and transport of the fake server.
 *
 * Owns the simulated tree, the fid table, the reply builders and the entry
 * point used as pgw_p9_rpc_fn. The message handlers themselves are in
 * fake_serve.c; this file is only about state and about rejecting a request
 * that is not exactly one well-formed 9P message.
 */
#include "fake_server.h"

#include "pgw/buf.h"
#include "pgw/limits.h"
#include "pgw/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- tree ---------------------------------------------------------------- */

static bool node_grow(fake_srv *fs, uint32_t node, size_t need)
{
    fake_node *n = &fs->nodes[node];
    if (need <= n->cap)
        return true;
    size_t cap = n->cap != 0 ? n->cap : 64u;
    while (cap < need)
        cap *= 2u;
    uint8_t *p = realloc(n->data, cap);
    if (p == NULL)
        return false;
    n->data = p;
    n->cap = cap;
    return true;
}

uint32_t fake_node_add(fake_srv *fs, uint32_t parent, const char *name,
                       bool is_dir, uint32_t mode)
{
    for (uint32_t i = 0; i < FAKE_NODES; i++) {
        if (fs->nodes[i].used)
            continue;
        fake_node *n = &fs->nodes[i];
        memset(n, 0, sizeof *n);
        n->used = true;
        n->is_dir = is_dir;
        n->parent = parent;
        n->mode = mode;
        n->path = ++fs->next_path;
        n->qt = is_dir ? (uint8_t)PGW_P9_QTDIR : (uint8_t)PGW_P9_QTFILE;
        snprintf(n->name, sizeof n->name, "%s", name);
        return i;
    }
    return UINT32_MAX;
}

int32_t fake_node_find(const fake_srv *fs, uint32_t dir, const char *name,
                       size_t len)
{
    if (len == 0 || len >= FAKE_NAME)
        return -1;
    for (uint32_t i = 0; i < FAKE_NODES; i++) {
        const fake_node *n = &fs->nodes[i];
        if (!n->used || n->parent != dir || i == 0)
            continue;
        if (strlen(n->name) != len)
            continue;
        if (memcmp(n->name, name, len) == 0)
            return (int32_t)i;
    }
    return -1;
}

bool fake_node_store(fake_srv *fs, uint32_t node, size_t off,
                     const uint8_t *data, size_t len)
{
    if (node >= FAKE_NODES || !fs->nodes[node].used)
        return false;
    if (len == 0)
        return true;
    if (!node_grow(fs, node, off + len))
        return false;
    memcpy(fs->nodes[node].data + off, data, len);
    if (off + len > fs->nodes[node].size)
        fs->nodes[node].size = off + len;
    return true;
}

pgw_p9_qid fake_node_qid(const fake_srv *fs, uint32_t node)
{
    pgw_p9_qid q;
    const fake_node *n = &fs->nodes[node];
    q.type = n->qt;
    q.version = 0;
    q.path = n->path;
    return q;
}

void fake_node_attr(const fake_srv *fs, uint32_t node, pgw_p9_attr *out)
{
    const fake_node *n = &fs->nodes[node];
    memset(out, 0, sizeof *out);
    out->valid = PGW_P9_GETATTR_BASIC;
    out->qid = fake_node_qid(fs, node);
    out->mode = n->mode;
    out->nlink = n->is_dir ? 2u : 1u;
    out->size = n->is_dir ? 0u : (uint64_t)n->size;
    out->blksize = 4096u;
    out->blocks = ((uint64_t)n->size + 511u) / 512u;
    out->atime_sec = 1700000000u;
    out->mtime_sec = 1700000001u;
    out->ctime_sec = 1700000002u;
}

/* --- fids ---------------------------------------------------------------- */

bool fake_fid_ok(const fake_srv *fs, uint32_t fid, uint32_t *node)
{
    if (fid >= FAKE_FIDS || !fs->fids[fid].used)
        return false;
    if (node != NULL)
        *node = fs->fids[fid].node;
    return true;
}

bool fake_fid_set(fake_srv *fs, uint32_t fid, uint32_t node, uint32_t flags)
{
    if (fid >= FAKE_FIDS || node >= FAKE_NODES)
        return false;
    fs->fids[fid].used = true;
    fs->fids[fid].node = node;
    fs->fids[fid].flags = flags;
    return true;
}

void fake_fid_clear(fake_srv *fs, uint32_t fid)
{
    if (fid < FAKE_FIDS)
        fs->fids[fid].used = false;
}

size_t fake_open_fids(const fake_srv *fs)
{
    size_t n = 0;
    for (uint32_t i = 0; i < FAKE_FIDS; i++) {
        if (fs->fids[i].used)
            n++;
    }
    return n;
}

/* --- replies ------------------------------------------------------------- */

bool fake_begin(pgw_out *w, void *rx, size_t cap, uint8_t type, uint16_t tag)
{
    if (cap < PGW_HEADER_SIZE)
        return false;
    pgw_out_init(w, rx, cap);
    pgw_put_u32(w, 0);
    pgw_put_u8(w, type);
    pgw_put_u16(w, tag);
    return pgw_out_ok(w);
}

void fake_end(pgw_out *w, size_t *rx_len)
{
    (void)pgw_patch_len_u32(w, 0);
    *rx_len = pgw_written(w);
}

pgw_err fake_reply_error(void *rx, size_t cap, size_t *rx_len, uint16_t tag,
                         uint32_t linux_errno)
{
    pgw_out w;
    if (!fake_begin(&w, rx, cap, PGW_P9_RLERROR, tag))
        return PGW_EINVAL;
    pgw_put_u32(&w, linux_errno);
    if (!pgw_out_ok(&w))
        return PGW_EINVAL;
    fake_end(&w, rx_len);
    return PGW_OK;
}

/* --- tree built for the tests -------------------------------------------- */

static void build_tree(fake_srv *fs)
{
    uint32_t root = fake_node_add(fs, 0, "", true, 040755u);
    fs->nodes[root].parent = root;
    uint32_t hello = fake_node_add(fs, root, "hello.txt", false, 0100644u);
    (void)fake_node_store(fs, hello, 0, (const uint8_t *)"hello\n", 6);
    uint32_t big = fake_node_add(fs, root, "big.bin", false, 0100644u);
    if (node_grow(fs, big, FAKE_BIG_SIZE)) {
        fs->nodes[big].size = FAKE_BIG_SIZE;
        /* Deterministic content, so a test can prove the bytes survived the
         * round trip without keeping a copy (pgw_test_pattern). */
        pgw_test_pattern(fs->nodes[big].data, FAKE_BIG_SIZE, 4242u, 0);
    }
    uint32_t sub = fake_node_add(fs, root, "sub", true, 040755u);
    uint32_t nested = fake_node_add(fs, sub, "nested.txt", false, 0100644u);
    (void)fake_node_store(fs, nested, 0, (const uint8_t *)"nested", 6);
    uint32_t deeper = fake_node_add(fs, sub, "deeper", true, 040755u);
    uint32_t leaf = fake_node_add(fs, deeper, "leaf.txt", false, 0100644u);
    (void)fake_node_store(fs, leaf, 0, (const uint8_t *)"leaf", 4);
    uint32_t many = fake_node_add(fs, root, "many", true, 040755u);
    for (uint32_t i = 0; i < FAKE_MANY_COUNT; i++) {
        char name[FAKE_NAME];
        snprintf(name, sizeof name, "e%03u-padding-0123456789", i);
        (void)fake_node_add(fs, many, name, false, 0100644u);
    }
    uint32_t link = fake_node_add(fs, root, "link", false, 0120777u);
    fs->nodes[link].qt = (uint8_t)PGW_P9_QTSYMLINK;
    snprintf(fs->nodes[link].link, sizeof fs->nodes[link].link, "%s",
             "hello.txt");
}

fake_srv *fake_new(uint32_t msize_cap)
{
    fake_srv *fs = calloc(1, sizeof *fs);
    if (fs == NULL)
        return NULL;
    fs->msize_cap = msize_cap;
    snprintf(fs->version, sizeof fs->version, "%s", "9P2000.L");
    build_tree(fs);
    return fs;
}

void fake_free(fake_srv *fs)
{
    if (fs == NULL)
        return;
    for (uint32_t i = 0; i < FAKE_NODES; i++)
        free(fs->nodes[i].data);
    free(fs);
}

/* --- inspection ---------------------------------------------------------- */

size_t fake_calls(const fake_srv *fs)
{
    return fs != NULL ? fs->calls : 0;
}

size_t fake_calls_of(const fake_srv *fs, uint8_t type)
{
    return fs != NULL ? fs->per_type[type] : 0;
}

void fake_reset_counters(fake_srv *fs)
{
    if (fs == NULL)
        return;
    fs->calls = 0;
    memset(fs->per_type, 0, sizeof fs->per_type);
}

uint32_t fake_max_tag(const fake_srv *fs)
{
    return fs != NULL ? fs->max_tag : 0;
}

size_t fake_read_file(const fake_srv *fs, const char *path, void *buf,
                      size_t cap)
{
    uint32_t cur = 0;
    const char *p = path;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *end = p;
        while (*end != '\0' && *end != '/')
            end++;
        int32_t idx = fake_node_find(fs, cur, p, (size_t)(end - p));
        if (idx < 0)
            return (size_t)-1;
        cur = (uint32_t)idx;
        p = end;
    }
    const fake_node *n = &fs->nodes[cur];
    size_t len = n->size < cap ? n->size : cap;
    if (len > 0 && buf != NULL)
        memcpy(buf, n->data, len);
    return len;
}

/* --- entry point --------------------------------------------------------- */

pgw_err fake_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                 size_t *rx_len, uint32_t prio, uint32_t timeout_ms,
                 void *user)
{
    fake_srv *fs = user;
    (void)prio;
    if (fs == NULL || tx == NULL || rx == NULL || rx_len == NULL)
        return PGW_EINVAL;
    pgw_p9_hdr h;
    pgw_err e = pgw_p9_dec_hdr(tx, tx_len, &h);
    if (e != PGW_OK || (size_t)h.size != tx_len)
        return PGW_EINVAL; /* not exactly one well-formed message */
    fs->calls++;
    fs->per_type[h.type]++;
    fs->last_tag = h.tag;
    fs->last_timeout_ms = timeout_ms;
    if (h.tag != PGW_TAG_NONE && h.tag > fs->max_tag)
        fs->max_tag = h.tag;
    if (fs->hook != NULL)
        fs->hook(fs, h.type, fs->hook_user);
    if (fs->drop_type == h.type) {
        fs->drop_type = 0;
        return PGW_ETIMEDOUT;
    }
    if (fs->fail_type == h.type) {
        fs->fail_type = 0;
        return fake_reply_error(rx, rx_cap, rx_len, h.tag, fs->fail_errno);
    }
    if (fs->wrong_type == h.type) {
        fs->wrong_type = 0;
        pgw_out w;
        if (!fake_begin(&w, rx, rx_cap, fs->wrong_reply, h.tag))
            return PGW_EINVAL;
        fake_end(&w, rx_len);
        return PGW_OK;
    }
    pgw_buf req;
    pgw_buf_body(&req, tx, h.size, PGW_HEADER_SIZE);
    if (!pgw_buf_ok(&req))
        return PGW_EINVAL;
    e = fake_dispatch(fs, &h, &req, rx, rx_cap, rx_len);
    if (e == PGW_OK && pgw_left(&req) != 0)
        return PGW_EINVAL; /* trailing bytes inside a request */
    if (e == PGW_OK && fs->trailing) {
        fs->trailing = false;
        /* One extra byte *inside* the message: size[4] is raised with it, so
         * the reply really does carry a byte no field accounts for. */
        uint8_t *rxb = (uint8_t *)rx;
        rxb[*rx_len] = 0xAA;
        (*rx_len)++;
        rxb[0] = (uint8_t)*rx_len;
        rxb[1] = (uint8_t)(*rx_len >> 8);
        rxb[2] = (uint8_t)(*rx_len >> 16);
        rxb[3] = (uint8_t)(*rx_len >> 24);
    }
    return e;
}
