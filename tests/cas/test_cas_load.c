/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_load.c - a mount under a continuous workload (gap 63).
 *
 * Concurrent clients hammer the layer with metadata operations while a bulk
 * copy runs, and the case asserts what a mount must guarantee under load:
 * it finishes (no deadlock), the descriptors stay bounded and come back, the
 * tree it reports still matches the truth, the copied bytes are exact, and a
 * high-priority read stays fast while the load runs (PERF-05, PC side).
 *
 * One mount per client thread, on purpose: the layer under test keeps its
 * state (the in-flight counter, the open-file table, the attribute cache, the
 * lock table) without an internal lock, so several threads sharing *one*
 * pgw_rmfs would be a data race and not a test. The workload modelled here is
 * the one the specification describes: several guest processes on the same
 * share. That limitation is stated in the pull request, not hidden.
 *
 * Scale from PGW_CAS_LOAD_THREADS / PGW_CAS_LOAD_SECONDS: three threads and
 * two seconds in a pull request, more in the DISABLED long campaign.
 *
 * Covers PERF-05, ROB-02, ROB-03, FS-11 (gap 63 of docs/gap-analysis.md).
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "diff.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"
#include "pgw/time.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (128u * 1024u)
#define BLOCK_PROBE 4096u
#define SRC_BYTES (2u * 1024u * 1024u)
#define FILES_PER_ROUND 8u
#define LATENCY_BOUND_NS 500000000ULL /* 500 ms, generous under ASan */
#define FD_HEADROOM 96u

typedef struct load_run {
    char root[512];
    uint64_t deadline_ns;
    uint64_t budget_ns;
    size_t threads;
    uint64_t max_latency_ns; /* high-priority read, worst case */
    size_t fd_baseline;
    size_t fd_peak;
} load_run;

typedef struct load_worker {
    load_run *run;
    size_t index;
    size_t ops;
    uint64_t copied;
    pgw_err verdict;
    size_t handles_left;
} load_worker;

static size_t max_size(size_t a, size_t b)
{
    return (a > b) ? a : b;
}

static uint64_t load_seconds(void)
{
    return cas_env_u64("PGW_CAS_LOAD_SECONDS", 2);
}

static size_t load_threads(void)
{
    return (size_t)cas_env_u64("PGW_CAS_LOAD_THREADS", 3);
}

/* stat, list and unlink what the round created: the subtree stays empty, so
 * the tree comparison at the end of the load stays cheap. */
static size_t metadata_round_tail(load_worker *w, cas_mount *m,
                                  const char *dir)
{
    size_t ops = 0;
    char rel[64];
    for (size_t i = 0; i < FILES_PER_ROUND && w->verdict == PGW_OK; i++) {
        snprintf(rel, sizeof(rel), "%s/f%02zu", dir, i);
        pgw_fs_attr at;
        w->verdict = pgw_rmfs_stat(m->fs, &m->cred, rel, &at);
        ops++;
    }
    pgw_rmfs_fd *dh = NULL;
    if (w->verdict == PGW_OK)
        w->verdict = pgw_rmfs_open(m->fs, &m->cred, dir,
                                   PGW_O_RDONLY | PGW_O_DIRECTORY, 0, &dh);
    if (w->verdict == PGW_OK) {
        pgw_fs_dirent ents[FILES_PER_ROUND];
        size_t got = 0;
        uint64_t next = 0;
        w->verdict = pgw_rmfs_readdir(m->fs, dh, 0, ents, FILES_PER_ROUND,
                                      &got, &next);
        ops += 1 + got;
        w->verdict = pgw_rmfs_close(m->fs, dh);
    }
    for (size_t i = 0; i < FILES_PER_ROUND && w->verdict == PGW_OK; i++) {
        snprintf(rel, sizeof(rel), "%s/f%02zu", dir, i);
        w->verdict = pgw_rmfs_unlink(m->fs, &m->cred, rel, false);
        ops++;
    }
    return ops;
}

/* One round of metadata work on the worker's own subtree: create, write,
 * stat, list, unlink. Returns the number of operations really performed. */
