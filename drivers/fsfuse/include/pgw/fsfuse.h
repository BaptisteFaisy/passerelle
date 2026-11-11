/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pgw/fsfuse.h - the virtiofs driver's own glue.
 *
 * Same shape as the 9P driver, but the device is the virtiofs one (virtio
 * device id 26) and the transport has two classes of queues: a *hiprio* queue
 * that carries only the FORGET and INTERRUPT messages, and the request queues
 * that carry everything else (VFS-02).
 *
 * Covered requirements: VFS-01 (device id 26, its tag and its number of
 * request queues), VFS-02 (priority queue for FORGET/INTERRUPT), VFS-04
 * (reference counts), QNX-01, QNX-06, SEC-01.
 */
#ifndef PGW_FSFUSE_H
#define PGW_FSFUSE_H

#include "pgw/drvopts.h"
#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/rmfs.h"
#include "pgw/virtio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The mounted share: the device, its queues and the layer 3 view. */
typedef struct fsfuse_dev {
    pgw_mount_opts opts;
    pgw_vio_dev *vio;     /* layer 1 device (VFS-01) */
    pgw_virtq *hiprio;    /* queue 0: FORGET and INTERRUPT only (VFS-02) */
    pgw_virtq **requests; /* queues 1..n: everything else */
    uint16_t num_requests;/* how many request queues the device exposes */
    pgw_rmfs *fs;         /* layer 3 view (FS-*) */
    int irq;
} fsfuse_dev;

/* --- mount_qnx.c ---------------------------------------------------------- */

/* Parse `-o` options and the positional tag + mount point (QNX-07). */
pgw_err fsfuse_parse_args(int argc, char *argv[], pgw_mount_opts *opts,
                          char *mount_point, size_t cap);

/* Open the device, negotiate FUSE, build the rmfs view, attach and serve. */
pgw_err fsfuse_mount_and_serve(pgw_mount_opts *opts, const char *mount_point);

/* Undo a mount, in the reverse order (VIO-09, VFS-04). Safe to call twice. */
void fsfuse_unmount(void);

/* --- dispatch_qnx.c ------------------------------------------------------ */

/* The backend vtable that layer 3 calls; it forwards to the FUSE session with
 * the caller's priority (QNX-03). */
const pgw_fs_backend_ops *fsfuse_backend_ops(void);

/* The backend instance bound to `s`; NULL when the session is not open. */
void *fsfuse_backend_init(pgw_fuse_session *s);

/* Open the FUSE session on the device's queues (VFS-01, VFS-02). */
pgw_err fsfuse_dispatch_open(fsfuse_dev *dev);
void fsfuse_dispatch_close(void);

/* The session the resource-manager handlers use. */
pgw_fuse_session *fsfuse_session(void);

/* --- privdrop_qnx.c ------------------------------------------------------ */

pgw_err fsfuse_privdrop(void);

#endif /* PGW_FSFUSE_H */
