/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/include/pgw/cookie.h - the request cookies of both drivers.
 *
 * One virtqueue serves every client of a mount, so every in-flight request needs
 * an identifier the completion can be matched against: the driver hands it to
 * the transport as the cookie and pairs the submission event with the reply event
 * on it (QNX-09). Two threads must never hold the same cookie at the same time,
 * or one thread takes the other's reply - which is the failure
 * drivers/common/src/reqtab_qnx.c exists for, and which a counter cannot fix on
 * its own.
 *
 * The first version was `static uint32_t g_tag_next` with a read-modify-write in
 * the request path. The kernel trace of a 16-reader run showed the same tag twice
 * (measured 14 November 2026, tests/qemu/qnx8/trace_report.py), so the counter
 * was already handing duplicates to concurrent threads. It is atomic here, and it
 * lives in one portable file because both drivers need the same thing.
 *
 * Covered requirements: QNX-05, ROB-05, VIO-02, QNX-09.
 */
#ifndef PGW_COOKIE_H
#define PGW_COOKIE_H

#include <stdatomic.h>
#include <stdint.h>

/* One counter per mount. Value 0 is never handed out: the resource-manager
 * handlers use 0 as "no protocol tag" in the trace, so a cookie of 0 would make
 * a transport request indistinguishable from a handler edge. */
typedef struct pgw_cookie {
    _Atomic uint32_t next;
} pgw_cookie;

#define PGW_COOKIE_INITIALIZER {1u}

/* Draw the next cookie. Wraps to 1 rather than to 0, and never repeats a value
 * still in use in practice: the space is 2^32 and a mount has at most a few
 * hundred requests in flight. */
uint32_t pgw_cookie_next(pgw_cookie *c);

#endif /* PGW_COOKIE_H */
