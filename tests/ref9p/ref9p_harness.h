/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/ref9p_harness.h - glue between the C tests and the Python
 * reference server of this directory.
 *
 * The harness owns the three things both benches need and that must not be
 * written twice:
 *
 *   - starting ref9p_server.py as a child process and learning the port it
 *     chose, through --ready-fd (no polling, no fixed sleep);
 *   - one TCP connection with the 4-byte big-endian length prefix of the
 *     reference transport, used both as a raw frame exchange (the wire
 *     conformance bench) and as the pgw_p9_rpc_fn callback of a session;
 *   - stopping the child, even when a test case abandons itself through
 *     pgw_fail(): every started server is also stopped from an atexit hook.
 *
 * Covers P9-10 (the client is exercised against a second, independent
 * implementation) and P9-09 (the fault-injecting modes of the server).
 */
#ifndef REF9P_HARNESS_H
#define REF9P_HARNESS_H

#include "pgw/err.h"
#include "pgw/p9.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ref9p_server;

/* Start the reference server over `root` (a real directory) and wait for its
 * ready notification. `fault` is "none", "unknown-errno" or "truncate".
 * Returns NULL when the server could not be started. */
struct ref9p_server *ref9p_start(const char *root, const char *fault);

/* Stop the server (SIGTERM then SIGKILL) and free the handle. */
void ref9p_stop(struct ref9p_server *srv);

/* The served directory and the connected socket of a running server. */
const char *ref9p_root(const struct ref9p_server *srv);
int ref9p_fd(const struct ref9p_server *srv);

/* The `user` pointer to hand to pgw_p9_session_open() together with
 * ref9p_rpc(). */
void *ref9p_rpc_user(struct ref9p_server *srv);

/* One framed exchange: send `tx_len` bytes, receive one reply of at most
 * `rx_cap` bytes. Both sides use a 4-byte big-endian length prefix. A silent
 * server yields PGW_ETIMEDOUT, a broken pipe PGW_EIO. */
pgw_err ref9p_frame(int fd, const void *tx, size_t tx_len, void *rx,
                    size_t rx_cap, size_t *rx_len, uint32_t timeout_ms);

/* pgw_p9_rpc_fn over ref9p_frame(); `user` is ref9p_rpc_user(). */
pgw_err ref9p_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                  size_t *rx_len, uint32_t prio, uint32_t timeout_ms,
                  void *user);

#ifdef __cplusplus
}
#endif

#endif /* REF9P_HARNESS_H */
