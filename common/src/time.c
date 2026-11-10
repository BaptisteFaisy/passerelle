/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/src/time.c - monotonic clock and deadlines.
 */
#define _POSIX_C_SOURCE 200809L
#include "pgw/time.h"

#include <limits.h>
#include <time.h>

static pgw_clock_fn g_clock = NULL;
static void *g_clock_user = NULL;

uint64_t pgw_now_ns(void)
{
    if (g_clock != NULL)
        return g_clock(g_clock_user);
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void pgw_time_set_clock(pgw_clock_fn fn, void *user)
{
    g_clock = fn;
    g_clock_user = user;
}

bool pgw_time_advance(uint64_t ns)
{
    if (g_clock == NULL || g_clock_user == NULL)
        return false;
    uint64_t *fake = (uint64_t *)g_clock_user;
    *fake += ns;
    return true;
}

pgw_deadline pgw_deadline_from_ms(uint64_t timeout_ms)
{
    pgw_deadline d;
    if (timeout_ms == 0) {
        d.due_ns = 0;
        d.forever = true;
        return d;
    }
    uint64_t now = pgw_now_ns();
    uint64_t delta = pgw_ms_to_ns(timeout_ms);
    d.due_ns = (UINT64_MAX - now < delta) ? UINT64_MAX : now + delta;
    d.forever = false;
    return d;
}

bool pgw_deadline_expired(pgw_deadline d)
{
    if (d.forever)
        return false;
    return pgw_now_ns() >= d.due_ns;
}

uint64_t pgw_deadline_left_ns(pgw_deadline d)
{
    if (d.forever)
        return UINT64_MAX;
    uint64_t now = pgw_now_ns();
    return (now >= d.due_ns) ? 0 : (d.due_ns - now);
}

int32_t pgw_deadline_left_ms(pgw_deadline d)
{
    if (d.forever)
        return -1;
    uint64_t left = pgw_deadline_left_ns(d);
    if (left == 0)
        return 0;
    uint64_t ms = (left + 999999ULL) / 1000000ULL;
    return (ms > (uint64_t)INT32_MAX) ? INT32_MAX : (int32_t)ms;
}

uint64_t pgw_ms_to_ns(uint64_t ms)
{
    if (ms > UINT64_MAX / 1000000ULL)
        return UINT64_MAX;
    return ms * 1000000ULL;
}

uint64_t pgw_sec_to_ns(uint64_t s)
{
    if (s > UINT64_MAX / 1000000000ULL)
        return UINT64_MAX;
    return s * 1000000000ULL;
}

void pgw_ns_split(uint64_t ns, uint64_t *sec, uint32_t *nsec)
{
    if (sec != NULL)
        *sec = ns / 1000000000ULL;
    if (nsec != NULL)
        *nsec = (uint32_t)(ns % 1000000000ULL);
}

uint64_t pgw_ns_join(uint64_t sec, uint32_t nsec)
{
    uint64_t hi = pgw_sec_to_ns(sec);
    if (UINT64_MAX - hi < (uint64_t)nsec)
        return UINT64_MAX;
    return hi + (uint64_t)nsec;
}

uint64_t pgw_elapsed_ns(uint64_t start_ns)
{
    uint64_t now = pgw_now_ns();
    return (now >= start_ns) ? (now - start_ns) : 0;
}
