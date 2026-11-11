/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_sched.c - Priority-ordered submission scheduler.
 *
 * Covers requirements QNX-04 (when the queue is full, pending requests leave
 * in priority order), QNX-05 (a blocked client does not hold a driver thread:
 * enqueue returns at once, the work only runs when a slot frees) and PERF-05
 * (a priority 4 KiB read issued during a bulk low-priority copy is not delayed
 * behind the whole copy).
 *
 * The test checks the *order of processing*, never durations, so it is stable
 * on any machine and needs no sleep.
 */
#include "../src/rmfs_internal.h"

#include "fake_backend.h"

#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <string.h>

#define MAX_EVENTS 32

/* Records the order in which the queued work ran. */
typedef struct trace {
    int order[MAX_EVENTS];
    size_t count;
} trace;

static void record(void *ctx)
{
    trace *t = (trace *)ctx;
    t->order[t->count++] = 1;
}

/* One work item per priority, each writing its own tag. */
typedef struct item {
    trace *t;
    int tag;
} item;

static void record_tag(void *ctx)
{
    item *it = (item *)ctx;
    it->t->order[it->t->count++] = it->tag;
}

/* --- QNX-04 / PERF-05: priority order ------------------------------------ */

static void test_high_priority_overtakes_bulk(void)
{
    pgw_prio_sched *s = pgw_prio_sched_create(2); /* two transport slots */
    PGW_ASSERT(s != NULL);
    trace t = {{0}, 0};
    item low[8];
    item high = {&t, 99};

    /* A bulk copy is already waiting: eight low-priority requests (prio 5). */
    for (int i = 0; i < 8; i++) {
        low[i].t = &t;
        low[i].tag = i;
        PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 5, record_tag, &low[i]),
                        PGW_OK);
    }
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(s), 8);

    /* The 4 KiB priority read arrives last but must leave first (PERF-05). */
    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 60, record_tag, &high), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(s), 9);

    PGW_ASSERT_EQ_U(pgw_prio_sched_drain_next(s), 1);
    PGW_ASSERT_EQ_I(t.order[0], 99); /* the high-priority request went first */

    /* The rest follow in their original (FIFO) order. */
    for (int i = 0; i < 8; i++)
        PGW_ASSERT_EQ_U(pgw_prio_sched_drain_next(s), 1);
    PGW_ASSERT_EQ_U(t.count, 9);
    for (int i = 0; i < 8; i++)
        PGW_ASSERT_EQ_I(t.order[1 + i], i);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(s), 0);

    pgw_prio_sched_destroy(s);
}

static void test_equal_priority_is_fifo(void)
{
    pgw_prio_sched *s = pgw_prio_sched_create(1);
    PGW_ASSERT(s != NULL);
    trace t = {{0}, 0};
    item items[5];
    for (int i = 0; i < 5; i++) {
        items[i].t = &t;
        items[i].tag = i;
        PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 10, record_tag, &items[i]),
                        PGW_OK);
    }
    while (pgw_prio_sched_drain_next(s) == 1) {
        /* drain everything */
    }
    PGW_ASSERT_EQ_U(t.count, 5);
    for (int i = 0; i < 5; i++)
        PGW_ASSERT_EQ_I(t.order[i], i);

    pgw_prio_sched_destroy(s);
}

/* --- QNX-05: enqueue does not run the work ------------------------------- */

static void test_enqueue_does_not_block(void)
{
    pgw_prio_sched *s = pgw_prio_sched_create(1);
    PGW_ASSERT(s != NULL);
    trace t = {{0}, 0};

    /* The caller returns immediately; nothing has run yet (QNX-05: the
     * request is parked, the driver thread is free). */
    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 10, record, &t), PGW_OK);
    PGW_ASSERT_EQ_U(t.count, 0);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(s), 1);

    /* Draining is what runs it, in a thread that owns a free slot. */
    PGW_ASSERT_EQ_U(pgw_prio_sched_drain_next(s), 1);
    PGW_ASSERT_EQ_U(t.count, 1);
    PGW_ASSERT_EQ_U(pgw_prio_sched_drain_next(s), 0);

    pgw_prio_sched_destroy(s);
}

/* --- bounded queue -------------------------------------------------------- */

