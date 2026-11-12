/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/ref9p_harness.c - start, talk to and stop ref9p_server.py.
 *
 * The transport is the one the reference server speaks: one 9P message per
 * frame, a frame being a 4-byte big-endian length followed by the message
 * itself. Nothing here knows about 9P semantics, so the same helpers serve the
 * session bench (through the pgw_p9_rpc_fn callback) and the wire-format bench
 * (which sends messages it builds by hand and inspects the reply bytes).
 *
 * The child is started with its stdout and stderr on /dev/null and with
 * PYTHONDONTWRITEBYTECODE, so a server that outlives a test case can neither
 * pollute the TAP output, nor hold the harness' output pipe open, nor leave
 * __pycache__ behind in the source tree.
 *
 * Covers P9-10 and the validation path of P9-09.
 */
#define _POSIX_C_SOURCE 200809L

#include "ref9p_harness.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REF9P_READY_TIMEOUT_MS 15000
#define REF9P_MAX_SERVERS 8
#define REF9P_PATH_MAX 512
#define READY_FD 3 /* the descriptor the child writes its port to */

struct ref9p_server {
    pid_t pid;
    int fd;
    int port;
    char root[REF9P_PATH_MAX];
};

static struct ref9p_server *g_servers[REF9P_MAX_SERVERS];
static size_t g_nservers;
static bool g_atexit_registered;

#ifndef PGW_REF9P_PYTHON
#define PGW_REF9P_PYTHON ""
#endif
#ifndef PGW_REF9P_SERVER
#define PGW_REF9P_SERVER ""
#endif

/* --- locating python3 and the server script -------------------------------- */

static const char *python_bin(void)
{
    const char *env = getenv("PGW_REF9P_PYTHON");
    if (env != NULL && env[0] != '\0')
        return env;
    if (PGW_REF9P_PYTHON[0] != '\0')
        return PGW_REF9P_PYTHON;
    return "python3"; /* resolved through PATH by execvp() */
}

/* Candidates for a hand-built compile, where CMake gave us no path: the
 * repository root, this directory and the usual build-directory depths. */
static const char *const g_candidates[] = {
    PGW_REF9P_SERVER,      "tests/ref9p/ref9p_server.py",
    "ref9p_server.py",     "../tests/ref9p/ref9p_server.py",
    "../../tests/ref9p/ref9p_server.py",
    "../../../tests/ref9p/ref9p_server.py",
};

static char g_script[REF9P_PATH_MAX];

static const char *server_script(void)
{
    const char *env = getenv("PGW_REF9P_SERVER");
    if (env != NULL && env[0] != '\0')
        return env;
    for (size_t i = 0; i < sizeof g_candidates / sizeof g_candidates[0]; i++) {
        if (g_candidates[i][0] == '\0')
            continue;
        if (access(g_candidates[i], R_OK) == 0) {
            snprintf(g_script, sizeof g_script, "%s", g_candidates[i]);
            return g_script;
        }
    }
    return NULL;
}

/* --- the child ------------------------------------------------------------- */

static void become_server(const char *python, const char *script,
                          const char *root, const char *fault, int ready_fd)
{
    char py[REF9P_PATH_MAX], sc[REF9P_PATH_MAX];
    char rt[REF9P_PATH_MAX], ft[32], fdno[8];
    snprintf(py, sizeof py, "%s", python);
    snprintf(sc, sizeof sc, "%s", script);
    snprintf(rt, sizeof rt, "%s", root);
    snprintf(ft, sizeof ft, "%s", fault);
    snprintf(fdno, sizeof fdno, "%d", READY_FD);
    if (ready_fd != READY_FD) {
        if (dup2(ready_fd, READY_FD) < 0)
            _exit(126);
        close(ready_fd);
    }
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        (void)dup2(devnull, STDOUT_FILENO);
        (void)dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }
    (void)setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    char *const argv[] = { py,     sc,   "--root", rt,  "--port", "0",
                           "--ready-fd", fdno, "--fault", ft, NULL };
    execvp(py, argv);
    _exit(127);
}

/* Read the port the child announced on the ready pipe. */
static int wait_for_port(int pipe_fd, int *port)
{
    char buf[32];
    size_t got = 0;
    while (got < sizeof buf - 1) {
        struct pollfd pfd;
        pfd.fd = pipe_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, REF9P_READY_TIMEOUT_MS) <= 0)
            return -1;
        ssize_t n = read(pipe_fd, buf + got, 1);
        if (n <= 0)
            return -1;
        if (buf[got] == '\n') {
            buf[got] = '\0';
            *port = (int)strtol(buf, NULL, 10);
            return *port > 0 ? 0 : -1;
        }
        got += (size_t)n;
    }
    return -1;
}

