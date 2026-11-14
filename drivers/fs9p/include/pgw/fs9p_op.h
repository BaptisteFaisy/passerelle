/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/fs9p/include/pgw/fs9p_op.h - what a 9P2000.L message is.
 *
 * One table from a wire message type to the project's operation numbering. It
 * lives outside the QNX half of the driver because it is a pure mapping: the PC
 * bench checks every row of it (drivers/tests/test_traceop.c), which is the only
 * place a wrong trace label can be caught before a guest runs.
 *
 * Covered requirements: QNX-09 (each request is labelled with the operation it
 * is, not with a default one).
 */
#ifndef PGW_FS9P_OP_H
#define PGW_FS9P_OP_H

#include "pgw/traceop.h"

#include <stdint.h>

/* The operation a 9P2000.L message belongs to. Every message type answers: a
 * type the project does not implement maps to PGW_OP_OTHER, so a request is
 * never missing from the trace and never labelled with an operation it did not
 * perform. */
enum pgw_op fs9p_op_of(uint8_t p9_type);

#endif /* PGW_FS9P_OP_H */
