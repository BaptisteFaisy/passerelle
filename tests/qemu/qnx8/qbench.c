/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/qemu/qnx8/qbench.c - the measurements, taken inside the QNX guest.
 *
 * Two numbers the specification asks for, measured on the real chain (guest,
 * our driver, the 9P server, the host):
 *
 *   rate  - read a file of the share and report the elapsed time and the
 *           rate it achieved (PERF-03).
 *   prio  - start a bulk copy in a thread, then read a small file from a
 *           thread with a *high* priority and report how long that read took
 *           (PERF-05). The driver re-applies the caller's priority to the
 *           thread that serves the request (QNX-03), so the high-priority read
 *           must not wait for the bulk copy to finish.
 *
 * The program prints one line per measurement, in a form the host can parse.
 */
#define _POSIX_C_SOURCE 200809L
#define _QNX_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BUF_BYTES (256u * 1024u)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Read the whole file and report the rate. */
static int bench_rate(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("qbench: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    char *buf = malloc(BUF_BYTES);
    if (buf == NULL) {
        close(fd);
        printf("qbench: out of memory\n");
        return 1;
    }
    double t0 = now_s();
    unsigned long long total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, BUF_BYTES);
        if (n < 0) {
            printf("qbench: read failed: %s\n", strerror(errno));
            free(buf);
            close(fd);
            return 1;
        }
        if (n == 0)
            break;
        total += (unsigned long long)n;
    }
    double dt = now_s() - t0;
    free(buf);
    close(fd);
    printf("qbench: rate bytes=%llu seconds=%.3f MiBps=%.2f\n", total, dt,
           dt > 0.0 ? (double)total / dt / (1024.0 * 1024.0) : 0.0);
    fflush(stdout);
    return 0;
}

struct bulk {
    const char *path;
    volatile int stop;
    unsigned long long bytes;
    int prio;
};

static void *bulk_reader(void *arg)
{
    struct bulk *b = (struct bulk *)arg;
    /* The bulk copy runs at a low priority: the point of the measurement is
     * that the high-priority read overtakes it (QNX-03, PERF-05). */
    int policy = 0;
    struct sched_param sp;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
        sp.sched_priority = b->prio;
        (void)pthread_setschedparam(pthread_self(), policy, &sp);
    }
    int fd = open(b->path, O_RDONLY);
    if (fd < 0)
        return NULL;
    char *buf = malloc(BUF_BYTES);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    while (!b->stop) {
        ssize_t n = read(fd, buf, BUF_BYTES);
        if (n <= 0)
            break;
        b->bytes += (unsigned long long)n;
    }
    free(buf);
    close(fd);
    return NULL;
}

/* Read a small file and time it. */
static int read_timed(const char *path, double *out_s, ssize_t *out_n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("qbench: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    char buf[64];
    double t0 = now_s();
    ssize_t n = read(fd, buf, sizeof buf);
    *out_s = now_s() - t0;
    *out_n = n;
    close(fd);
    return 0;
}

/* Start the bulk readers; returns how many are running. */
static int start_bulk(pthread_t *th, int nth, struct bulk *b)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u * 1024u);
    int started = 0;
    for (int i = 0; i < nth; i++) {
        if (pthread_create(&th[i], &attr, bulk_reader, b) != 0)
            break;
        started++;
    }
    pthread_attr_destroy(&attr);
    return started;
}

/* Read a small file while a bulk copy runs, at a high priority. */
static int bench_prio(const char *path, const char *small, int prio)
{
    /* The process priority bounds the thread priorities: it goes up first and
     * the bulk threads lower themselves afterwards. */
    if (setpriority(PRIO_PROCESS, 0, 63) != 0)
        printf("qbench: setpriority(63) failed: %s\n", strerror(errno));

    struct bulk b;
    b.path = path;
    b.stop = 0;
    b.bytes = 0;
    b.prio = 10;

    pthread_t th[16];
    int started = start_bulk(th, sizeof th / sizeof th[0], &b);
    if (started == 0) {
        printf("qbench: cannot start the bulk readers\n");
        return 1;
    }

    usleep(300000); /* let the bulk copies saturate the transport */

    /* The reading thread runs at the priority under test. */
    int policy = 0;
    struct sched_param sp;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
        sp.sched_priority = prio;
        (void)pthread_setschedparam(pthread_self(), policy, &sp);
    }

    double dt = 0.0;
    ssize_t n = -1;
    if (read_timed(small, &dt, &n) != 0) {
        b.stop = 1;
        for (int i = 0; i < started; i++)
            pthread_join(th[i], NULL);
        return 1;
    }

    b.stop = 1;
    for (int i = 0; i < started; i++)
        pthread_join(th[i], NULL);
    printf("qbench: prio priority=%d readers=%d bytes=%lld seconds=%.4f "
           "bulk_bytes=%llu\n",
           prio, started, (long long)n, dt, b.bytes);
    fflush(stdout);
    return 0;
}

/* Two readers at the same time: is the transport able to serve them? */
struct reader {
    const char *path;
    unsigned long long bytes;
    double seconds;
    int prio;
};

static void *reader_thread(void *arg)
{
    struct reader *r = (struct reader *)arg;
    if (r->prio > 0)
        (void)setpriority(PRIO_PROCESS, 0, r->prio);
    int fd = open(r->path, O_RDONLY);
    if (fd < 0)
        return NULL;
    char *buf = malloc(BUF_BYTES);
    if (buf == NULL) {
        close(fd);
        return NULL;
    }
    double t0 = now_s();
    for (;;) {
        ssize_t n = read(fd, buf, BUF_BYTES);
        if (n <= 0)
            break;
        r->bytes += (unsigned long long)n;
    }
    r->seconds = now_s() - t0;
    free(buf);
    close(fd);
    return NULL;
}

static int bench_two(const char *path, int prio)
{
    struct reader a;
    struct reader b;
    a.path = b.path = path;
    a.bytes = b.bytes = 0;
    a.seconds = b.seconds = 0.0;
    a.prio = b.prio = prio;
    pthread_t ta;
    pthread_t tb;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u * 1024u);
    (void)pthread_create(&ta, &attr, reader_thread, &a);
    (void)pthread_create(&tb, &attr, reader_thread, &b);
    pthread_attr_destroy(&attr);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    printf("qbench: two readers priority=%d a_bytes=%llu a_s=%.3f a_MiBps=%.2f "
           "b_bytes=%llu b_s=%.3f b_MiBps=%.2f\n",
           prio, a.bytes, a.seconds,
           a.seconds > 0.0 ? (double)a.bytes / a.seconds / (1024.0 * 1024.0) : 0.0,
           b.bytes, b.seconds,
           b.seconds > 0.0 ? (double)b.bytes / b.seconds / (1024.0 * 1024.0) : 0.0);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "rate";
    const char *path = argc > 2 ? argv[2] : "/host/big.bin";
    const char *small = argc > 3 ? argv[3] : "/host/from-host.txt";
    int prio = argc > 4 ? atoi(argv[4]) : 63;

    if (strcmp(mode, "rate") == 0)
        return bench_rate(path);
    if (strcmp(mode, "prio") == 0)
        return bench_prio(path, small, prio);
    if (strcmp(mode, "two") == 0)
        return bench_two(path, prio);
    printf("usage: qbench rate|prio|two [file] [small-file] [priority]\n");
    return 2;
}
