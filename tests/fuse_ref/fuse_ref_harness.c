/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_ref_harness.c - process/socket plumbing for the reference FUSE server.
 *
 * Implements requirements:
 *   - VFS-03 / VFS-05: a real transport (UNIX stream socket, 4-byte big-endian
 *     length per message) between the Passerelle client and the reference
 *     server, instead of the in-memory fake
 *   - the server is started with fork+exec and waited on with --ready-fd, so
 *     the test never sleeps on a fixed delay and never races the listener
 *
 * Cleanup is guaranteed twice: fuse_ref_ctx_stop() on the normal path, and an
 * exit handler that kills whatever is still running when a case fails.
 */
#define _POSIX_C_SOURCE 200809L
#include "fuse_ref_harness.h"

#include "pgw/test.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PGW_PYTHON3
#define PGW_PYTHON3 "python3"
#endif
#ifndef PGW_FUSE_REF_SERVER
#define PGW_FUSE_REF_SERVER "tests/fuse_ref/fuse_ref_server.py"
#endif

#define READY_TIMEOUT_MS 15000
#define RECV_TIMEOUT_S 10
#define FRAME_MAX (1u << 20)

/* Requests the kernel never answers: the transport must not wait for them. */
static bool is_no_reply(uint32_t opcode)
{
    return opcode == (uint32_t)PGW_FUSE_FORGET ||
           opcode == (uint32_t)PGW_FUSE_BATCH_FORGET ||
           opcode == (uint32_t)PGW_FUSE_INTERRUPT;
}

static char *env_or(const char *name, char *fallback)
{
    char *v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* --- framed socket I/O ---------------------------------------------------- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static pgw_err write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return PGW_EIO;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return PGW_OK;
}

static pgw_err read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? PGW_ETIMEDOUT : PGW_EIO;
        }
        if (n == 0)
            return PGW_EIO; /* the server closed the connection */
        p += (size_t)n;
        len -= (size_t)n;
    }
    return PGW_OK;
}

static void drain(int fd, size_t len)
{
    uint8_t scratch[512];
    while (len > 0) {
        size_t want = (len < sizeof(scratch)) ? len : sizeof(scratch);
        if (read_all(fd, scratch, want) != PGW_OK)
            return;
        len -= want;
    }
}

pgw_err fuse_ref_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                     size_t *rx_len, bool hiprio, uint32_t prio, uint32_t timeout_ms,
                     void *user)
{
    fuse_ref_ctx *c = (fuse_ref_ctx *)user;
    (void)hiprio;
    (void)prio;
    (void)timeout_ms;
    if (c == NULL || tx == NULL || rx == NULL || rx_len == NULL)
        return PGW_EINVAL;
    *rx_len = 0;
    if (tx_len < 8 || tx_len > FRAME_MAX)
        return PGW_EINVAL;

    uint8_t prefix[4];
    put_be32(prefix, (uint32_t)tx_len);
    pgw_err rc = write_all(c->fd, prefix, sizeof(prefix));
    if (rc != PGW_OK)
        return rc;
    rc = write_all(c->fd, tx, tx_len);
    if (rc != PGW_OK)
        return rc;
    if (is_no_reply(get_le32((const uint8_t *)tx + 4)))
        return PGW_OK; /* FORGET / BATCH_FORGET / INTERRUPT have no reply */

    rc = read_all(c->fd, prefix, sizeof(prefix));
    if (rc != PGW_OK)
        return rc;
    uint32_t n = get_be32(prefix);
    if (n == 0 || n > FRAME_MAX)
        return PGW_EIO;
    if ((size_t)n > rx_cap) {
        drain(c->fd, n); /* keep the stream in sync, then refuse */
        return PGW_EIO;
    }
    rc = read_all(c->fd, rx, n);
    if (rc != PGW_OK)
        return rc;
    *rx_len = n;
    return PGW_OK;
}

/* --- the truth directory --------------------------------------------------- */

static bool write_file(const char *path, const void *data, size_t len, mode_t mode)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0)
        return false;
    pgw_err rc = write_all(fd, data, len);
    close(fd);
    return rc == PGW_OK;
}

static bool seed_truth(const char *root)
{
    char path[FUSE_REF_PATH_MAX];
    static uint8_t big[FUSE_REF_BIG_SIZE];
    pgw_test_pattern(big, sizeof(big), 0xC0FFEEu, 0);

    fuse_ref_join(root, FUSE_REF_BIG_FILE, path, sizeof(path));
    if (!write_file(path, big, sizeof(big), 0644))
        return false;
    fuse_ref_join(root, FUSE_REF_SMALL_FILE, path, sizeof(path));
    if (!write_file(path, "abcdefghij", 10, 0640))
        return false;
    fuse_ref_join(root, FUSE_REF_DIR, path, sizeof(path));
    if (mkdir(path, 0755) != 0)
        return false;
    fuse_ref_join(root, FUSE_REF_DIR "/" FUSE_REF_INNER_FILE, path, sizeof(path));
    if (!write_file(path, "inner", 5, 0644))
        return false;
    fuse_ref_join(root, FUSE_REF_EMPTY_DIR, path, sizeof(path));
    if (mkdir(path, 0755) != 0)
        return false;
    fuse_ref_join(root, FUSE_REF_LINK, path, sizeof(path));
    if (symlink(FUSE_REF_BIG_FILE, path) != 0)
        return false;
    /* A symlink that points at itself: opening it fails with ELOOP, which is
     * how the tests check that error travels back through the protocol. */
    fuse_ref_join(root, FUSE_REF_LOOP, path, sizeof(path));
    return symlink(FUSE_REF_LOOP, path) == 0;
}

