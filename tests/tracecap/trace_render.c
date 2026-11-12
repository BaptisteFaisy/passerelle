/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/tracecap/trace_render.c - render a captured exchange as a QEMU trace.
 *
 * The format is the one QEMU writes with -trace 'v9fs_*' and the one the
 * parser of host/bench/trace.py reads:
 *
 *     <pid>@<seconds>.<micros>:v9fs_read tag 5 id 116 fid 2 off 0 max_count 4096
 *
 * Two conventions of QEMU are kept, because the parser relies on them:
 *   - the "id" field of a request is the 9P *request* type, and the "id"
 *     field of an error reply is the reply type that was expected;
 *   - the requests QEMU traces without a reply event (clunk, fsync, flush,
 *     link) repeat their own name on the reply line.
 *
 * Only the facts that do not depend on the machine are rendered: an inode, a
 * uid or a wall-clock time would make the fixture differ from one machine to
 * another, and a fixture that differs is a fixture nobody can trust.
 */
#define _POSIX_C_SOURCE 200809L
#include "trace_render.h"

#include "pgw/limits.h"
#include "pgw/p9.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* The synthetic identity of the rendered trace: a fixed pid and a fixed
 * epoch, so two runs on two machines give the same bytes. */
#define TC_PID 225213u
#define TC_EPOCH_SEC 1794354229u
#define TC_EPOCH_USEC 123456u

/* The rendered clock: a reply 60 us after its request, the next request
 * 500 us after the previous one. A counter, never a clock. */
#define TC_REPLY_GAP_US 60u
#define TC_NEXT_GAP_US 440u

struct msg_info {
    uint8_t type;
    uint8_t reply_type;
    const char *req;
    const char *rpl; /* NULL: QEMU traces the reply with the request name */
};

static const struct msg_info g_msgs[] = {
        {PGW_P9_TVERSION, PGW_P9_RVERSION, "v9fs_version", "v9fs_version_return"},
        {PGW_P9_TATTACH, PGW_P9_RATTACH, "v9fs_attach", "v9fs_attach_return"},
        {PGW_P9_TWALK, PGW_P9_RWALK, "v9fs_walk", "v9fs_walk_return"},
        {PGW_P9_TLOPEN, PGW_P9_RLOPEN, "v9fs_open", "v9fs_open_return"},
        {PGW_P9_TLCREATE, PGW_P9_RLCREATE, "v9fs_lcreate", "v9fs_lcreate_return"},
        {PGW_P9_TREAD, PGW_P9_RREAD, "v9fs_read", "v9fs_read_return"},
        {PGW_P9_TWRITE, PGW_P9_RWRITE, "v9fs_write", "v9fs_write_return"},
        {PGW_P9_TFSYNC, PGW_P9_RFSYNC, "v9fs_fsync", NULL},
        {PGW_P9_TCLUNK, PGW_P9_RCLUNK, "v9fs_clunk", NULL},
        {PGW_P9_TFLUSH, PGW_P9_RFLUSH, "v9fs_flush", NULL},
        {PGW_P9_TGETATTR, PGW_P9_RGETATTR, "v9fs_getattr", "v9fs_getattr_return"},
        {PGW_P9_TSETATTR, PGW_P9_RSETATTR, "v9fs_setattr", "v9fs_setattr_return"},
        {PGW_P9_TREADDIR, PGW_P9_RREADDIR, "v9fs_readdir", "v9fs_readdir_return"},
        {PGW_P9_TSTATFS, PGW_P9_RSTATFS, "v9fs_statfs", "v9fs_statfs_return"},
        {PGW_P9_TMKDIR, PGW_P9_RMKDIR, "v9fs_mkdir", "v9fs_mkdir_return"},
        {PGW_P9_TUNLINKAT, PGW_P9_RUNLINKAT, "v9fs_unlinkat", "v9fs_unlinkat_return"},
        {PGW_P9_TRENAMEAT, PGW_P9_RRENAMEAT, "v9fs_renameat", "v9fs_renameat_return"},
        {PGW_P9_TSYMLINK, PGW_P9_RSYMLINK, "v9fs_symlink", "v9fs_symlink_return"},
        {PGW_P9_TREADLINK, PGW_P9_RREADLINK, "v9fs_readlink", "v9fs_readlink_return"},
        {PGW_P9_TLINK, PGW_P9_RLINK, "v9fs_link", NULL},
};

/* --- a small reader over the request bodies -------------------------------- */

struct rdr {
    const uint8_t *p;
    size_t len;
    size_t off;
    bool bad;
};

