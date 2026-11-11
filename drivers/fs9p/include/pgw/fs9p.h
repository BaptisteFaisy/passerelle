/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pgw/fs9p.h - the 9P2000.L driver's own glue: options, transport wiring and
 * the QNX entry points.
 *
 * Layer 1 (virtio) and layer 2 (9P2000.L) are portable libraries; this header
 * is the thin seam between them and the QNX resource manager. It declares only
 * what the driver files exchange, and it is compiled on the PC against
 * tests/qnx-stub so the seam is checked without an SDP.
 *
 * Covered requirements: QNX-01 (resource manager), QNX-07 (mount/umount),
 * P9-01 (msize negotiation), VIO-05 (device chosen by mount tag), ROB-05
 * (request timeout), QNX-06 / SEC-01 (privilege drop).
 */
#ifndef PGW_FS9P_H
#define PGW_FS9P_H

#include "pgw/drvopts.h"
#include "pgw/err.h"
#include "pgw/p9.h"
#include "pgw/rmfs.h"
#include "pgw/virtio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The 9P session, the device it runs on and the mount options, as one object:
 * the driver owns exactly one of these per process. */
typedef struct fs9p_dev {
    pgw_mount_opts opts;
    pgw_vio_dev *vio;   /* layer 1 device (VIO-01..VIO-05) */
    pgw_virtq *queue;   /* the single 9P virtqueue */
    pgw_rmfs *fs;       /* layer 3 view (FS-*) */
    int irq;            /* line the interrupt service thread waits on */
} fs9p_dev;

/* --- mount_qnx.c ---------------------------------------------------------- */

/* Parse `-o` options and the positional tag + mount point (QNX-07: the same
 * command line mount(8) hands over). */
pgw_err fs9p_parse_args(int argc, char *argv[], pgw_mount_opts *opts,
                        char *mount_point, size_t cap);

/* Open the device, negotiate 9P, build the rmfs view, attach and serve.
 * Returns only on failure. */
pgw_err fs9p_mount_and_serve(pgw_mount_opts *opts, const char *mount_point);

/* The backend instance bound to `s`; NULL when the session is not open. */
void *fs9p_backend_init(pgw_p9_session *s);

/* --- dispatch_qnx.c ------------------------------------------------------ */

/* The 9P session used by the resource manager handlers, once it is open. */
pgw_p9_session *fs9p_session(void);

/* The backend vtable that layer 3 calls; it forwards to the 9P session with the
 * caller's priority (QNX-03) and splits large transfers (P9-08). */
const pgw_fs_backend_ops *fs9p_backend_ops(void);

/* --- privdrop_qnx.c ------------------------------------------------------ */

/* The abilities the 9P driver needs, and the drop itself (QNX-06, SEC-01). */
pgw_err fs9p_privdrop(void);

#endif /* PGW_FS9P_H */
