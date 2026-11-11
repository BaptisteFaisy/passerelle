/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_internal.h - Shared helpers for the FUSE layer.
 *
 * Implements requirements:
 *   - VFS-03: one request-header writer and one reply prologue validator used
 *     by every encoder and decoder (no duplication between translation units)
 *   - ROB-06: the prologue validator is the single place where a reply's
 *     length, `unique` tag and error field are checked against the request
 *   - VFS-04: attribute decoding is shared by LOOKUP, GETATTR and CREATE
 *
 * Internal to protofuse. Tests include it to drive the internals directly.
 */
#ifndef PGW_FUSEP_INTERNAL_H
#define PGW_FUSEP_INTERNAL_H

#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/fusep.h"

#include "fusep_abi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Error policy of this layer:
 *   - PGW_EINVAL: the caller handed us something impossible (NULL buffer,
 *     oversized name, zero-capacity destination);
 *   - PGW_EIO: the server sent something we refuse to trust (short header,
 *     inconsistent internal lengths, trailing bytes, mismatched `unique`).
 * Both are documented in the header comments of the public decoders. */
#define PGW_FUSEP_E_BAD_MSG PGW_EIO

/* Write the 40-byte FUSE input header. `len` stays zero until enc_end().
 * uid/gid/pid are written as zero: the guest kernel is the one that knows the
 * caller's credentials, and virtiofsd ignores them for a shared directory. */
static inline void pgw_fusep_put_in_hdr(pgw_out *w, uint32_t opcode,
                                        uint64_t unique, uint64_t nodeid)
{
    pgw_put_u32(w, 0); /* len placeholder, patched at offset 0 */
    pgw_put_u32(w, opcode);
    pgw_put_u64(w, unique);
    pgw_put_u64(w, nodeid);
    pgw_put_u32(w, 0); /* uid */
    pgw_put_u32(w, 0); /* gid */
    pgw_put_u32(w, 0); /* pid */
    pgw_put_u32(w, 0); /* total_extlen (u16) then padding (u16) */
}

/* Begin an encoder: bind the cursor and emit the input header. */
static inline void pgw_fusep_enc_begin(pgw_out *w, void *buf, size_t cap,
                                       uint32_t opcode, uint64_t unique,
                                       uint64_t nodeid)
{
    pgw_out_init(w, buf, cap);
    pgw_fusep_put_in_hdr(w, opcode, unique, nodeid);
}

/* Finish an encoder: patch len[4]. Returns the message length, or -1. */
static inline int32_t pgw_fusep_enc_end(pgw_out *w)
{
    if (!pgw_patch_len_u32(w, 0) || !pgw_out_ok(w))
        return -1;
    size_t n = pgw_written(w);
    if (n > 0x7FFFFFFFu)
        return -1;
    return (int32_t)n;
}

/* Wire length of a NUL-terminated FUSE name, including the terminating NUL.
 * Returns 0 when the name is missing or too long for the protocol. */
static inline size_t pgw_fusep_name_wire_len(const char *name)
{
    if (name == NULL)
        return 0;
    size_t n = strlen(name) + 1u;
    return (n <= (size_t)PGW_FUSE_NAME_WIRE_MAX + 1u) ? n : 0;
}

/* Decode the 16-byte reply header and validate it against `unique`.
 *
 * On PGW_OK the body cursor is bound to bytes [16, hdr.len) of `msg`, so that
 * every decoder sees exactly the declared message and a virtio used-ring length
 * larger than hdr.len is ignored instead of being mistaken for payload.
 *
 * A negative error field is translated to the matching pgw_err and returned;
 * the body is then not touched, because an error reply has no body. */
static inline pgw_err pgw_fusep_dec_head(const void *msg, size_t len,
                                         uint64_t unique, pgw_buf *body)
{
    pgw_fuse_out_hdr h;
    pgw_err rc = pgw_fuse_dec_hdr(msg, len, &h);
    if (rc != PGW_OK)
        return rc;
    if (h.unique != unique)
        return PGW_FUSEP_E_BAD_MSG;
    if (h.error > 0)
        return PGW_FUSEP_E_BAD_MSG; /* must be 0 or a negative Linux errno */
    if (h.error < 0) {
        uint32_t linux_no = (uint32_t)(-h.error);
        return pgw_code_from_linux(linux_no);
    }
    pgw_buf_body(body, msg, h.len, PGW_FUSE_OUT_HDR_SZ);
    return PGW_OK;
}

/* Decode one fuse_attr of `avail` bytes. Accepts the three widths that exist
 * in the wild: 80 (pre-7.9, no blksize), 84 (no flags word) and 88 (7.32+). */
static inline pgw_err pgw_fusep_dec_attr_bytes(pgw_buf *r, size_t avail,
                                               pgw_fuse_attr *out)
{
    if (out == NULL)
        return PGW_EINVAL;
    if (avail != PGW_FUSE_ATTR_SIZE_COMPAT && avail != PGW_FUSE_ATTR_SIZE &&
        avail != PGW_FUSE_ATTR_SIZE_FLAGS)
        return PGW_FUSEP_E_BAD_MSG;
    out->ino = pgw_get_u64(r);
    out->size = pgw_get_u64(r);
    out->blocks = pgw_get_u64(r);
    out->atime = pgw_get_u64(r);
    out->mtime = pgw_get_u64(r);
    out->ctime = pgw_get_u64(r);
    out->atimensec = pgw_get_u32(r);
    out->mtimensec = pgw_get_u32(r);
    out->ctimensec = pgw_get_u32(r);
    out->mode = pgw_get_u32(r);
    out->nlink = pgw_get_u32(r);
    out->uid = pgw_get_u32(r);
    out->gid = pgw_get_u32(r);
    out->rdev = pgw_get_u32(r);
    out->blksize = (avail >= PGW_FUSE_ATTR_SIZE) ? pgw_get_u32(r) : 0u;
    if (avail >= PGW_FUSE_ATTR_SIZE_FLAGS)
        (void)pgw_get_u32(r); /* attr flags, not modelled */
    return pgw_buf_ok(r) ? PGW_OK : PGW_FUSEP_E_BAD_MSG;
}

/* 8-byte record alignment used by fuse_dirent. */
static inline size_t pgw_fusep_align8(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}

#endif /* PGW_FUSEP_INTERNAL_H */
