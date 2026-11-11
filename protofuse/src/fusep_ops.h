/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_ops.h - Shared entry points for the operation files.
 *
 * Implements requirements:
 *   - VFS-03: one helper for the "no body, empty reply" requests, used by
 *     RELEASE, FSYNC, UNLINK, RMDIR and the like (no duplicated prologue)
 */
#ifndef PGW_FUSEP_OPS_H
#define PGW_FUSEP_OPS_H

#include "fusep_reply.h"
#include "fusep_session.h"

#include "pgw/err.h"
#include "pgw/fusep.h"

#include <stdint.h>

/* Submit a request that expects an empty reply (header only). */
pgw_err pgw_fusep_call_empty(pgw_fuse_session *s, const void *tx, int32_t len,
                             uint64_t unique, uint32_t prio);

/* Record a reference for a node returned by LOOKUP, CREATE, MKDIR, SYMLINK or
 * LINK. A nodeid of 0 is a negative entry and is not tracked. */
pgw_err pgw_fusep_track_node(pgw_fuse_session *s, uint64_t nodeid);

#endif /* PGW_FUSEP_OPS_H */
