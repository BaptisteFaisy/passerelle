/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pgw/fsfuse_qnx.h - the QNX-only seam of the virtiofs driver.
 *
 * Everything declared here needs QNX headers, so it lives apart from
 * pgw/fsfuse.h: the portable half of the driver (option parsing, the layer 3
 * backend) must keep compiling on the PC without an SDP. Files that include
 * this header are the `*_qnx.c` set plus `main.c`, and the PC build compiles
 * them against tests/qnx-stub/include (pgw_qnx_syntax).
 *
 * Covered requirements: QNX-01, QNX-03, QNX-05, QNX-08, QNX-09, VIO-04, VIO-09,
 * VFS-01, VFS-02.
 */
#ifndef PGW_FSFUSE_QNX_H
#define PGW_FSFUSE_QNX_H

#include "pgw/drvopts.h"
#include "pgw/drvsrv.h"
#include "pgw/err.h"
#include "pgw/fsfuse.h"

#include <stdbool.h>
#include <stdint.h>

/* --- irq_qnx.c ----------------------------------------------------------- */

/* Find the interrupt of the device (MSI-X first, INTx fallback, VIO-04) and
 * attach an Interrupt Service Thread to it. */
pgw_err fsfuse_irq_start(fsfuse_dev *dev);

/* Detach the interrupt and reset the device (VIO-09). */
void fsfuse_irq_stop(fsfuse_dev *dev);

/* The INTx line to use, or -1 when the caller must let layer 1 decide. */
int fsfuse_irq_override(const pgw_mount_opts *opts);

/* --- dispatch_qnx.c ------------------------------------------------------ */

/* Open the FUSE session on the device's queues (VFS-01, VFS-02). */
pgw_err fsfuse_dispatch_open(fsfuse_dev *dev);
void fsfuse_dispatch_close(void);
/* Interrupt callback of the transport, bumped by the IST. */
void fsfuse_xport_irq(void *user);

/* --- trace_qnx.c --------------------------------------------------------- */

/* Switch kernel tracing on for this mount (QNX-09) and label a FUSE message
 * with the operation it belongs to. */
void fsfuse_trace_init(const pgw_mount_opts *opts);
enum pgw_op fsfuse_op_of(uint32_t opcode);

/* --- syslog_qnx.c -------------------------------------------------------- */

/* The driver's own slog2 buffer set and its start-up/shutdown lines (QNX-08). */
pgw_err fsfuse_log_open(void);
void fsfuse_log_close(void);
void fsfuse_log_mounted(const pgw_mount_opts *opts, uint32_t queues);
void fsfuse_log_unmounted(void);

/* --- mount_qnx.c --------------------------------------------------------- */

/* Undo a mount, in the reverse order (VIO-09, VFS-04). Safe to call twice. */
void fsfuse_unmount(void);

#endif /* PGW_FSFUSE_QNX_H */
