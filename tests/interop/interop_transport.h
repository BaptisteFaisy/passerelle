/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/interop/interop_transport.h - 9P2000.L over a plain TCP socket.
 *
 * A third-party 9P server (diod) speaks the protocol as the protocol
 * defines it: one message per direction, each message carrying its own
 * four-byte size field. That is not the transport of the reference server
 * of tests/ref9p/ (which prefixes every message with a separate length),
 * so the two cannot share a helper - but the framing rules implemented here
 * are the ones diod and the Linux kernel 9p client both use.
 *
 * Covers P9-01 (the session runs over a third-party transport) and P9-10
 * (the client is exercised against an implementation we did not write).
 */
#ifndef INTEROP_TRANSPORT_H
#define INTEROP_TRANSPORT_H

#include "pgw/err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One 9P message exchange: `tx` is a complete message (size field
 * included), the reply is a complete message too. A silent server yields
 * PGW_ETIMEDOUT, a closed connection PGW_EIO. */
pgw_err interop_frame(int fd, const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                      size_t *rx_len, uint32_t timeout_ms);

/* pgw_p9_rpc_fn over interop_frame(); `user` points to the socket fd. */
pgw_err interop_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                    size_t *rx_len, uint32_t prio, uint32_t timeout_ms, void *user);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_TRANSPORT_H */