static void rd_init(struct rdr *r, const uint8_t *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->off = 0;
    r->bad = false;
}

static uint8_t rd_u8(struct rdr *r)
{
    if (r->off + 1u > r->len) {
        r->bad = true;
        return 0;
    }
    return r->p[r->off++];
}

static uint16_t rd_u16(struct rdr *r)
{
    uint16_t v = rd_u8(r);
    v |= (uint16_t)((uint16_t)rd_u8(r) << 8);
    return v;
}

static uint32_t rd_u32(struct rdr *r)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4u; i++)
        v |= (uint32_t)rd_u8(r) << (8u * i);
    return v;
}

static uint64_t rd_u64(struct rdr *r)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++)
        v |= (uint64_t)rd_u8(r) << (8u * i);
    return v;
}

static void rd_str(struct rdr *r, char *out, size_t cap)
{
    uint16_t n = rd_u16(r);
    if (r->bad || r->off > r->len || (size_t)n > r->len - r->off) {
        r->bad = true;
        n = 0;
    }
    if (cap == 0u)
        return;
    size_t copy = ((size_t)n < cap - 1u) ? (size_t)n : cap - 1u;
    memcpy(out, r->p + r->off, copy);
    out[copy] = '\0';
    r->off += (size_t)n;
}

/* --- the request fields ---------------------------------------------------- */

static void render_meta(const struct msg_info *mi, struct rdr *r, char *out, size_t cap)
{
    switch (mi->type) {
    case PGW_P9_TVERSION: {
        uint32_t msize = rd_u32(r);
        char ver[32];
        rd_str(r, ver, sizeof ver);
        snprintf(out, cap, "msize %" PRIu32 " version %s", msize, ver);
        break;
    }
    case PGW_P9_TATTACH: {
        uint32_t fid = rd_u32(r);
        uint32_t afid = rd_u32(r);
        char uname[32];
        rd_str(r, uname, sizeof uname);
        snprintf(out, cap, "fid %" PRIu32 " afid %" PRId32 " uname %s", fid,
                 (int32_t)afid, uname);
        break;
    }
    case PGW_P9_TWALK: {
        uint32_t fid = rd_u32(r);
        uint32_t newfid = rd_u32(r);
        uint16_t nwname = rd_u16(r);
        snprintf(out, cap, "fid %" PRIu32 " newfid %" PRIu32 " nwnames %" PRIu16, fid,
                 newfid, nwname);
        break;
    }
    case PGW_P9_TLOPEN: {
        uint32_t fid = rd_u32(r);
        uint32_t flags = rd_u32(r);
        snprintf(out, cap, "fid %" PRIu32 " mode %" PRIu32, fid, flags);
        break;
    }
    case PGW_P9_TGETATTR: {
        uint32_t fid = rd_u32(r);
        uint64_t mask = rd_u64(r);
        snprintf(out, cap, "fid %" PRIu32 " request_mask %" PRIu64, fid, mask);
        break;
    }
    default:
        (void)rd_u32(r);
        snprintf(out, cap, "fid %" PRIu32, rd_u32(r));
        break;
    }
}

static void render_io(const struct msg_info *mi, struct rdr *r, char *out, size_t cap)
{
    switch (mi->type) {
    case PGW_P9_TREAD: {
        uint32_t fid = rd_u32(r);
        uint64_t off = rd_u64(r);
        uint32_t count = rd_u32(r);
        snprintf(out, cap, "fid %" PRIu32 " off %" PRIu64 " max_count %" PRIu32, fid, off,
                 count);
        break;
    }
    case PGW_P9_TWRITE: {
        uint32_t fid = rd_u32(r);
        uint64_t off = rd_u64(r);
        uint32_t count = rd_u32(r);
        snprintf(out, cap, "fid %" PRIu32 " off %" PRIu64 " count %" PRIu32, fid, off,
                 count);
        break;
    }
    case PGW_P9_TREADDIR: {
        uint32_t fid = rd_u32(r);
        uint64_t off = rd_u64(r);
        uint32_t count = rd_u32(r);
        snprintf(out, cap, "fid %" PRIu32 " offset %" PRIu64 " max_count %" PRIu32, fid,
                 off, count);
        break;
    }
    default:
        snprintf(out, cap, "fid %" PRIu32, rd_u32(r));
        break;
    }
}