static size_t metadata_round(load_worker *w, cas_mount *m, const char *dir)
{
    size_t ops = 0;
    char rel[64];
    for (size_t i = 0; i < FILES_PER_ROUND && w->verdict == PGW_OK; i++) {
        snprintf(rel, sizeof(rel), "%s/f%02zu", dir, i);
        pgw_rmfs_fd *fd = NULL;
        w->verdict = pgw_rmfs_open(m->fs, &m->cred, rel,
                                   PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                   &fd);
        if (w->verdict != PGW_OK)
            break;
        char data[32];
        int n = snprintf(data, sizeof(data), "%zu-%zu", w->index, i);
        size_t written = 0;
        w->verdict = pgw_rmfs_write(m->fs, fd, 0, data, (size_t)n, &written);
        if (w->verdict == PGW_OK)
            w->verdict = pgw_rmfs_close(m->fs, fd);
        ops += 3;
    }
    return ops + metadata_round_tail(w, m, dir);
}

static void *metadata_worker(void *arg)
{
    load_worker *w = (load_worker *)arg;
    cas_mount m;
    if (!cas_mount_open_at(&m, w->run->root, NULL, NULL)) {
        w->verdict = PGW_EIO;
        return NULL;
    }
    char dir[32];
    snprintf(dir, sizeof(dir), "w%zu", w->index);
    pgw_err e = pgw_rmfs_mkdir(m.fs, &m.cred, dir, 0755);
    if (e != PGW_OK && e != PGW_EEXIST)
        w->verdict = e;
    while (w->verdict == PGW_OK && pgw_now_ns() < w->run->deadline_ns)
        w->ops += metadata_round(w, &m, dir);
    if (w->verdict == PGW_OK)
        w->verdict = pgw_rmfs_unlink(m.fs, &m.cred, dir, true);
    w->handles_left = pgw_posix_backend_open_handles(m.be);
    cas_mount_close(&m);
    return NULL;
}

/* The bulk copy: read a real file through the layer and write it back through
 * the same layer, chunk by chunk, checking every chunk on the way. */
static void *bulk_worker(void *arg)
{
    load_worker *w = (load_worker *)arg;
    cas_mount m;
    if (!cas_mount_open_at(&m, w->run->root, NULL, NULL)) {
        w->verdict = PGW_EIO;
        return NULL;
    }
    pgw_rmfs_fd *src = NULL;
    pgw_rmfs_fd *dst = NULL;
    w->verdict = pgw_rmfs_open(m.fs, &m.cred, "src.bin", PGW_O_RDONLY, 0, &src);
    if (w->verdict == PGW_OK)
        w->verdict = pgw_rmfs_open(m.fs, &m.cred, "copy.bin",
                                   PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                   &dst);
    uint8_t buf[CHUNK];
    while (w->verdict == PGW_OK && w->copied < SRC_BYTES) {
        size_t n = 0;
        w->verdict = pgw_rmfs_read(m.fs, src, w->copied, buf, sizeof(buf), &n);
        if (w->verdict != PGW_OK || n == 0)
            break;
        if (!pgw_test_pattern_ok(buf, n, 42, w->copied)) {
            w->verdict = PGW_EIO; /* the source did not survive the load */
            break;
        }
        size_t written = 0;
        w->verdict = pgw_rmfs_write(m.fs, dst, w->copied, buf, n, &written);
        if (w->verdict == PGW_OK && written != n)
            w->verdict = PGW_EIO;
        w->copied += written;
    }
    if (src != NULL)
        pgw_rmfs_close(m.fs, src);
    if (dst != NULL)
        pgw_rmfs_close(m.fs, dst);
    w->handles_left = pgw_posix_backend_open_handles(m.be);
    cas_mount_close(&m);
    return NULL;
}

/* The high-priority client: one small read, timed, while everybody else runs.
 * Its mount is its own, its priority is the highest of the run. */
