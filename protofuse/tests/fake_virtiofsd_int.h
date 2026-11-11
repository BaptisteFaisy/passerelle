/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/fake_virtiofsd_int.h - Internal interface between the fake
 * server's transport file and its operation file.
 *
 * Implements requirements:
 *   - VFS-03: reply builders shared by every simulated operation
 */
#ifndef PGW_FAKE_VIRTIOFSD_INT_H
#define PGW_FAKE_VIRTIOFSD_INT_H

#include "fake_virtiofsd.h"

#include "pgw/buf.h"

#include <stddef.h>
#include <stdint.h>

/* Linux errno value as it travels on the wire, never the host numbering. */
#define FV_ERR(code) (-(int32_t)pgw_linux_errno(code))

void fv_rep_begin(pgw_out *w, void *buf, size_t cap, uint64_t unique,
                  int32_t error);
size_t fv_rep_finish(pgw_out *w);
void fv_put_attr(pgw_out *w, const fv_node *n);
void fv_put_entry(pgw_out *w, const fv_node *n);
void fv_put_open_out(pgw_out *w, uint64_t fh);

/* Run one operation. Returns 0 on success, or a negative Linux errno; on error
 * the reply body written so far is discarded by the caller. */
/* Data-path operations, implemented in fake_virtiofsd_io.c. */
int32_t op_statfs(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid, pgw_buf *r);
int32_t op_read(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid, pgw_buf *r);
int32_t op_write(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid, pgw_buf *r);
int32_t op_readdir(fake_virtiofsd *f, pgw_out *w, uint64_t nodeid, pgw_buf *r);

int32_t fv_op_dispatch(fake_virtiofsd *f, pgw_out *w, uint32_t opcode,
                       uint64_t nodeid, pgw_buf *r);

#endif /* PGW_FAKE_VIRTIOFSD_INT_H */
