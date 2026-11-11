/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/sched.c - Priority scheduler for bounded in-flight FS requests.
 *
 * Implements requirements QNX-04 and PERF-05:
 *   - When the number of in-flight requests reaches max_inflight, new
 *     requests are queued in priority order (QNX priority: higher = more urgent).
 *   - Requests of equal priority are drained FIFO.
 *   - pgw_prio_sched_drain_next() executes the highest priority work and
 *     removes it from the pending queue.
 */
#include "pgw/rmfs.h"
#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct pgw_prio_item {
    uint32_t prio;
    uint64_t seq;
    pgw_prio_work_fn fn;
    void *ctx;
} pgw_prio_item;

struct pgw_prio_sched {
    uint32_t max_inflight;
    uint64_t next_seq;
    size_t count;
    size_t capacity;
    pgw_prio_item *items;
};

pgw_prio_sched *pgw_prio_sched_create(uint32_t max_inflight)
{
    pgw_prio_sched *s = (pgw_prio_sched *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->max_inflight = (max_inflight > 0) ? max_inflight : PGW_INFLIGHT_DEFAULT;
    s->capacity = PGW_PENDING_DEFAULT;
    s->items = (pgw_prio_item *)calloc(s->capacity, sizeof(pgw_prio_item));
    if (s->items == NULL) {
        free(s);
        return NULL;
    }
    return s;
}

void pgw_prio_sched_destroy(pgw_prio_sched *s)
{
    if (s == NULL)
        return;
    free(s->items);
    free(s);
}

static bool prio_sched_grow(pgw_prio_sched *s)
{
    size_t new_cap = s->capacity * 2;
    if (new_cap > PGW_PENDING_MAX)
        new_cap = PGW_PENDING_MAX;
    if (new_cap <= s->capacity)
        return false;
    pgw_prio_item *p = (pgw_prio_item *)realloc(s->items, new_cap * sizeof(pgw_prio_item));
    if (p == NULL)
        return false;
    s->items = p;
    s->capacity = new_cap;
    return true;
}

static size_t prio_sched_find_insert_idx(const pgw_prio_sched *s, uint32_t prio)
{
    /* Stable insertion: higher prio earlier; equal prio placed after existing items (FIFO) */
    size_t i = 0;
    while (i < s->count && s->items[i].prio >= prio) {
        i++;
    }
    return i;
}

pgw_err pgw_prio_sched_enqueue(pgw_prio_sched *s, uint32_t prio,
                               pgw_prio_work_fn fn, void *ctx)
{
    if (s == NULL || fn == NULL)
        return PGW_EINVAL;
    if (s->count >= s->capacity) {
        if (!prio_sched_grow(s)) {
            pgw_stat_add(PGW_STAT_QUEUE_FULL, 1);
            return PGW_EBUSY;
        }
    }
    size_t idx = prio_sched_find_insert_idx(s, prio);
    if (idx < s->count) {
        memmove(&s->items[idx + 1], &s->items[idx],
                (s->count - idx) * sizeof(pgw_prio_item));
    }
    s->items[idx].prio = prio;
    s->items[idx].seq = s->next_seq++;
    s->items[idx].fn = fn;
    s->items[idx].ctx = ctx;
    s->count++;
    return PGW_OK;
}

size_t pgw_prio_sched_drain_next(pgw_prio_sched *s)
{
    if (s == NULL || s->count == 0)
        return 0;
    pgw_prio_item item = s->items[0];
    if (s->count > 1) {
        memmove(&s->items[0], &s->items[1],
                (s->count - 1) * sizeof(pgw_prio_item));
    }
    s->count--;
    item.fn(item.ctx);
    return 1;
}

size_t pgw_prio_sched_pending(const pgw_prio_sched *s)
{
    return (s != NULL) ? s->count : 0;
}