static void render_ns(const struct msg_info *mi, struct rdr *r, char *out, size_t cap)
{
    char name[64];
    switch (mi->type) {
    case PGW_P9_TMKDIR: {
        uint32_t dfid = rd_u32(r);
        rd_str(r, name, sizeof name);
        uint32_t mode = rd_u32(r);
        snprintf(out, cap, "dfid %" PRIu32 " name %s mode %" PRIu32, dfid, name, mode);
        break;
    }
    case PGW_P9_TUNLINKAT: {
        uint32_t dfid = rd_u32(r);
        rd_str(r, name, sizeof name);
        uint32_t flags = rd_u32(r);
        snprintf(out, cap, "dfid %" PRIu32 " name %s flags %" PRIu32, dfid, name, flags);
        break;
    }
    case PGW_P9_TRENAMEAT: {
        uint32_t olddfid = rd_u32(r);
        char oldname[64];
        rd_str(r, oldname, sizeof oldname);
        uint32_t newdfid = rd_u32(r);
        rd_str(r, name, sizeof name);
        snprintf(out, cap,
                 "olddfid %" PRIu32 " oldname %s newdfid %" PRIu32 " newname %s", olddfid,
                 oldname, newdfid, name);
        break;
    }
    default:
        snprintf(out, cap, "fid %" PRIu32, rd_u32(r));
        break;
    }
}

static void render_request(const struct msg_info *mi, const uint8_t *body, size_t len,
                           char *out, size_t cap)
{
    struct rdr r;
    rd_init(&r, body, len);
    out[0] = '\0';
    switch (mi->type) {
    case PGW_P9_TREAD:
    case PGW_P9_TWRITE:
    case PGW_P9_TREADDIR:
        render_io(mi, &r, out, cap);
        break;
    case PGW_P9_TMKDIR:
    case PGW_P9_TUNLINKAT:
    case PGW_P9_TRENAMEAT:
    case PGW_P9_TSYMLINK:
    case PGW_P9_TLCREATE:
    case PGW_P9_TLINK:
        render_ns(mi, &r, out, cap);
        break;
    default:
        render_meta(mi, &r, out, cap);
        break;
    }
}

/* --- the reply fields (decoded by the production decoders) ----------------- */

static void render_qid(const pgw_p9_qid *qid, char *out, size_t cap)
{
    snprintf(out, cap, "type %u version %" PRIu32, (unsigned)qid->type, qid->version);
}

static void render_reply_data(const struct msg_info *mi, const uint8_t *msg, size_t len,
                              uint16_t tag, char *out, size_t cap)
{
    switch (mi->reply_type) {
    case PGW_P9_RVERSION: {
        uint32_t msize = 0;
        char ver[32];
        ver[0] = '\0';
        if (pgw_p9_dec_rversion(msg, len, tag, &msize, ver, sizeof ver) == PGW_OK)
            snprintf(out, cap, "msize %" PRIu32 " version %s", msize, ver);
        break;
    }
    case PGW_P9_RATTACH: {
        pgw_p9_qid qid;
        if (pgw_p9_dec_rattach(msg, len, tag, &qid) == PGW_OK)
            render_qid(&qid, out, cap);
        break;
    }
    case PGW_P9_RWALK: {
        uint16_t nwqid = 0;
        pgw_p9_qid qids[PGW_MAXWELEM];
        if (pgw_p9_dec_rwalk(msg, len, tag, &nwqid, qids, PGW_MAXWELEM) == PGW_OK)
            snprintf(out, cap, "nwnames %" PRIu16, nwqid);
        break;
    }
    case PGW_P9_RLOPEN: {
        pgw_p9_qid qid;
        uint32_t iounit = 0;
        if (pgw_p9_dec_rlopen(msg, len, tag, &qid, &iounit) == PGW_OK)
            snprintf(out, cap, "iounit %" PRIu32, iounit);
        break;
    }
    case PGW_P9_RREAD: {
        uint32_t count = 0;
        const uint8_t *data = NULL;
        if (pgw_p9_dec_rread(msg, len, tag, &count, &data) == PGW_OK)
            snprintf(out, cap, "count %" PRIu32 " err 0", count);
        break;
    }
    case PGW_P9_RWRITE: {
        uint32_t count = 0;
        if (pgw_p9_dec_rwrite(msg, len, tag, &count) == PGW_OK)
            snprintf(out, cap, "total %" PRIu32 " err 0", count);
        break;
    }
    case PGW_P9_RGETATTR: {
        pgw_p9_attr attr;
        if (pgw_p9_dec_rgetattr(msg, len, tag, &attr) == PGW_OK)
            snprintf(out, cap, "getattr={result_mask %" PRIu64 "}", attr.valid);
        break;
    }
    case PGW_P9_RREADDIR: {
        pgw_p9_dirent ents[8];
        size_t nents = 0;
        if (pgw_p9_dec_rreaddir_iter(msg, len, tag, ents, 8, &nents) != PGW_OK)
            break;
        struct rdr r;
        rd_init(&r, msg, len);
        (void)rd_u32(&r); /* size */
        (void)rd_u8(&r);  /* type */
        (void)rd_u16(&r); /* tag */
        uint32_t count = rd_u32(&r);
        snprintf(out, cap, "count %" PRIu32 " retval %" PRIu32, count, count);
        break;
    }
    case PGW_P9_RSTATFS: {
        pgw_p9_statfs_t st;
        if (pgw_p9_dec_rstatfs(msg, len, tag, &st) == PGW_OK)
            snprintf(out, cap, "type 0x%08" PRIx32, st.type);
        break;
    }
    default:
        break;
    }
}

