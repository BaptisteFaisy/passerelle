/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_xlate.c - P9-04: QNX <-> Linux values on the wire.
 *
 * 9P2000.L carries *Linux* numbers: open(2) flags in Tlopen/Tlcreate and errno
 * values in Rlerror. QNX Neutrino numbers differ above 34 (ENOSYS is 38 on
 * Linux, 89 on QNX), so nothing may be passed through unchanged.
 *
 * The tables themselves are not duplicated here: pgw/err.h already owns them
 * (pgw_linux_oflags, pgw_flags_from_host, pgw_code_from_linux,
 * pgw_host_errno) and the project rule is that a mapping exists once. This
 * file is the 9P-specific glue over them, in the two directions the driver
 * needs:
 *
 *   guest open(2) with QNX flags -> our bits -> Linux bits   (request)
 *   Rlerror ecode (Linux)        -> our bits -> QNX errno    (reply)
 *
 * Both are total functions: an unknown bit or an unknown errno never silently
 * becomes success (ROB-01).
 */
#include "p9_priv.h"

#include "pgw/err.h"

uint32_t pgw_p9__wire_flags(uint32_t pgw_flags)
{
    return pgw_linux_oflags(pgw_flags);
}

uint32_t pgw_p9__wire_flags_from_host(uint32_t host_flags)
{
    return pgw_linux_oflags(pgw_flags_from_host(host_flags));
}

pgw_err pgw_p9__err_from_wire(uint32_t linux_errno)
{
    return pgw_code_from_linux(linux_errno);
}

int pgw_p9__host_errno(uint32_t linux_errno)
{
    return pgw_host_errno(pgw_code_from_linux(linux_errno));
}
