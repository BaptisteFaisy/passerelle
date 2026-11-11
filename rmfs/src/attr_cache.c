/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/attr_cache.c - Optional, TTL-bounded inode attribute cache.
 *
 * Implements requirements D4, FS-12, FS-13:
 *   - Cache is optional (ttl_ns == 0 disables it entirely): the PC folder is
 *     the truth, a modified file must be seen modified on the next read.
 *   - When enabled, validity is bounded by ttl_ns, itself clamped to
 *     PGW_ATTR_CACHE_NS_MAX at mount time by the core (rmfs.c).
 *   - Only attributes are cached, never data (D4).
 *   - Hits and misses feed PGW_STAT_INODE_HIT / PGW_STAT_INODE_MISS.
 *
 * Expiry uses the injectable monotonic clock pgw_now_ns(), so tests exercise
 * the TTL with pgw_time_advance() and never sleep.
 */
#include "attr_cache.h"

#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATTR_CACHE_BUCKETS 256u
#define ATTR_CACHE_MAX_ENTRIES 1024u

typedef struct attr_entry {
    struct attr_entry *next;
    uint64_t expires_ns;
    uint64_t stamp; /* insertion order for eviction */
    char path[PGW_PATH_MAX + 2]; /* "s:" + a maximal path */
    pgw_fs_attr attr;
} attr_entry;

struct rmfs_attr_cache {
    uint64_t ttl_ns;
    uint64_t next_stamp;
    size_t count;
    attr_entry *buckets[ATTR_CACHE_BUCKETS];
};

static uint32_t attr_hash(const char *s)
{
    /* FNV-1a, small and dependency-free */
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h % ATTR_CACHE_BUCKETS;
}

rmfs_attr_cache *rmfs_attr_cache_create(uint64_t ttl_ns)
{
    rmfs_attr_cache *c = (rmfs_attr_cache *)calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->ttl_ns = ttl_ns;
    return c;
}

static void attr_entry_free_chain(attr_entry *e)
{
    while (e != NULL) {
        attr_entry *next = e->next;
        free(e);
        e = next;
    }
}

void rmfs_attr_cache_destroy(rmfs_attr_cache *c)
{
    if (c == NULL)
        return;
    for (size_t i = 0; i < ATTR_CACHE_BUCKETS; i++)
        attr_entry_free_chain(c->buckets[i]);
    free(c);
}

/* Unlink and free an entry; caller owns the bucket head update. */
static void attr_unlink(rmfs_attr_cache *c, uint32_t b, attr_entry *prev,
                        attr_entry *e)
{
    if (prev == NULL)
        c->buckets[b] = e->next;
    else
        prev->next = e->next;
    c->count--;
    free(e);
}

bool rmfs_attr_cache_get(rmfs_attr_cache *c, const char *relpath,
                         pgw_fs_attr *out)
{
    if (c == NULL || c->ttl_ns == 0 || relpath == NULL || out == NULL) {
        pgw_stat_add(PGW_STAT_INODE_MISS, 1);
        return false;
    }
    uint32_t b = attr_hash(relpath);
    uint64_t now = pgw_now_ns();
    attr_entry *prev = NULL;
    for (attr_entry *e = c->buckets[b]; e != NULL; prev = e, e = e->next) {
        if (strcmp(e->path, relpath) != 0)
            continue;
        if (now >= e->expires_ns) {
            attr_unlink(c, b, prev, e); /* expired: treat as miss (FS-13) */
            pgw_stat_add(PGW_STAT_INODE_MISS, 1);
            return false;
        }
        *out = e->attr;
        pgw_stat_add(PGW_STAT_INODE_HIT, 1);
        return true;
    }
    pgw_stat_add(PGW_STAT_INODE_MISS, 1);
    return false;
}

/* Drop one entry when the table is at its bound: expired first, else oldest. */
static void attr_evict_one(rmfs_attr_cache *c)
{
    uint64_t now = pgw_now_ns();
    attr_entry *oldest = NULL;
    attr_entry *oldest_prev = NULL;
    uint32_t oldest_b = 0;
    for (uint32_t b = 0; b < ATTR_CACHE_BUCKETS; b++) {
        attr_entry *prev = NULL;
        for (attr_entry *e = c->buckets[b]; e != NULL; prev = e, e = e->next) {
            if (now >= e->expires_ns) {
                attr_unlink(c, b, prev, e);
                return;
            }
            if (oldest == NULL || e->stamp < oldest->stamp) {
                oldest = e;
                oldest_prev = prev;
                oldest_b = b;
            }
        }
    }
    if (oldest != NULL)
        attr_unlink(c, oldest_b, oldest_prev, oldest);
}

void rmfs_attr_cache_put(rmfs_attr_cache *c, const char *relpath,
                         const pgw_fs_attr *attr)
{
    if (c == NULL || c->ttl_ns == 0 || relpath == NULL || attr == NULL)
        return;
    rmfs_attr_cache_invalidate(c, relpath); /* replace any previous entry */
    while (c->count >= ATTR_CACHE_MAX_ENTRIES)
        attr_evict_one(c);
    attr_entry *e = (attr_entry *)calloc(1, sizeof(*e));
    if (e == NULL)
        return;
    snprintf(e->path, sizeof(e->path), "%s", relpath);
    e->attr = *attr;
    e->stamp = c->next_stamp++;
    e->expires_ns = pgw_now_ns() + c->ttl_ns;
    uint32_t b = attr_hash(relpath);
    e->next = c->buckets[b];
    c->buckets[b] = e;
    c->count++;
}

void rmfs_attr_cache_invalidate(rmfs_attr_cache *c, const char *relpath)
{
    if (c == NULL || relpath == NULL)
        return;
    uint32_t b = attr_hash(relpath);
    attr_entry *prev = NULL;
    for (attr_entry *e = c->buckets[b]; e != NULL; prev = e, e = e->next) {
        if (strcmp(e->path, relpath) == 0) {
            attr_unlink(c, b, prev, e);
            return;
        }
    }
}

void rmfs_attr_cache_clear(rmfs_attr_cache *c)
{
    if (c == NULL)
        return;
    for (size_t i = 0; i < ATTR_CACHE_BUCKETS; i++) {
        attr_entry_free_chain(c->buckets[i]);
        c->buckets[i] = NULL;
    }
    c->count = 0;
}
