/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/common/src/reqtab_qnx.c - the replies that were popped by another
 * waiter.
 *
 * One virtqueue serves every client of the mount, so the reply of a request
 * can be popped by the thread that waits for another one. Dropping it (the
 * obvious thing to do) loses that reply: its owner waits until the deadline and
 * the other client never gets its answer. That is what two clients did to each
 * other on the QNX guest: four requests timed out and the rate fell by a
 * factor of eighteen.
 *
 * The table keeps the completions that arrive early, keyed by their cookie,
 * until their owner comes for them.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "pgw/drvsrv.h"

#include <pthread.h>
#include <string.h>

#define PGW_DONE_SLOTS 64u

typedef struct done_entry {
    void *cookie;
    uint32_t len;
    bool used;
} done_entry;

static done_entry g_done[PGW_DONE_SLOTS];
static pthread_mutex_t g_done_lock = PTHREAD_MUTEX_INITIALIZER;

/* Remember a reply that its owner has not come for yet. */
void pgw_reqtab_keep(void *cookie, uint32_t len)
{
    if (cookie == NULL)
        return;
    pthread_mutex_lock(&g_done_lock);
    for (unsigned i = 0; i < PGW_DONE_SLOTS; i++) {
        if (g_done[i].used)
            continue;
        g_done[i].cookie = cookie;
        g_done[i].len = len;
        g_done[i].used = true;
        break;
    }
    pthread_mutex_unlock(&g_done_lock);
}

/* Take the reply of `cookie`, if it is waiting for us. */
bool pgw_reqtab_take(void *cookie, uint32_t *out_len)
{
    bool found = false;
    if (cookie == NULL)
        return false;
    pthread_mutex_lock(&g_done_lock);
    for (unsigned i = 0; i < PGW_DONE_SLOTS; i++) {
        if (!g_done[i].used || g_done[i].cookie != cookie)
            continue;
        if (out_len != NULL)
            *out_len = g_done[i].len;
        g_done[i].used = false;
        g_done[i].cookie = NULL;
        found = true;
        break;
    }
    pthread_mutex_unlock(&g_done_lock);
    return found;
}

/* Forget everything: the mount is going away (ROB-03). */
void pgw_reqtab_clear(void)
{
    pthread_mutex_lock(&g_done_lock);
    memset(g_done, 0, sizeof g_done);
    pthread_mutex_unlock(&g_done_lock);
}