static void priority_probe(load_run *run, uint64_t deadline_ns)
{
    cas_mount m;
    if (!cas_mount_open_at(&m, run->root, NULL, NULL))
        return;
    pgw_fs_cred urgent = m.cred;
    urgent.prio = 200; /* the scheduler orders by urgency, bigger is first */
    pgw_rmfs_fd *fd = NULL;
    if (pgw_rmfs_open(m.fs, &urgent, "prio.bin", PGW_O_RDONLY, 0, &fd) !=
        PGW_OK) {
        cas_mount_close(&m);
        return;
    }
    uint8_t buf[BLOCK_PROBE];
    while (pgw_now_ns() < deadline_ns) {
        const uint64_t t0 = pgw_now_ns();
        size_t n = 0;
        pgw_err e = pgw_rmfs_read(m.fs, fd, 0, buf, sizeof(buf), &n);
        const uint64_t dt = pgw_now_ns() - t0;
        if (e == PGW_OK && n == sizeof(buf) && dt > run->max_latency_ns)
            run->max_latency_ns = dt;
        run->fd_peak = max_size(run->fd_peak, cas_fd_count());
    }
    pgw_rmfs_close(m.fs, fd);
    cas_mount_close(&m);
}

/* --- the cases ------------------------------------------------------------ */

/* The tree the load starts from, written by the test on the truth side: a real
 * file to copy and a small file the high-priority client reads. */
static void prepare_truth_tree(const char *root)
{
    uint8_t *buf = malloc(CHUNK);
    PGW_ASSERT(buf != NULL);
    char path[CAS_PATH_MAX];

    cas_path_join(path, sizeof(path), root, "src.bin");
    FILE *f = fopen(path, "wb");
    PGW_ASSERT(f != NULL);
    for (uint64_t off = 0; off < SRC_BYTES; off += CHUNK) {
        pgw_test_pattern(buf, CHUNK, 42, off);
        PGW_ASSERT_EQ_U(fwrite(buf, 1, CHUNK, f), CHUNK);
    }
    PGW_ASSERT_EQ_I(fclose(f), 0);

    cas_path_join(path, sizeof(path), root, "prio.bin");
    f = fopen(path, "wb");
    PGW_ASSERT(f != NULL);
    pgw_test_pattern(buf, BLOCK_PROBE, 7, 0);
    PGW_ASSERT_EQ_U(fwrite(buf, 1, BLOCK_PROBE, f), BLOCK_PROBE);
    PGW_ASSERT_EQ_I(fclose(f), 0);
    free(buf);
}

/* Start the workload: `threads` metadata clients plus, when asked, the bulk
 * copy. The calling thread is the monitor and the high-priority client. */
static void load_start(load_run *run, load_worker *w, pthread_t *th,
                       bool with_bulk)
{
    const size_t n = run->threads + (with_bulk ? 1 : 0);
    for (size_t i = 0; i < n; i++) {
        w[i].run = run;
        w[i].index = i;
        void *(*fn)(void *) = with_bulk && i + 1 == n ? bulk_worker
                                                      : metadata_worker;
        PGW_ASSERT_EQ_I(pthread_create(&th[i], NULL, fn, &w[i]), 0);
    }
}

static void load_join(load_run *run, pthread_t *th, bool with_bulk)
{
    const size_t n = run->threads + (with_bulk ? 1 : 0);
    for (size_t i = 0; i < n; i++)
        PGW_ASSERT_EQ_I(pthread_join(th[i], NULL), 0);
}

/* Every worker must have finished cleanly and left no descriptor behind. */
static void expect_workers_clean(const load_worker *w, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        CAS_ASSERT_MSG(w[i].verdict == PGW_OK,
                       "worker %zu failed under load with %s", i,
                       pgw_err_name(w[i].verdict));
        CAS_ASSERT_MSG(w[i].handles_left == 0,
                       "worker %zu left %zu backend handles open", i,
                       w[i].handles_left);
    }
}