static int connect_loopback(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* --- lifecycle ------------------------------------------------------------- */

static void untrack(struct ref9p_server *srv)
{
    for (size_t i = 0; i < g_nservers; i++) {
        if (g_servers[i] == srv)
            g_servers[i] = NULL;
    }
}

static void stop_all_at_exit(void)
{
    for (size_t i = 0; i < g_nservers; i++) {
        if (g_servers[i] != NULL)
            ref9p_stop(g_servers[i]);
    }
}

static void track(struct ref9p_server *srv)
{
    if (!g_atexit_registered) {
        atexit(stop_all_at_exit);
        g_atexit_registered = true;
    }
    if (g_nservers < REF9P_MAX_SERVERS)
        g_servers[g_nservers++] = srv;
}

struct ref9p_server *ref9p_start(const char *root, const char *fault)
{
    const char *python = python_bin();
    const char *script = server_script();
    if (root == NULL || python == NULL || script == NULL) {
        fprintf(stderr, "ref9p: cannot locate python3 or ref9p_server.py\n");
        return NULL;
    }
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    if (pid == 0) {
        close(pipefd[0]);
        become_server(python, script, root, fault != NULL ? fault : "none",
                      pipefd[1]);
        _exit(127); /* not reached */
    }
    close(pipefd[1]);
    struct ref9p_server *srv = calloc(1, sizeof *srv);
    if (srv == NULL) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return NULL;
    }
    srv->pid = pid;
    srv->fd = -1;
    snprintf(srv->root, sizeof srv->root, "%s", root);
    int port = 0;
    if (wait_for_port(pipefd[0], &port) != 0) {
        fprintf(stderr, "ref9p: server on %s did not announce a port\n", root);
        close(pipefd[0]);
        ref9p_stop(srv);
        return NULL;
    }
    close(pipefd[0]);
    srv->port = port;
    srv->fd = connect_loopback(port);
    if (srv->fd < 0) {
        fprintf(stderr, "ref9p: cannot connect to 127.0.0.1:%d\n", port);
        ref9p_stop(srv);
        return NULL;
    }
    track(srv);
    return srv;
}

void ref9p_stop(struct ref9p_server *srv)
{
    if (srv == NULL)
        return;
    untrack(srv);
    if (srv->fd >= 0)
        close(srv->fd);
    if (srv->pid > 0) {
        if (kill(srv->pid, SIGTERM) == 0) {
            for (int i = 0; i < 100; i++) {
                int status = 0;
                pid_t done = waitpid(srv->pid, &status, WNOHANG);
                if (done == srv->pid || done < 0)
                    break;
                struct timespec nap = { 0, 20 * 1000 * 1000 };
                (void)nanosleep(&nap, NULL);
            }
            if (kill(srv->pid, 0) == 0) {
                kill(srv->pid, SIGKILL);
                (void)waitpid(srv->pid, NULL, 0);
            }
        }
        srv->pid = -1;
    }
    free(srv);
}

const char *ref9p_root(const struct ref9p_server *srv)
{
    return srv != NULL ? srv->root : NULL;
}

int ref9p_fd(const struct ref9p_server *srv)
{
    return srv != NULL ? srv->fd : -1;
}

void *ref9p_rpc_user(struct ref9p_server *srv)
{
    return srv != NULL ? (void *)&srv->fd : NULL;
}

/* --- framing --------------------------------------------------------------- */

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
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? PGW_ETIMEDOUT
                                                     : PGW_EIO;
}

/* MSG_NOSIGNAL: a server that closes the connection mid-frame must not kill
 * the client with SIGPIPE while it is still writing (ROB-07). Without it the
 * closing clunk of pgw_p9_session_close() raised SIGPIPE and the whole process
 * died with exit status 141 instead of reporting PGW_EIO - found by
 * tests/faulty/ (--fault close-mid-frame). */
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

pgw_err ref9p_frame(int fd, const void *tx, size_t tx_len, void *rx,
                    size_t rx_cap, size_t *rx_len, uint32_t timeout_ms)
{
    if (fd < 0 || tx == NULL || (rx == NULL && rx_cap > 0) || rx_len == NULL)
        return PGW_EINVAL;
    *rx_len = 0;
    set_socket_timeout(fd, timeout_ms != 0 ? timeout_ms : 10000u);
    uint8_t prefix[4];
    uint32_t out_len = (uint32_t)tx_len;
    prefix[0] = (uint8_t)(out_len >> 24);
    prefix[1] = (uint8_t)(out_len >> 16);
    prefix[2] = (uint8_t)(out_len >> 8);
    prefix[3] = (uint8_t)out_len;
    pgw_err e = send_all(fd, prefix, sizeof prefix);
    if (e != PGW_OK || (e = send_all(fd, tx, tx_len)) != PGW_OK)
        return e;
    uint8_t in_prefix[4];
    if ((e = recv_all(fd, in_prefix, sizeof in_prefix)) != PGW_OK)
        return e;
    uint32_t in_len = ((uint32_t)in_prefix[0] << 24) |
                      ((uint32_t)in_prefix[1] << 16) |
                      ((uint32_t)in_prefix[2] << 8) | (uint32_t)in_prefix[3];
    if ((size_t)in_len > rx_cap)
        return PGW_EMSGSIZE;
    if (in_len > 0 && (e = recv_all(fd, rx, in_len)) != PGW_OK)
        return e;
    *rx_len = in_len;
    return PGW_OK;
}

pgw_err ref9p_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                  size_t *rx_len, uint32_t prio, uint32_t timeout_ms,
                  void *user)
{
    (void)prio; /* the reference transport has a single FIFO connection */
    if (user == NULL)
        return PGW_EINVAL;
    const int *fd = (const int *)user;
    return ref9p_frame(*fd, tx, tx_len, rx, rx_cap, rx_len, timeout_ms);
}
