/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/lock.c - POSIX-like byte-range locks local to the guest.
 *
 * Implements requirement FS-14:
 *   - Shared read locks (PGW_LOCK_RD) and exclusive write locks (PGW_LOCK_WR)
 *   - Unlocking (PGW_LOCK_UN) splits, truncates or deletes held ranges
 *   - Conflict checks: a write lock conflicts with any lock from another PID;
 *     a read lock conflicts with a write lock from another PID.
 *   - Process cleanup (ROB-02): rmfs_lock_release_pid() frees all locks of a
 *     given owner_pid.
 *   - Ranges: len == 0 means "from start to infinity (EOF)".
 */
#include "lock.h"

#include "pgw/limits.h"
#include "pgw/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rmfs_range_lock {
    struct rmfs_range_lock *next;
    char path[PGW_PATH_MAX];
    uint32_t owner_pid;
    pgw_lock_type type;
    uint64_t start;
    uint64_t len; /* 0 = to EOF (UINT64_MAX) */
} rmfs_range_lock;

struct rmfs_lock_table {
    rmfs_range_lock *head;
};

rmfs_lock_table *rmfs_lock_table_create(void)
{
    return (rmfs_lock_table *)calloc(1, sizeof(rmfs_lock_table));
}

void rmfs_lock_table_destroy(rmfs_lock_table *t)
{
    if (t == NULL)
        return;
    rmfs_range_lock *cur = t->head;
    while (cur != NULL) {
        rmfs_range_lock *next = cur->next;
        free(cur);
        cur = next;
    }
    free(t);
}

static bool ranges_overlap(uint64_t s1, uint64_t l1, uint64_t s2, uint64_t l2)
{
    uint64_t e1 = (l1 == 0 || UINT64_MAX - s1 < l1) ? UINT64_MAX : s1 + l1;
    uint64_t e2 = (l2 == 0 || UINT64_MAX - s2 < l2) ? UINT64_MAX : s2 + l2;
    return (s1 < e2) && (s2 < e1);
}

static bool locks_conflict(const rmfs_range_lock *held, uint32_t req_pid,
                           pgw_lock_type req_type, uint64_t start, uint64_t len)
{
    if (held->owner_pid == req_pid)
        return false;
    if (!ranges_overlap(held->start, held->len, start, len))
        return false;
    if (held->type == PGW_LOCK_WR || req_type == PGW_LOCK_WR)
        return true;
    return false;
}

pgw_err rmfs_lock_test(rmfs_lock_table *t, const char *path, uint32_t owner_pid,
                       pgw_lock_type *type, uint64_t *start, uint64_t *len,
                       uint32_t *holder_pid)
{
    if (t == NULL || path == NULL || type == NULL || start == NULL ||
        len == NULL || holder_pid == NULL)
        return PGW_EINVAL;

    for (rmfs_range_lock *cur = t->head; cur != NULL; cur = cur->next) {
        if (strcmp(cur->path, path) != 0)
            continue;
        if (locks_conflict(cur, owner_pid, *type, *start, *len)) {
            *type = cur->type;
            *start = cur->start;
            *len = cur->len;
            *holder_pid = cur->owner_pid;
            return PGW_OK;
        }
    }
    *type = PGW_LOCK_UN;
    *holder_pid = 0;
    return PGW_OK;
}

static void lock_add(rmfs_lock_table *t, const char *path, uint32_t owner_pid,
                     pgw_lock_type type, uint64_t start, uint64_t len)
{
    rmfs_range_lock *lk = (rmfs_range_lock *)calloc(1, sizeof(*lk));
    if (lk == NULL)
        return;
    snprintf(lk->path, sizeof(lk->path), "%s", path);
    lk->owner_pid = owner_pid;
    lk->type = type;
    lk->start = start;
    lk->len = len;
    lk->next = t->head;
    t->head = lk;
}

/* Removes or shrinks ranges owned by owner_pid on path that overlap [start, len) */
static void lock_unlock_range(rmfs_lock_table *t, const char *path,
                              uint32_t owner_pid, uint64_t start, uint64_t len)
{
    uint64_t u_end = (len == 0 || UINT64_MAX - start < len) ? UINT64_MAX : start + len;
    rmfs_range_lock **prev = &t->head;
    while (*prev != NULL) {
        rmfs_range_lock *cur = *prev;
        if (strcmp(cur->path, path) != 0 || cur->owner_pid != owner_pid) {
            prev = &cur->next;
            continue;
        }
        uint64_t c_end = (cur->len == 0 || UINT64_MAX - cur->start < cur->len)
                         ? UINT64_MAX : cur->start + cur->len;
        if (!ranges_overlap(cur->start, cur->len, start, len)) {
            prev = &cur->next;
            continue;
        }
        /* Case 1: unl covers entire held range */
        if (start <= cur->start && u_end >= c_end) {
            *prev = cur->next;
            free(cur);
            continue;
        }
        /* Case 2: unl chops suffix */
        if (start > cur->start && u_end >= c_end) {
            cur->len = start - cur->start;
            prev = &cur->next;
            continue;
        }
        /* Case 3: unl chops prefix */
        if (start <= cur->start && u_end < c_end) {
            cur->start = u_end;
            cur->len = (c_end == UINT64_MAX) ? 0 : (c_end - u_end);
            prev = &cur->next;
            continue;
        }
        /* Case 4: unl punches a hole in the middle - split into two */
        uint64_t right_start = u_end;
        uint64_t right_len = (c_end == UINT64_MAX) ? 0 : (c_end - u_end);
        cur->len = start - cur->start;
        lock_add(t, path, owner_pid, cur->type, right_start, right_len);
        prev = &cur->next;
    }
}

pgw_err rmfs_lock_apply(rmfs_lock_table *t, const char *path, uint32_t owner_pid,
                        pgw_lock_type type, uint64_t start, uint64_t len)
{
    if (t == NULL || path == NULL)
        return PGW_EINVAL;
    if (type == PGW_LOCK_UN) {
        lock_unlock_range(t, path, owner_pid, start, len);
        return PGW_OK;
    }
    /* Check for conflict with other owners */
    for (rmfs_range_lock *cur = t->head; cur != NULL; cur = cur->next) {
        if (strcmp(cur->path, path) == 0 &&
            locks_conflict(cur, owner_pid, type, start, len)) {
            return PGW_EACCES; /* or EAGAIN */
        }
    }
    /* Carve out any existing lock held by this owner in that sub-range */
    lock_unlock_range(t, path, owner_pid, start, len);
    lock_add(t, path, owner_pid, type, start, len);
    return PGW_OK;
}

void rmfs_lock_release_pid(rmfs_lock_table *t, uint32_t owner_pid)
{
    if (t == NULL)
        return;
    rmfs_range_lock **prev = &t->head;
    while (*prev != NULL) {
        rmfs_range_lock *cur = *prev;
        if (cur->owner_pid == owner_pid) {
            *prev = cur->next;
            free(cur);
        } else {
            prev = &cur->next;
        }
    }
}
