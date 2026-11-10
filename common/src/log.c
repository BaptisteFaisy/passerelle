/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/src/log.c - levelled logging and atomic counters.
 */
#include "pgw/log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int g_level = PGW_LOG_WARN;
static pgw_log_fn g_backend = NULL;
static void *g_backend_user = NULL;
static _Atomic uint64_t g_stats[PGW_STAT__MAX];

static const char *const g_lvl_names[] = {
    "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

static const char *const g_stat_names[PGW_STAT__MAX] = {
    [PGW_STAT_REQ] = "requests",
    [PGW_STAT_RESP] = "responses",
    [PGW_STAT_ERR] = "errors",
    [PGW_STAT_TIMEOUT] = "timeouts",
    [PGW_STAT_FLUSH] = "flushes",
    [PGW_STAT_RETRY] = "retries",
    [PGW_STAT_SPLIT] = "splits",
    [PGW_STAT_QUEUE_FULL] = "queue_full",
    [PGW_STAT_BAD_RESP] = "bad_responses",
    [PGW_STAT_INODE_HIT] = "attr_hits",
    [PGW_STAT_INODE_MISS] = "attr_misses",
    [PGW_STAT_BYTES_READ] = "bytes_read",
    [PGW_STAT_BYTES_WRITTEN] = "bytes_written",
};

void pgw_log_set_level(pgw_level lvl)
{
    atomic_store_explicit(&g_level, (int)lvl, memory_order_relaxed);
}

pgw_level pgw_log_level(void)
{
    return (pgw_level)atomic_load_explicit(&g_level, memory_order_relaxed);
}

void pgw_log_set_backend(pgw_log_fn fn, void *user)
{
    g_backend = fn;
    g_backend_user = user;
}

bool pgw_log_enabled(pgw_level lvl)
{
    return (int)lvl <= atomic_load_explicit(&g_level, memory_order_relaxed);
}

void pgw_log_write(pgw_level lvl, const char *fmt, ...)
{
    if (!pgw_log_enabled(lvl) || fmt == NULL)
        return;
    char buf[512];
    const char *tag = (lvl >= 0 && lvl <= PGW_LOG_TRACE) ? g_lvl_names[lvl] : "?";
    int n = snprintf(buf, sizeof(buf), "[%s] ", tag);
    if (n < 0 || (size_t)n >= sizeof(buf))
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    pgw_log_fn fn = g_backend;
    if (fn != NULL) {
        fn(lvl, buf, g_backend_user);
    } else {
        fprintf(stderr, "%s\n", buf);
    }
}

static bool ci_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

bool pgw_log_parse_level(const char *text, pgw_level *out)
{
    if (text == NULL || out == NULL)
        return false;
    for (int i = 0; i <= PGW_LOG_TRACE; i++) {
        if (ci_eq(text, g_lvl_names[i])) {
            *out = (pgw_level)i;
            return true;
        }
    }
    return false;
}

void pgw_stat_add(pgw_stat which, uint64_t delta)
{
    if (which < 0 || which >= PGW_STAT__MAX)
        return;
    atomic_fetch_add_explicit(&g_stats[which], delta, memory_order_relaxed);
}

uint64_t pgw_stat_get(pgw_stat which)
{
    if (which < 0 || which >= PGW_STAT__MAX)
        return 0;
    return atomic_load_explicit(&g_stats[which], memory_order_relaxed);
}

void pgw_stat_reset(void)
{
    for (int i = 0; i < PGW_STAT__MAX; i++)
        atomic_store_explicit(&g_stats[i], 0, memory_order_relaxed);
}

const char *pgw_stat_name(pgw_stat which)
{
    if (which < 0 || which >= PGW_STAT__MAX)
        return "unknown";
    return g_stat_names[which];
}

size_t pgw_stat_format(char *out, size_t cap)
{
    size_t pos = 0;
    for (int i = 0; i < PGW_STAT__MAX; i++) {
        uint64_t v = pgw_stat_get((pgw_stat)i);
        char line[96];
        int n = snprintf(line, sizeof(line), "%s=%llu\n",
                         g_stat_names[i], (unsigned long long)v);
        if (n <= 0)
            continue;
        size_t len = (size_t)n;
        if (out != NULL && pos < cap) {
            size_t avail = cap - pos - 1;
            size_t take = len < avail ? len : avail;
            if (take > 0)
                memcpy(out + pos, line, take);
        }
        pos += len;
    }
    if (out != NULL && cap > 0)
        out[pos < cap ? pos : cap - 1] = '\0';
    return pos;
}
