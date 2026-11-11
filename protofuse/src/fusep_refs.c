/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_refs.c - Node reference-count table (VFS-04).
 *
 * Implements requirements:
 *   - VFS-04: every successful LOOKUP/CREATE adds one reference to the node it
 *     returns, FORGET gives back exactly `nlookup` references, and closing the
 *     session gives back whatever is left.
 *
 * The table is a sorted array of (nodeid, nlookup) pairs: binary search for
 * lookup, memmove for insert. Nodes whose count drops to zero are removed, so
 * "no node remains referenced" is exactly "the array is empty", which is what
 * the session-close path asserts.
 */
#include "fusep_session.h"

#include <stdlib.h>
#include <string.h>

static size_t lower_bound(const pgw_fuse_refs *t, uint64_t nodeid)
{
    size_t lo = 0;
    size_t hi = t->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (t->v[mid].nodeid < nodeid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

void pgw_fusep_refs_init(pgw_fuse_refs *t)
{
    if (t == NULL)
        return;
    t->v = NULL;
    t->n = 0;
    t->cap = 0;
}

void pgw_fusep_refs_free(pgw_fuse_refs *t)
{
    if (t == NULL)
        return;
    free(t->v);
    t->v = NULL;
    t->n = 0;
    t->cap = 0;
}

static pgw_err refs_grow(pgw_fuse_refs *t, size_t want)
{
    if (want <= t->cap)
        return PGW_OK;
    size_t cap = (t->cap == 0) ? 16u : t->cap;
    while (cap < want) {
        if (cap > (SIZE_MAX / 2u))
            return PGW_ENOMEM;
        cap *= 2u;
    }
    if (cap > SIZE_MAX / sizeof(pgw_fuse_ref))
        return PGW_ENOMEM;
    pgw_fuse_ref *nv = (pgw_fuse_ref *)realloc(t->v, cap * sizeof(*nv));
    if (nv == NULL)
        return PGW_ENOMEM; /* never proceed with a half-grown table */
    t->v = nv;
    t->cap = cap;
    return PGW_OK;
}

uint64_t pgw_fusep_refs_get(const pgw_fuse_refs *t, uint64_t nodeid)
{
    if (t == NULL || t->n == 0)
        return 0;
    size_t i = lower_bound(t, nodeid);
    if (i < t->n && t->v[i].nodeid == nodeid)
        return t->v[i].nlookup;
    return 0;
}

size_t pgw_fusep_refs_count(const pgw_fuse_refs *t)
{
    return (t != NULL) ? t->n : 0;
}

pgw_err pgw_fusep_refs_add(pgw_fuse_refs *t, uint64_t nodeid, uint64_t delta)
{
    if (t == NULL)
        return PGW_EINVAL;
    if (nodeid == 0)
        return PGW_EINVAL; /* nodeid 0 is "no node" on the wire */
    if (delta == 0)
        return PGW_OK;
    size_t i = lower_bound(t, nodeid);
    if (i < t->n && t->v[i].nodeid == nodeid) {
        if (t->v[i].nlookup > UINT64_MAX - delta)
            return PGW_EOVERFLOW;
        t->v[i].nlookup += delta;
        return PGW_OK;
    }
    pgw_err rc = refs_grow(t, t->n + 1u);
    if (rc != PGW_OK)
        return rc;
    if (i < t->n)
        memmove(&t->v[i + 1u], &t->v[i], (t->n - i) * sizeof(t->v[0]));
    t->v[i].nodeid = nodeid;
    t->v[i].nlookup = delta;
    t->n++;
    return PGW_OK;
}

pgw_err pgw_fusep_refs_take(pgw_fuse_refs *t, uint64_t nodeid, uint64_t *out)
{
    if (out != NULL)
        *out = 0;
    if (t == NULL)
        return PGW_EINVAL;
    size_t i = lower_bound(t, nodeid);
    if (i >= t->n || t->v[i].nodeid != nodeid)
        return PGW_OK; /* unknown node: forgetting it is a no-op, not an error */
    if (out != NULL)
        *out = t->v[i].nlookup;
    if (i + 1u < t->n)
        memmove(&t->v[i], &t->v[i + 1u], (t->n - i - 1u) * sizeof(t->v[0]));
    t->n--;
    return PGW_OK;
}

const pgw_fuse_ref *pgw_fusep_refs_at(const pgw_fuse_refs *t, size_t i)
{
    if (t == NULL || i >= t->n)
        return NULL;
    return &t->v[i];
}

void pgw_fusep_refs_clear(pgw_fuse_refs *t)
{
    if (t != NULL)
        t->n = 0;
}
