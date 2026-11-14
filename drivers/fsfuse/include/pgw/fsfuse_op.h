/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fsfuse/include/pgw/fsfuse_op.h - what a FUSE opcode is.
 *
 * One table from a FUSE opcode to the project's operation numbering, outside
 * the QNX half of the driver on purpose: it is a pure mapping, so the PC bench
 * checks every row (drivers/tests/test_traceop.c).
 *
 * Covered requirements: QNX-09 (each request is labelled with the operation it
 * is, not with a default one).
 */
#ifndef PGW_FSFUSE_OP_H
#define PGW_FSFUSE_OP_H

#include "pgw/traceop.h"

#include <stdint.h>

/* The operation a FUSE opcode belongs to. Every opcode answers: one this
 * project does not implement maps to PGW_OP_OTHER, so a request is never
 * missing from the trace and never labelled with an operation it did not
 * perform. */
enum pgw_op fsfuse_op_of(uint32_t opcode);

#endif /* PGW_FSFUSE_OP_H */
