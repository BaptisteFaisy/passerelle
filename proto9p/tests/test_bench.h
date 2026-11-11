/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_bench.h - the session bench shared by the session tests.
 *
 * Every session test needs the same three things: a fake server, a session
 * opened on it, and a clean teardown that proves nothing was left open. They
 * live here once so that the tests themselves stay short enough to read.
 */
#ifndef PGW_TEST_BENCH_H
#define PGW_TEST_BENCH_H

#include "fake_server.h"

#include "pgw/p9.h"

#include <stdint.h>

#define BENCH_MSIZE 8192u

/* Open a fake server (accepting at most `server_cap` bytes) and a session. */
fake_srv *bench_up(uint32_t msize_req, uint32_t server_cap);
pgw_p9_session *bench_session(void);
fake_srv *bench_server(void);
void bench_down(void);

/* Walk `path` from the root and return the new fid. */
uint32_t bench_walk(const char *path);

#endif /* PGW_TEST_BENCH_H */