static void test_queue_is_bounded(void)
{
    pgw_prio_sched *s = pgw_prio_sched_create(4);
    PGW_ASSERT(s != NULL);
    trace t = {{0}, 0};

    /* Fill up to the documented ceiling; every enqueue succeeds. */
    for (size_t i = 0; i < PGW_PENDING_MAX; i++)
        PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 1, record, &t), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(s), PGW_PENDING_MAX);

    /* Past the ceiling the request is refused instead of growing forever. */
    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 1, record, &t), PGW_EBUSY);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(s), PGW_PENDING_MAX);

    pgw_prio_sched_destroy(s);
}

static void test_priority_range_spans_low_and_high(void)
{
    pgw_prio_sched *s = pgw_prio_sched_create(8);
    PGW_ASSERT(s != NULL);
    trace t = {{0}, 0};
    item a = {&t, 1};
    item b = {&t, 2};
    item c = {&t, 3};

    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 1, record_tag, &a), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 63, record_tag, &c), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 32, record_tag, &b), PGW_OK);
    while (pgw_prio_sched_drain_next(s) == 1) {
        /* drain everything */
    }
    PGW_ASSERT_EQ_U(t.count, 3);
    PGW_ASSERT_EQ_I(t.order[0], 3); /* prio 63 */
    PGW_ASSERT_EQ_I(t.order[1], 2); /* prio 32 */
    PGW_ASSERT_EQ_I(t.order[2], 1); /* prio 1  */

    pgw_prio_sched_destroy(s);
}

static void test_null_arguments_are_rejected(void)
{
    pgw_prio_sched *dflt = pgw_prio_sched_create(0); /* 0 -> default */
    PGW_ASSERT(dflt != NULL);
    pgw_prio_sched_destroy(dflt);
    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(NULL, 1, record, NULL), PGW_EINVAL);
    pgw_prio_sched *s = pgw_prio_sched_create(1);
    PGW_ASSERT(s != NULL);
    PGW_ASSERT_EQ_I(pgw_prio_sched_enqueue(s, 1, NULL, NULL), PGW_EINVAL);
    PGW_ASSERT_EQ_U(pgw_prio_sched_drain_next(NULL), 0);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(NULL), 0);
    pgw_prio_sched_destroy(NULL); /* must not crash */
    pgw_prio_sched_destroy(s);
}

/* --- saturation through the core: a parked request still completes ------- */

typedef struct nest_ctx {
    pgw_rmfs *fs;
    int inner_ran;
} nest_ctx;

static pgw_err inner_work(void *arg)
{
    nest_ctx *c = (nest_ctx *)arg;
    c->inner_ran++;
    return PGW_OK;
}

static pgw_err outer_work(void *arg)
{
    nest_ctx *c = (nest_ctx *)arg;
    /* With max_inflight = 1 the slot is taken, so this request is parked and
     * drained by priority (QNX-04): it must still complete. */
    return rmfs_run(c->fs, 1, inner_work, c);
}

static void test_core_runs_when_saturated(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.max_inflight = 1;
    pgw_rmfs *fs = pgw_rmfs_create(b->ops, b, &opts);
    PGW_ASSERT(fs != NULL);
    PGW_ASSERT_EQ_U(pgw_rmfs_opts(fs)->max_inflight, 1);

    nest_ctx c = {fs, 0};
    uint64_t full0 = pgw_stat_get(PGW_STAT_QUEUE_FULL);
    PGW_ASSERT_EQ_I(rmfs_run(fs, 10, outer_work, &c), PGW_OK);
    PGW_ASSERT_EQ_I(c.inner_ran, 1);
    /* The saturation was counted (QNX-08 exposes it). */
    PGW_ASSERT(pgw_stat_get(PGW_STAT_QUEUE_FULL) > full0);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(fs->sched), 0);

    /* Once the slot is free again the fast path is used: nothing is queued. */
    PGW_ASSERT_EQ_I(rmfs_run(fs, 10, inner_work, &c), PGW_OK);
    PGW_ASSERT_EQ_I(c.inner_ran, 2);
    PGW_ASSERT_EQ_U(pgw_prio_sched_pending(fs->sched), 0);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_core_runs_when_saturated);
PGW_TEST(test_high_priority_overtakes_bulk);
PGW_TEST(test_equal_priority_is_fifo);
PGW_TEST(test_enqueue_does_not_block);
PGW_TEST(test_queue_is_bounded);
PGW_TEST(test_priority_range_spans_low_and_high);
PGW_TEST(test_null_arguments_are_rejected);