static void render_reply(const struct msg_info *mi, uint8_t rpl_type, const uint8_t *msg,
                         size_t len, uint16_t tag, char *out, size_t cap)
{
    out[0] = '\0';
    if (rpl_type == PGW_P9_RLERROR) {
        uint32_t err = 0;
        if (pgw_p9_dec_rlerror(msg, len, tag, &err) == PGW_OK)
            snprintf(out, cap, "err %" PRIu32, err);
        return;
    }
    render_reply_data(mi, msg, len, tag, out, cap);
}

/* --- the trace itself ------------------------------------------------------ */

static const struct msg_info *find_msg(uint8_t type)
{
    for (size_t i = 0; i < sizeof g_msgs / sizeof g_msgs[0]; i++) {
        if (g_msgs[i].type == type)
            return &g_msgs[i];
    }
    return NULL;
}

static void emit(FILE *out, const char *event, uint16_t tag, uint8_t id,
                 const char *fields, uint64_t *usec)
{
    uint64_t sec = TC_EPOCH_SEC + (*usec / 1000000u);
    uint32_t us = (uint32_t)(*usec % 1000000u);
    fprintf(out, "%u@%" PRIu64 ".%06" PRIu32 ":%s tag %u id %u", TC_PID, sec, us, event,
            (unsigned)tag, (unsigned)id);
    if (fields != NULL && fields[0] != '\0')
        fprintf(out, " %s", fields);
    fputc('\n', out);
}

static void render_pair(FILE *out, const tracecap_frame *req, const tracecap_frame *rpl,
                        uint64_t *usec)
{
    pgw_p9_hdr hdr;
    char fields[192];
    if (pgw_p9_dec_hdr(req->data, req->len, &hdr) != PGW_OK)
        return;
    const struct msg_info *mi = find_msg(hdr.type);
    if (mi == NULL)
        return;
    fields[0] = '\0';
    render_request(mi, req->data + PGW_HEADER_SIZE, req->len - (size_t)PGW_HEADER_SIZE,
                   fields, sizeof fields);
    emit(out, mi->req, hdr.tag, hdr.type, fields, usec);
    *usec += TC_REPLY_GAP_US;
    if (rpl == NULL || !rpl->is_reply)
        return;
    pgw_p9_hdr rhdr;
    if (pgw_p9_dec_hdr(rpl->data, rpl->len, &rhdr) != PGW_OK)
        return;
    render_reply(mi, rhdr.type, rpl->data, rpl->len, rhdr.tag, fields, sizeof fields);
    /* An error reply is traced as v9fs_rerror. Its "id" is the type of the
     * *request*, exactly like in a real QEMU trace: that is what lets the
     * parser pair the error with the request that caused it. */
    bool failed = (rhdr.type == PGW_P9_RLERROR);
    const char *event = failed ? "v9fs_rerror" : (mi->rpl != NULL ? mi->rpl : mi->req);
    emit(out, event, rhdr.tag, hdr.type, fields, usec);
    *usec += TC_NEXT_GAP_US;
}

pgw_err tracecap_render(const tracecap_log *log, FILE *out)
{
    uint64_t usec = TC_EPOCH_USEC;
    for (size_t i = 0; i < log->count; i++) {
        if (log->frames[i].is_reply)
            continue;
        const tracecap_frame *rpl = (i + 1u < log->count) ? &log->frames[i + 1u] : NULL;
        render_pair(out, &log->frames[i], rpl, &usec);
        i++; /* the reply of this request */
    }
    return PGW_OK;
}
