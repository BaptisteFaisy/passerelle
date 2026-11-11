/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_priv.h - internal glue of the 9P2000.L layer.
 *
 * Private to proto9p/: nothing here is part of the contract in pgw/p9.h, so
 * the public API is unchanged. Three kinds of things live here:
 *
 *   - the wire constants the codecs share (header length, per-message
 *     overheads used for chunking, negotiated dialect name);
 *   - two decoders that several public entry points need but that the contract
 *     does not name (Rlcreate/Rsymlink share their layout with Rlopen/Rmkdir);
 *   - the P9-04 translations, which are *not* re-implemented here: they call
 *     the single tables of common/pgw/err.h (no duplicated mapping anywhere).
 */
#ifndef PGW_P9_PRIV_H
#define PGW_P9_PRIV_H

#include "pgw/buf.h"
#include "pgw/p9.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* size[4] type[1] tag[2] */
#define PGW_P9_HDR_LEN 7u

/* Worst case per-message overhead above the payload, rounded up to 8:
 * Twrite is 7 + fid[4] + offset[8] + count[4] = 23, Tread is 7 + 4 + 8 + 4 =
 * 23, and Rread is 7 + count[4] = 11. A payload of `msize - 24` therefore
 * always fits both directions (P9-08). */
#define PGW_P9_IO_OVERHEAD 24u

/* Dialect we speak. A server that answers anything else is unusable. */
#define PGW_P9_VERSION     "9P2000.L"
#define PGW_P9_VERSION_LEN 8u

/* Tags 0..65534: NOTAG (0xFFFF) is reserved for Tversion and is never handed
 * out by the pool. Fids 0..65535: NOFID (0xFFFFFFFF) is never handed out. */
#define PGW_P9_MAX_TAGS 65535u
#define PGW_P9_MAX_FIDS 65536u

/* Concurrent requests a single session can have in flight (P9-05). Each slot
 * owns its request/reply buffers so that a nested or concurrent call never
 * overwrites the bytes of a request still on the wire. */
#define PGW_P9_SLOTS 4u

/* Smallest msize we accept from a server: header plus a usable payload. */
#define PGW_P9_MSIZE_FLOOR 512u

/* --- private decoders (shared bodies, no public API change) --------------- */

/* Validate the header, require `type` and `tag`, and bind a reader to the
 * message body (bytes [7, size)). */
pgw_err pgw_p9__body(const void *msg, size_t len, uint8_t type, uint16_t tag,
                     pgw_buf *r);

/* "Every field was read and nothing is left over" (P9-09). */
pgw_err pgw_p9__done(const pgw_buf *r);

void pgw_p9__get_qid(pgw_buf *r, pgw_p9_qid *q);

/* Decode one readdir record (qid[13] offset[8] type[1] name[s]) in place.
 * False when the record is malformed or the name does not fit `out->name`. */
bool pgw_p9__dirent_next(pgw_buf *r, pgw_p9_dirent *out);

/* Rlopen and Rlcreate: qid[13] iounit[4]. */
pgw_err pgw_p9__dec_qid_iounit(const void *msg, size_t len, uint8_t expect_type,
                               uint16_t tag, pgw_p9_qid *qid, uint32_t *iounit);

/* Rmkdir and Rsymlink: qid[13]. */
pgw_err pgw_p9__dec_qid_only(const void *msg, size_t len, uint8_t expect_type,
                             uint16_t tag, pgw_p9_qid *qid);

/* --- P9-04 translations, built on the single common/ tables --------------- */

/* Our open-flag bits -> Linux (wire) open-flag bits. */
uint32_t pgw_p9__wire_flags(uint32_t pgw_flags);

/* This build's (QNX on target) open-flag bits -> Linux (wire) bits. */
uint32_t pgw_p9__wire_flags_from_host(uint32_t host_flags);

/* Linux (wire) errno -> internal code (unknown numbers stay errors). */
pgw_err pgw_p9__err_from_wire(uint32_t linux_errno);

/* Linux (wire) errno -> errno of this build (QNX numbers on target). */
int pgw_p9__host_errno(uint32_t linux_errno);

#endif /* PGW_P9_PRIV_H */