/* PERF-05, ROB-02, ROB-03 (gap 63): the mount survives a continuous workload. */
static void test_load_mount_survives_a_continuous_workload(void)
{
    load_run run;
    memset(&run, 0, sizeof(run));
    char *root = pgw_test_tmpdir("pgwcas-load");
    PGW_ASSERT(root != NULL);
    snprintf(run.root, sizeof(run.root), "%s", root);
    prepare_truth_tree(run.root);

    run.threads = load_threads();
    run.budget_ns = load_seconds() * 1000000000ULL;
    run.fd_baseline = cas_fd_count();
    const size_t n = run.threads + 1;
    load_worker *w = calloc(n, sizeof(*w));
    pthread_t *th = calloc(n, sizeof(*th));
    PGW_ASSERT(w != NULL && th != NULL);

    const uint64_t t0 = pgw_now_ns();
    run.deadline_ns = t0 + run.budget_ns;
    load_start(&run, w, th, true);
    priority_probe(&run, run.deadline_ns);
    load_join(&run, th, true);
    const uint64_t elapsed_ns = pgw_now_ns() - t0;

    size_t ops = 0;
    for (size_t i = 0; i < run.threads; i++) {
        CAS_ASSERT_MSG(w[i].ops > 0, "worker %zu did no work at all", i);
        ops += w[i].ops;
    }
    CAS_ASSERT_MSG(w[n - 1].copied == SRC_BYTES,
                   "the bulk copy moved %llu of %u bytes",
                   (unsigned long long)w[n - 1].copied, SRC_BYTES);
    expect_workers_clean(w, n);

    /* No deadlock: the whole workload is back inside the budget, plus a wide
     * margin for a slow or instrumented machine. */
    CAS_ASSERT_MSG(elapsed_ns < run.budget_ns + 30000000000ULL,
                   "the workload did not finish in %llu s: a deadlock?",
                   (unsigned long long)(elapsed_ns / 1000000000ULL));

    /* No unbounded descriptor growth, and everything is back afterwards. */
    CAS_ASSERT_MSG(run.fd_peak <= run.fd_baseline + FD_HEADROOM,
                   "descriptors grew from %zu to %zu under load",
                   run.fd_baseline, run.fd_peak);
    CAS_ASSERT_MSG(cas_fd_count() <= run.fd_baseline + 8,
                   "descriptors did not come back: %zu now, %zu before",
                   cas_fd_count(), run.fd_baseline);

    printf("# load: %zu metadata clients, %zu operations, %llu MiB copied, "
           "%llu ms of budget\n",
           run.threads, ops, (unsigned long long)(SRC_BYTES >> 20),
           (unsigned long long)(elapsed_ns / 1000000ULL));

    /* The copy is byte exact on the truth side: read it whole, then check
     * every chunk against the pattern it was written with. */
    uint8_t *whole = malloc(SRC_BYTES);
    PGW_ASSERT(whole != NULL);
    size_t len = 0;
    PGW_ASSERT(cas_truth_read(run.root, "copy.bin", whole, SRC_BYTES, &len));
    CAS_ASSERT_MSG(len == SRC_BYTES, "the copy holds %zu bytes, expected %u",
                   len, SRC_BYTES);
    for (uint64_t off = 0; off < SRC_BYTES; off += CHUNK)
        CAS_ASSERT_MSG(pgw_test_pattern_ok(whole + off, CHUNK, 42, off),
                       "the copy differs from the source at offset %llu",
                       (unsigned long long)off);
    free(whole);

    /* And the tree the layer reports is still the tree the PC holds. */
    cas_mount check;
    PGW_ASSERT(cas_mount_open_at(&check, run.root, NULL, NULL));
    char err[512];
    err[0] = '\0';
    CAS_ASSERT_MSG(diff_verify_tree(check.fs, check.root, err, sizeof(err)) ==
                       PGW_OK,
                   "after the load the tree diverges: %s", err);
    cas_mount_close(&check);
    free(w);
    free(th);
}
PGW_TEST(test_load_mount_survives_a_continuous_workload);

