/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/rmfs_glue.h - The two hooks the QNX resource-manager glue needs.
 *
 * pgw/rmfs.h is the protocol-neutral contract of layer 3 and stays untouched.
 * Two things it cannot express are needed by a real resource manager, so they
 * live here instead of being forced into the contract:
 *
 *   - the guest process that owns an open descriptor, so that its death can
 *     close everything it held (ROB-02). pgw_fs_cred carries a uid, a gid and
 *     a priority, not a pid.
 *   - an "execution request" bit, so that the `noexec` and `nosuid` mount
 *     options can be enforced (FS-09, SEC-01). It is a guest-only convention
 *     and is never forwarded to a protocol backend.
 *
 * Both are pure additions: no declaration of pgw/rmfs.h changes.
 */
#ifndef PGW_RMFS_GLUE_H
#define PGW_RMFS_GLUE_H

#include "pgw/rmfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open flag requesting an execution mapping of the file (guest only). The
 * protocol translation tables drop it on purpose: it must never reach the
 * server, and an unknown flag must not change the meaning of an open. */
#define PGW_RMFS_O_EXEC 0x01000000u

/* Tag an open descriptor with the guest process that owns it. Call it right
 * after pgw_rmfs_open() from the resmgr connection handler. */
void pgw_rmfs_fd_set_owner(pgw_rmfs_fd *fd, uint32_t owner_pid);

/* Take an extra reference on an open descriptor, as a resource manager does
 * when it hands the same open file to another thread or handler. The backend
 * handle is released only when the last reference is closed (FS-11). */
pgw_err pgw_rmfs_fd_ref(pgw_rmfs_fd *fd);

/* Number of live references on an open descriptor, 0 for NULL. */
uint32_t pgw_rmfs_fd_refcount(const pgw_rmfs_fd *fd);

/* Close every descriptor and drop every lock owned by `owner_pid`: the
 * equivalent of a QNX client-close / process-death handler (ROB-02). */
pgw_err pgw_rmfs_release_pid(pgw_rmfs *fs, uint32_t owner_pid);

#ifdef __cplusplus
}
#endif

#endif /* PGW_RMFS_GLUE_H */
