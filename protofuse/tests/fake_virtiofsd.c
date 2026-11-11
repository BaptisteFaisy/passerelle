/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/fake_virtiofsd.c - Transport half of the in-memory virtiofs
 * server: request parsing, reply framing, fault injection and the two requests
 * that have no reply at all (FORGET and INTERRUPT).
 *
 * Implements requirements:
 *   - VFS-02: records the lane (priority or normal) of every request and the
 *     order in which requests arrived, so the queue discipline is observable
 *   - VFS-03: speaks the FUSE wire protocol in memory, without QEMU
 *   - VFS-04: counts the references the client gives back
 */
#include "fake_virtiofsd_int.h"

#include "pgw/buf.h"
#include "pgw/err.h"

#include <stdbool.h>
#include <string.h>

static void log_op(fake_virtiofsd *f, uint32_t opcode, uint64_t unique,
                   bool hiprio)
{
    if (f->op_log_len < FV_LOG_MAX) {
        f->op_log[f->op_log_len] = opcode;
        f->unique_log[f->op_log_len] = unique;
        f->hiprio_log[f->op_log_len] = hiprio;
        f->op_log_len++;
    }
}

static void count_forget(fake_virtiofsd *f, uint64_t nlookup)
{
    f->forget_msgs++;
    f->forgotten_refs += nlookup;
    f->forgotten_nodes++;
}

/* FORGET and BATCH_FORGET are requests the server never answers. */
static pgw_err handle_forget(fake_virtiofsd *f, uint32_t opcode, pgw_buf *r)
{
    if (opcode == (uint32_t)PGW_FUSE_FORGET) {
        uint64_t nlookup = pgw_get_u64(r);
        if (!pgw_buf_ok(r))
            return PGW_EINVAL;
        count_forget(f, nlookup);
        return PGW_OK;
    }
    uint32_t count = pgw_get_u32(r);
    (void)pgw_get_u32(r); /* dummy */
    if (!pgw_buf_ok(r) || count > 4096u)
        return PGW_EINVAL;
    f->forget_msgs++; /* one BATCH_FORGET message, whatever its node count */
    f->forget_batches++;
    for (uint32_t i = 0; i < count; i++) {
        (void)pgw_get_u64(r); /* nodeid */
        uint64_t nlookup = pgw_get_u64(r);
        if (!pgw_buf_ok(r))
            return PGW_EINVAL;
        f->forgotten_refs += nlookup;
        f->forgotten_nodes++;
    }
    return PGW_OK;
}

static void inject_truncation(void *rx, size_t out_len)
{
    uint8_t *p = (uint8_t *)rx;
    uint32_t fake = (uint32_t)out_len + 8u;
    p[0] = (uint8_t)fake;
    p[1] = (uint8_t)(fake >> 8);
    p[2] = (uint8_t)(fake >> 16);
    p[3] = (uint8_t)(fake >> 24);
}

pgw_err fv_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
               size_t *rx_len, bool hiprio, uint32_t prio, uint32_t timeout_ms,
               void *user)
{
    fake_virtiofsd *f = (fake_virtiofsd *)user;
    (void)prio;
    (void)timeout_ms;
    if (f == NULL || tx == NULL || rx == NULL || rx_len == NULL)
        return PGW_EINVAL;
    *rx_len = 0;

    pgw_buf r;
    pgw_buf_init(&r, tx, tx_len);
    uint32_t len = pgw_get_u32(&r);
    uint32_t opcode = pgw_get_u32(&r);
    uint64_t unique = pgw_get_u64(&r);
    uint64_t nodeid = pgw_get_u64(&r);
    pgw_skip(&r, 16u); /* uid, gid, pid, total_extlen + padding */
    if (!pgw_buf_ok(&r) || (size_t)len != tx_len)
        return PGW_EINVAL;

    f->calls++;
    if (hiprio)
        f->hiprio_calls++;
    else
        f->normal_calls++;
    log_op(f, opcode, unique, hiprio);

    if (opcode == (uint32_t)PGW_FUSE_FORGET ||
        opcode == (uint32_t)PGW_FUSE_BATCH_FORGET)
        return handle_forget(f, opcode, &r);
    if (opcode == (uint32_t)PGW_FUSE_INTERRUPT)
        return PGW_OK; /* no reply, exactly like the kernel expects */

    int32_t injected = f->force_error;
    f->force_error = 0;
    uint64_t echo = f->corrupt_unique ? unique + 1u : unique;
    f->corrupt_unique = false;

    pgw_out w;
    fv_rep_begin(&w, rx, rx_cap, echo, (injected < 0) ? injected : 0);
    if (injected >= 0) {
        int32_t rc = fv_op_dispatch(f, &w, opcode, nodeid, &r);
        if (rc < 0)
            fv_rep_begin(&w, rx, rx_cap, echo, rc); /* discard partial body */
    }
    size_t out_len = fv_rep_finish(&w);
    if (!pgw_out_ok(&w))
        return PGW_ENOSPC;
    if (f->drop_reply) {
        f->drop_reply = false;
        *rx_len = 0;
        return PGW_OK;
    }
    if (f->truncate_reply) {
        f->truncate_reply = false;
        inject_truncation(rx, out_len);
    }
    *rx_len = out_len;
    return PGW_OK;
}
