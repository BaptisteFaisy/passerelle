/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/interop/interop_transport.c - the 9P2000.L framing of a third-party
 * server: the message carries its own size.
 *
 * The size field of a 9P message counts the whole message, the four bytes
 * of the field included, so a reader takes the size first and then the
 * remaining bytes. diod and the Linux kernel 9p client agree on this; the
 * reference server of tests/ref9p/ uses a different transport (a separate
 * length prefix), which is why this file exists next to it.
 *
 * Covers P9-01 and P9-10.
 */
#define _POSIX_C_SOURCE 200809L

#include "interop_transport.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void set_socket_timeout(int fd, uint32_t timeout_ms)
{
    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static pgw_err io_error(void)
{
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? PGW_ETIMEDOUT : PGW_EIO;
}

static pgw_err send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return io_error();
        }
        sent += (size_t)n;
    }
    return PGW_OK;
}

static pgw_err recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0)
            return PGW_EIO; /* the server closed the connection */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return io_error();
        }
        got += (size_t)n;
    }
    return PGW_OK;
}

pgw_err interop_frame(int fd, const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                      size_t *rx_len, uint32_t timeout_ms)
{
    if (fd < 0 || tx == NULL || rx_len == NULL || (rx == NULL && rx_cap > 0))
        return PGW_EINVAL;
    *rx_len = 0;
    set_socket_timeout(fd, timeout_ms != 0 ? timeout_ms : 10000u);
    pgw_err e = send_all(fd, tx, tx_len);
    if (e != PGW_OK)
        return e;
    uint8_t head[4];
    if ((e = recv_all(fd, head, sizeof head)) != PGW_OK)
        return e;
    /* The size field is little endian, as every 9P integer is. */
    uint32_t total = (uint32_t)head[0] | ((uint32_t)head[1] << 8) |
                     ((uint32_t)head[2] << 16) | ((uint32_t)head[3] << 24);
    if (total < sizeof head || (size_t)total > rx_cap)
        return PGW_EMSGSIZE;
    memcpy(rx, head, sizeof head);
    size_t rest = (size_t)total - sizeof head;
    if (rest > 0 && (e = recv_all(fd, (uint8_t *)rx + sizeof head, rest)) != PGW_OK)
        return e;
    *rx_len = (size_t)total;
    return PGW_OK;
}

pgw_err interop_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                    size_t *rx_len, uint32_t prio, uint32_t timeout_ms, void *user)
{
    (void)prio; /* one FIFO connection: the order of the replies is the
                 * order of the requests */
    if (user == NULL)
        return PGW_EINVAL;
    const int *fd = (const int *)user;
    return interop_frame(*fd, tx, tx_len, rx, rx_cap, rx_len, timeout_ms);
}
