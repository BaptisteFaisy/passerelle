/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_idpool.c - the identifier reservoirs (P9-07).
 *
 * P9-07 asks for one thing that a unit test can pin exactly: a million
 * allocations and frees must leave no identifier behind. The bench checks that
 * on a tag pool (0..65534, NOTAG excluded) and on a fid pool (NOFID excluded),
 * plus the properties a pool must never break: no double allocation, no
 * out-of-range identifier, a clean refusal when the range is exhausted, and a
 * free of an identifier that was never allocated (or already freed) leaves the
 * pool intact instead of corrupting it.
 */
#include "p9_priv.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdint.h>

static void test_pool_tags_range_and_exhaustion(void)
{
    pgw_p9_idpool *p = pgw_p9_pool_create(0, PGW_P9_MAX_TAGS);
    PGW_ASSERT(p != NULL);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), 0u);

    /* Every tag of the range is handed out exactly once, and NOTAG never. */
    static uint8_t bitmap[(PGW_P9_MAX_TAGS + 7u) / 8u];
    for (size_t i = 0; i < sizeof bitmap; i++)
        bitmap[i] = 0;
    for (uint32_t i = 0; i < PGW_P9_MAX_TAGS; i++) {
        uint32_t id = 0;
        PGW_ASSERT(pgw_p9_pool_alloc(p, &id));
        PGW_ASSERT(id < PGW_P9_MAX_TAGS);
        PGW_ASSERT(id != PGW_TAG_NONE);
        PGW_ASSERT((bitmap[id / 8u] & (1u << (id % 8u))) == 0);
        bitmap[id / 8u] |= (uint8_t)(1u << (id % 8u));
    }
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), PGW_P9_MAX_TAGS);
    /* Exhausted: a further allocation is refused, not a wrap-around. */
    uint32_t extra = 0;
    PGW_ASSERT_FALSE(pgw_p9_pool_alloc(p, &extra));

    for (uint32_t id = 0; id < PGW_P9_MAX_TAGS; id++)
        pgw_p9_pool_free(p, id);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), 0u);
    pgw_p9_pool_destroy(p);
}
PGW_TEST(test_pool_tags_range_and_exhaustion);

static void test_pool_fids_range(void)
{
    pgw_p9_idpool *p = pgw_p9_pool_create(0, PGW_P9_MAX_FIDS);
    PGW_ASSERT(p != NULL);
    for (uint32_t i = 0; i < PGW_P9_MAX_FIDS; i++) {
        uint32_t id = 0;
        PGW_ASSERT(pgw_p9_pool_alloc(p, &id));
        PGW_ASSERT(id != PGW_FID_NONE);
        PGW_ASSERT(id < PGW_P9_MAX_FIDS);
    }
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), PGW_P9_MAX_FIDS);
    pgw_p9_pool_destroy(p);
}
PGW_TEST(test_pool_fids_range);

static void test_pool_no_double_alloc_no_double_free(void)
{
    pgw_p9_idpool *p = pgw_p9_pool_create(100, 4);
    PGW_ASSERT(p != NULL);
    uint32_t a = 0, b = 0, c = 0, d = 0;
    PGW_ASSERT(pgw_p9_pool_alloc(p, &a));
    PGW_ASSERT(pgw_p9_pool_alloc(p, &b));
    PGW_ASSERT(pgw_p9_pool_alloc(p, &c));
    PGW_ASSERT(pgw_p9_pool_alloc(p, &d));
    PGW_ASSERT(a >= 100u && a < 104u);
    PGW_ASSERT(a != b && b != c && c != d && a != c && a != d && b != d);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), 4u);

    /* A double free must not put the identifier twice on the free list: the
     * pool would then hand the same fid to two owners. */
    pgw_p9_pool_free(p, a);
    pgw_p9_pool_free(p, a);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), 3u);
    uint32_t again = 0;
    PGW_ASSERT(pgw_p9_pool_alloc(p, &again));
    PGW_ASSERT_EQ_U(again, a);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), 4u);
    PGW_ASSERT_FALSE(pgw_p9_pool_alloc(p, &again)); /* full again */

    /* Out-of-range identifiers are ignored, never corrupt the bitmap. */
    pgw_p9_pool_free(p, 99);
    pgw_p9_pool_free(p, 104);
    pgw_p9_pool_free(p, PGW_FID_NONE);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(p), 4u);
    pgw_p9_pool_destroy(p);
}
PGW_TEST(test_pool_no_double_alloc_no_double_free);

/* The P9-07 acceptance criterion: one million alloc/free, no leak. */
static void test_pool_million_cycles_no_leak(void)
{
    pgw_p9_idpool *fids = pgw_p9_pool_create(0, 1024);
    pgw_p9_idpool *tags = pgw_p9_pool_create(0, PGW_P9_MAX_TAGS);
    PGW_ASSERT(fids != NULL && tags != NULL);
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint32_t fid = 0;
        uint32_t tag = 0;
        PGW_ASSERT(pgw_p9_pool_alloc(fids, &fid));
        PGW_ASSERT(pgw_p9_pool_alloc(tags, &tag));
        PGW_ASSERT(fid < 1024u);
        PGW_ASSERT(tag < PGW_P9_MAX_TAGS);
        pgw_p9_pool_free(fids, fid);
        pgw_p9_pool_free(tags, tag);
    }
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(fids), 0u);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(tags), 0u);
    /* Every identifier is available again: the whole range allocates. */
    for (uint32_t i = 0; i < 1024u; i++) {
        uint32_t id = 0;
        PGW_ASSERT(pgw_p9_pool_alloc(fids, &id));
    }
    PGW_ASSERT_FALSE(pgw_p9_pool_alloc(fids, NULL));
    pgw_p9_pool_destroy(fids);
    pgw_p9_pool_destroy(tags);
}
PGW_TEST(test_pool_million_cycles_no_leak);

static void test_pool_bad_arguments(void)
{
    PGW_ASSERT(pgw_p9_pool_create(0, 0) == NULL);
    pgw_p9_pool_destroy(NULL);
    PGW_ASSERT_FALSE(pgw_p9_pool_alloc(NULL, NULL));
    pgw_p9_pool_free(NULL, 0);
    PGW_ASSERT_EQ_U(pgw_p9_pool_in_use(NULL), 0u);
    pgw_p9_idpool *p = pgw_p9_pool_create(0, 1);
    PGW_ASSERT(p != NULL);
    PGW_ASSERT_FALSE(pgw_p9_pool_alloc(p, NULL));
    pgw_p9_pool_destroy(p);
}
PGW_TEST(test_pool_bad_arguments);
