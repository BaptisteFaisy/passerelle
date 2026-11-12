/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/interop/interop_diod.c - start, talk to and stop diod.
 *
 * The server is started with the command line of diod(8):
 *
 *   diod -f -l 127.0.0.1:PORT -e EXPORT -n -N -L stderr
 *
 *   -f  stay in the foreground, so the bench owns the process;
 *   -l  listen on a loopback port the bench chose;
 *   -e  the export to serve (diod names an export after its path, and the
 *       client attaches with that path as aname);
 *   -n  no MUNGE credential is required to attach (our client does not
 *       carry one);
 *   -N  no password database lookup, so any uid may attach;
 *   -L  the log goes to stderr, which the bench redirects to a file.
 *
 * Nothing here needs any privilege: diod serves the export with the rights
 * of the bench itself, which is why the truth on disk stays comparable.
 *
 * Covers P9-01 and P9-10.
 */
#define _POSIX_C_SOURCE 200809L

#include "interop_diod.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIOD_START_TIMEOUT_MS 10000
#define DIOD_POLL_MS 100
#define DIOD_STOP_TRIES 50

struct interop_diod {
    pid_t pid;
    int fd;
    int port;
    char log[INTEROP_DIOD_PATH_MAX];
    char root[INTEROP_DIOD_PATH_MAX];
};

static const char *const g_known[] = {
        "/usr/sbin/diod",
        "/usr/local/sbin/diod",
        "/usr/bin/diod",
        "/sbin/diod",
};

bool interop_diod_find(char *path, size_t cap, char *why, size_t why_cap)
{
    const char *env = getenv("PGW_DIOD");
    if (env != NULL && env[0] != '\0') {
        if (access(env, X_OK) != 0) {
            snprintf(why, why_cap, "PGW_DIOD=%s is not an executable file", env);
            return false;
        }
        snprintf(path, cap, "%s", env);
        return true;
    }
    for (size_t i = 0; i < sizeof g_known / sizeof g_known[0]; i++) {
        if (access(g_known[i], X_OK) == 0) {
            snprintf(path, cap, "%s", g_known[i]);
            return true;
        }
    }
    const char *path_env = getenv("PATH");
    if (path_env != NULL) {
        char buf[INTEROP_DIOD_PATH_MAX];
        snprintf(buf, sizeof buf, "%s", path_env);
        for (char *tok = strtok(buf, ":"); tok != NULL; tok = strtok(NULL, ":")) {
            char cand[INTEROP_DIOD_PATH_MAX];
            snprintf(cand, sizeof cand, "%s/diod", tok);
            if (access(cand, X_OK) == 0) {
                snprintf(path, cap, "%s", cand);
                return true;
            }
        }
    }
    snprintf(why, why_cap,
             "diod (a third-party 9P2000.L server) is not installed here: no "
             "'diod' in PATH, none of /usr/sbin/diod, /usr/local/sbin/diod, "
             "/usr/bin/diod, /sbin/diod; install the 'diod' package "
             "(Debian/Ubuntu: apt-get install diod) or point PGW_DIOD at it");
    return false;
}

static int free_loopback_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = (socklen_t)sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    int port = (int)ntohs(addr.sin_port);
    close(fd);
    return port;
}

static int connect_loopback(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int nodelay = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
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

static void sleep_ms(int ms)
{
    struct timespec nap = {0, (long)ms * 1000L * 1000L};
    (void)nanosleep(&nap, NULL);
}

void interop_diod_log_tail(const struct interop_diod *d, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    out[0] = '\0';
    if (d == NULL || d->log[0] == '\0')
        return;
    FILE *f = fopen(d->log, "r");
    if (f == NULL)
        return;
    size_t len = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0) {
            long start = size - (long)cap + 1;
            if (start < 0)
                start = 0;
            (void)fseek(f, start, SEEK_SET);
            len = fread(out, 1, cap - 1, f);
        }
    }
    out[len] = '\0';
    fclose(f);
}

struct interop_diod *interop_diod_start(const char *root, char *why, size_t why_cap)
{
    char bin[INTEROP_DIOD_PATH_MAX];
    if (!interop_diod_find(bin, sizeof bin, why, why_cap))
        return NULL;
    int port = free_loopback_port();
    if (port <= 0) {
        snprintf(why, why_cap, "no free TCP port on the loopback");
        return NULL;
    }
    struct interop_diod *d = calloc(1, sizeof *d);
    if (d == NULL) {
        snprintf(why, why_cap, "out of memory");
        return NULL;
    }
    d->pid = -1;
    d->fd = -1;
    d->port = port;
    snprintf(d->root, sizeof d->root, "%s", root);
    /* The log lives next to the export, not inside it: the export is
     * the tree the client walks, it must hold nothing but the truth. */
    snprintf(d->log, sizeof d->log, "%s.log", root);
    char listen_arg[64];
    snprintf(listen_arg, sizeof listen_arg, "127.0.0.1:%d", port);
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(why, why_cap, "fork failed: %s", strerror(errno));
        free(d);
        return NULL;
    }
    if (pid == 0) {
        int lf = open(d->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (lf >= 0) {
            (void)dup2(lf, STDOUT_FILENO);
            (void)dup2(lf, STDERR_FILENO);
            if (lf > STDERR_FILENO)
                close(lf);
        }
        char *const argv[] = {bin,  "-f", "-l", listen_arg, "-e", d->root,
                              "-n", "-N", "-L", "stderr",   NULL};
        execv(bin, argv);
        _exit(127);
    }
    d->pid = pid;
    int fd = -1;
    for (int i = 0; i < DIOD_START_TIMEOUT_MS / DIOD_POLL_MS; i++) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            char tail[512];
            interop_diod_log_tail(d, tail, sizeof tail);
            snprintf(why, why_cap, "diod exited before listening: %s", tail);
            d->pid = -1;
            free(d);
            return NULL;
        }
        fd = connect_loopback(port);
        if (fd >= 0)
            break;
        sleep_ms(DIOD_POLL_MS);
    }
    if (fd < 0) {
        char tail[512];
        interop_diod_log_tail(d, tail, sizeof tail);
        snprintf(why, why_cap, "diod did not listen on 127.0.0.1:%d: %s", port, tail);
        interop_diod_stop(d);
        return NULL;
    }
    d->fd = fd;
    return d;
}

void interop_diod_stop(struct interop_diod *d)
{
    if (d == NULL)
        return;
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    if (d->pid > 0) {
        (void)kill(d->pid, SIGTERM);
        for (int i = 0; i < DIOD_STOP_TRIES; i++) {
            int status = 0;
            pid_t done = waitpid(d->pid, &status, WNOHANG);
            if (done == d->pid || done < 0) {
                d->pid = -1;
                break;
            }
            sleep_ms(20);
        }
        if (d->pid > 0) {
            (void)kill(d->pid, SIGKILL);
            (void)waitpid(d->pid, NULL, 0);
            d->pid = -1;
        }
    }
    free(d);
}

int interop_diod_fd(const struct interop_diod *d)
{
    return d != NULL ? d->fd : -1;
}

void *interop_diod_rpc_user(struct interop_diod *d)
{
    return d != NULL ? (void *)&d->fd : NULL;
}
