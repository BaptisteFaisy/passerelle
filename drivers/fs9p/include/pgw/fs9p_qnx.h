/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pgw/fs9p_qnx.h - the QNX-only seam of the 9P driver.
 *
 * Everything declared here needs QNX headers, so it lives apart from
 * pgw/fs9p.h: the portable half of the driver (option parsing, the layer 3
 * backend) must keep compiling on the PC without an SDP. Files that include
 * this header are the `*_qnx.c` set plus `main.c`, and the PC build compiles
 * them against tests/qnx-stub/include (pgw_qnx_syntax).
 *
 * Covered requirements: QNX-01, QNX-03, QNX-05, QNX-08, QNX-09, VIO-04, VIO-09.
 */
#ifndef PGW_FS9P_QNX_H
#define PGW_FS9P_QNX_H

#include "pgw/drvopts.h"
#include "pgw/drvsrv.h"
#include "pgw/err.h"
#include "pgw/fs9p.h"

#include <stdbool.h>
#include <stdint.h>

/* --- irq_qnx.c ----------------------------------------------------------- */

/* Find the interrupt of the device (MSI-X first, INTx fallback, VIO-04),
 * attach an Interrupt Service Thread to it and wire it to the queue. */
pgw_err fs9p_irq_start(fs9p_dev *dev);

/* Detach the interrupt and reset the device, so nothing stays attached after
 * umount (VIO-09). */
void fs9p_irq_stop(fs9p_dev *dev);

/* The INTx line to use, or -1 when the caller must let layer 1 decide. */
int fs9p_irq_override(const pgw_mount_opts *opts);

/* --- dispatch_qnx.c ------------------------------------------------------ */

/* Bring up the single 9P virtqueue and open the layer 2 session on it. */
pgw_err fs9p_dispatch_open(pgw_vio_dev *dev, pgw_virtq *vq, uint32_t timeout_ms);
void fs9p_dispatch_close(void);
/* Interrupt callback of the transport, bumped by the IST. */
void fs9p_xport_irq(void *user);

/* --- trace_qnx.c --------------------------------------------------------- */

/* Switch kernel tracing on for this mount (QNX-09). The message table lives in
 * pgw/fs9p_op.h and the payload packing in pgw/traceop.h, both portable. */
void fs9p_trace_init(const pgw_mount_opts *opts);

/* --- syslog_qnx.c -------------------------------------------------------- */

/* The driver's own slog2 buffer set, and the start-up/shutdown lines that make
 * a run readable in `slog2info` (QNX-08). */
pgw_err fs9p_log_open(void);
void fs9p_log_close(void);
void fs9p_log_mounted(const pgw_mount_opts *opts, uint32_t msize);
void fs9p_log_unmounted(void);

/* --- mount_qnx.c --------------------------------------------------------- */

/* Undo a mount, in the reverse order (VIO-09, P9-07). Safe to call twice. */
void fs9p_unmount(void);

#endif /* PGW_FS9P_QNX_H */