/* PERF-05 (gap 63): a high-priority read stays bounded while a bulk copy
 * saturates the same share. This is the PC side of the claim: the scheduler
 * itself is proven by rmfs/tests/test_rmfs_sched.c, the QNX end-to-end latency
 * is not proven here and the report says so. */
static void test_load_high_priority_read_under_bulk_copy(void)
{
    load_run run;
    memset(&run, 0, sizeof(run));
    char *root = pgw_test_tmpdir("pgwcas-loadprio");
    PGW_ASSERT(root != NULL);
    snprintf(run.root, sizeof(run.root), "%s", root);
    prepare_truth_tree(run.root);

    run.threads = 1; /* the bulk copy is the load here */
    run.budget_ns = load_seconds() * 1000000000ULL;
    run.fd_baseline = cas_fd_count();
    load_worker w[2];
    pthread_t th[2];
    memset(w, 0, sizeof(w));

    const uint64_t t0 = pgw_now_ns();
    run.deadline_ns = t0 + run.budget_ns;
    w[0].run = &run;
    w[0].index = 0;
    PGW_ASSERT_EQ_I(pthread_create(&th[0], NULL, bulk_worker, &w[0]), 0);
    priority_probe(&run, run.deadline_ns);
    PGW_ASSERT_EQ_I(pthread_join(th[0], NULL), 0);

    CAS_ASSERT_MSG(w[0].verdict == PGW_OK, "the bulk copy failed with %s",
                   pgw_err_name(w[0].verdict));
    CAS_ASSERT_MSG(run.max_latency_ns > 0,
                   "the high-priority client never completed a read");
    printf("# priority read under a bulk copy: worst latency %llu us\n",
           (unsigned long long)(run.max_latency_ns / 1000ULL));
    CAS_ASSERT_MSG(run.max_latency_ns < LATENCY_BOUND_NS,
                   "a high-priority 4 KiB read took %llu ms under load, more "
                   "than the %llu ms bound: a priority inversion?",
                   (unsigned long long)(run.max_latency_ns / 1000000ULL),
                   (unsigned long long)(LATENCY_BOUND_NS / 1000000ULL));
}
PGW_TEST(test_load_high_priority_read_under_bulk_copy);

/* ROB-02, FS-11 (gap 63): mounting and unmounting under work leaks no
 * descriptor and no backend handle - the growth a long session would show. */
static void test_load_no_descriptor_leak_across_remounts(void)
{
    char *root = pgw_test_tmpdir("pgwcas-remount");
    PGW_ASSERT(root != NULL);
    const size_t baseline = cas_fd_count();
    for (unsigned cycle = 0; cycle < 25; cycle++) {
        cas_mount m;
        PGW_ASSERT(cas_mount_open_at(&m, root, NULL, NULL));
        char rel[32];
        for (size_t i = 0; i < 8; i++) {
            snprintf(rel, sizeof(rel), "f%zu", i);
            pgw_rmfs_fd *fd = NULL;
            PGW_ASSERT_EQ_I(pgw_rmfs_open(m.fs, &m.cred, rel,
                                          PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                            PGW_OK);
            size_t n = 0;
            PGW_ASSERT_EQ_I(pgw_rmfs_write(m.fs, fd, 0, "x", 1, &n), PGW_OK);
            PGW_ASSERT_EQ_I(pgw_rmfs_fsync(m.fs, fd), PGW_OK);
            PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
            PGW_ASSERT_EQ_I(pgw_rmfs_unlink(m.fs, &m.cred, rel, false),
                            PGW_OK);
        }
        PGW_ASSERT_EQ_U(pgw_rmfs_open_count(m.fs), 0);
        PGW_ASSERT_EQ_U(pgw_posix_backend_open_handles(m.be), 0);
        cas_mount_close(&m);
        CAS_ASSERT_MSG(cas_fd_count() <= baseline + 4,
                       "cycle %u leaked descriptors: %zu now, %zu at start",
                       cycle, cas_fd_count(), baseline);
    }
    PGW_ASSERT_EQ_U(cas_truth_count(root, "."), 0);
}
PGW_TEST(test_load_no_descriptor_leak_across_remounts);