/* --- server process -------------------------------------------------------- */

static fuse_ref_ctx *g_live[8];

static void kill_live(void)
{
    for (size_t i = 0; i < sizeof(g_live) / sizeof(g_live[0]); i++) {
        if (g_live[i] == NULL)
            continue;
        if (g_live[i]->fd >= 0)
            close(g_live[i]->fd);
        if (g_live[i]->pid > 0) {
            kill(g_live[i]->pid, SIGKILL);
            (void)waitpid(g_live[i]->pid, NULL, 0);
        }
        g_live[i] = NULL;
    }
}

static void track(fuse_ref_ctx *c)
{
    static bool registered = false;
    if (!registered) {
        atexit(kill_live);
        registered = true;
    }
    for (size_t i = 0; i < sizeof(g_live) / sizeof(g_live[0]); i++) {
        if (g_live[i] == NULL) {
            g_live[i] = c;
            return;
        }
    }
}

static void untrack(fuse_ref_ctx *c)
{
    for (size_t i = 0; i < sizeof(g_live) / sizeof(g_live[0]); i++) {
        if (g_live[i] == c)
            g_live[i] = NULL;
    }
}

/* Read one line (the socket path) from the server's --ready-fd pipe. */
static int read_ready_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, READY_TIMEOUT_MS) <= 0)
            return -1;
        char ch = '\0';
        if (read(fd, &ch, 1) != 1)
            return -1; /* the server died before announcing itself */
        if (ch == '\n')
            break;
        if (n + 1 < cap)
            buf[n++] = ch;
    }
    buf[n] = '\0';
    return (n > 0) ? 0 : -1;
}

static int connect_unix(const char *path)
{
    struct sockaddr_un addr;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(addr.sun_path))
        return -1; /* a UNIX socket path must fit in sun_path */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, len);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = RECV_TIMEOUT_S;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void child_exec(char *root, char *sock, char *fault, char *minor, int ready_fd)
{
    char *argv[16];
    size_t n = 0;
    argv[n++] = env_or("PGW_FUSE_REF_PY", PGW_PYTHON3);
    argv[n++] = "-B"; /* never leave a __pycache__ behind in the source tree */
    argv[n++] = env_or("PGW_FUSE_REF_SERVER", PGW_FUSE_REF_SERVER);
    argv[n++] = "--root";
    argv[n++] = root;
    argv[n++] = "--socket";
    argv[n++] = sock;
    argv[n++] = "--ready-fd";
    argv[n++] = "3";
    argv[n++] = "--idle-timeout";
    argv[n++] = "30";
    if (fault != NULL) {
        argv[n++] = "--fault";
        argv[n++] = fault;
    }
    if (minor != NULL) {
        argv[n++] = "--minor";
        argv[n++] = minor;
    }
    argv[n] = NULL;
    if (ready_fd != 3) {
        if (dup2(ready_fd, 3) < 0)
            _exit(127);
        close(ready_fd);
    }
    execvp(argv[0], argv);
    _exit(127);
}

/* fork+exec the server; returns the connected socket, or -1. */
static int spawn_server(char *root, char *sock, char *fault, char *minor, pid_t *out_pid)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        child_exec(root, sock, fault, minor, fds[1]);
        _exit(127); /* unreachable */
    }
    close(fds[1]);
    char path[FUSE_REF_PATH_MAX];
    if (read_ready_line(fds[0], path, sizeof(path)) != 0) {
        close(fds[0]);
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    close(fds[0]);
    int fd = connect_unix(path);
    if (fd < 0) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    *out_pid = pid;
    return fd;
}

fuse_ref_ctx *fuse_ref_ctx_start(char *fault, char *minor)
{
    char *tmp = pgw_test_tmpdir("fuse-ref");
    if (tmp == NULL)
        return NULL;
    fuse_ref_ctx *c = (fuse_ref_ctx *)calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->fd = -1;
    fuse_ref_join(tmp, "root", c->root, sizeof(c->root));
    char sock[FUSE_REF_PATH_MAX];
    fuse_ref_join(tmp, "ref.sock", sock, sizeof(sock));
    if (c->root[0] == '\0' || sock[0] == '\0' || mkdir(c->root, 0755) != 0) {
        free(c);
        return NULL;
    }
    if (!seed_truth(c->root)) {
        free(c);
        return NULL;
    }
    int fd = spawn_server(c->root, sock, fault, minor, &c->pid);
    if (fd < 0) {
        free(c);
        return NULL;
    }
    c->fd = fd;
    c->session = pgw_fuse_session_open(fuse_ref_rpc, c, FUSE_REF_MAX_WRITE);
    if (c->session == NULL) {
        close(fd);
        kill(c->pid, SIGTERM);
        (void)waitpid(c->pid, NULL, 0);
        free(c);
        return NULL;
    }
    track(c);
    return c;
}

void fuse_ref_ctx_stop(fuse_ref_ctx *c)
{
    if (c == NULL)
        return;
    untrack(c);
    if (c->session != NULL) {
        pgw_fuse_session_close(c->session); /* sends the pending FORGETs */
        c->session = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    if (c->pid > 0) {
        kill(c->pid, SIGTERM);
        (void)waitpid(c->pid, NULL, 0);
        c->pid = 0;
    }
    free(c);
}
