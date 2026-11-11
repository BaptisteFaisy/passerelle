/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_idpool.c - allocator for tags and fids (P9-07).
 *
 * A 9P session hands out two kinds of small identifiers: tags (one per
 * request in flight, 0..65534 with NOTAG excluded) and fids (one per open
 * object, with NOFID excluded). Both must be reusable a million times without
 * ever leaking, and a freed identifier must never be handed out twice while
 * the previous owner may still be alive on the server.
 *
 * The implementation is a free-list (O(1) alloc, O(1) free) plus a validity
 * bitmap so that a double free or an out-of-range free is detected instead of
 * corrupting the pool. The free-list is filled in reverse at creation so the
 * first allocations return low identifiers, which keeps wire traces readable.
 */
#include "p9_priv.h"

#include <stdlib.h>

struct pgw_p9_idpool {
    uint32_t first;    /* first identifier of the range */
    uint32_t count;    /* size of the range */
    uint32_t in_use;   /* identifiers currently allocated */
    uint32_t free_top; /* number of entries on the free stack */
    uint32_t *free_ids;
    uint64_t *used; /* validity bitmap, one bit per identifier */
    uint32_t words;
};

static bool is_used(const pgw_p9_idpool *p, uint32_t idx)
{
    return ((p->used[idx / 64u] >> (idx % 64u)) & 1u) != 0;
}

static void set_used(pgw_p9_idpool *p, uint32_t idx, bool on)
{
    uint64_t bit = 1ULL << (idx % 64u);
    if (on)
        p->used[idx / 64u] |= bit;
    else
        p->used[idx / 64u] &= ~bit;
}

pgw_p9_idpool *pgw_p9_pool_create(uint32_t first, uint32_t count)
{
    if (count == 0 || count > 0x7FFFFFFFu)
        return NULL;
    pgw_p9_idpool *p = calloc(1, sizeof *p);
    if (p == NULL)
        return NULL;
    p->words = (count + 63u) / 64u;
    p->free_ids = calloc(count, sizeof *p->free_ids);
    p->used = calloc(p->words, sizeof *p->used);
    if (p->free_ids == NULL || p->used == NULL) {
        free(p->free_ids);
        free(p->used);
        free(p);
        return NULL;
    }
    p->first = first;
    p->count = count;
    /* Reverse fill: the last push is identifier `first`, so the first
     * allocation returns the lowest identifier of the range. */
    for (uint32_t i = count; i > 0; i--)
        p->free_ids[p->free_top++] = i - 1u;
    return p;
}

void pgw_p9_pool_destroy(pgw_p9_idpool *p)
{
    if (p == NULL)
        return;
    free(p->free_ids);
    free(p->used);
    free(p);
}

bool pgw_p9_pool_alloc(pgw_p9_idpool *p, uint32_t *out_id)
{
    if (p == NULL || out_id == NULL || p->free_top == 0)
        return false;
    uint32_t idx = p->free_ids[--p->free_top];
    if (idx >= p->count || is_used(p, idx))
        return false; /* corrupted pool: refuse rather than hand out twice */
    set_used(p, idx, true);
    p->in_use++;
    *out_id = p->first + idx;
    return true;
}

void pgw_p9_pool_free(pgw_p9_idpool *p, uint32_t id)
{
    if (p == NULL || id < p->first)
        return;
    uint32_t idx = id - p->first;
    if (idx >= p->count || !is_used(p, idx))
        return; /* unknown or already free: never corrupt the pool */
    set_used(p, idx, false);
    p->free_top++;
    p->free_ids[p->free_top - 1u] = idx;
    p->in_use--;
}

uint32_t pgw_p9_pool_in_use(const pgw_p9_idpool *p)
{
    return p != NULL ? p->in_use : 0;
}
