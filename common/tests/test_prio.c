/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_prio.c - the priority conversion is the one place where the QNX
 * numbering (1 = most urgent) meets the scheduler convention (bigger = more
 * urgent). A mistake here silently inverts PERF-05 on target, so it is pinned
 * down value by value.
 */
#include "pgw/prio.h"
#include "pgw/test.h"

static void test_most_urgent_qnx_priority_wins(void)
{
    /* QNX 1 is the most urgent; it must map to the largest urgency. */
    PGW_ASSERT(pgw_prio_from_qnx(1) > pgw_prio_from_qnx(10));
    PGW_ASSERT(pgw_prio_from_qnx(10) > pgw_prio_from_qnx(100));
    PGW_ASSERT(pgw_prio_from_qnx(100) > pgw_prio_from_qnx(250));
    PGW_ASSERT_EQ_U(pgw_prio_from_qnx(1), 255u);
    PGW_ASSERT_EQ_U(pgw_prio_from_qnx(255), 1u);
}
PGW_TEST(test_most_urgent_qnx_priority_wins);

static void test_conversion_is_an_involution(void)
{
    for (uint32_t p = PGW_QNX_PRIO_MIN; p <= PGW_QNX_PRIO_MAX; p++)
        PGW_ASSERT_EQ_U(pgw_prio_to_qnx(pgw_prio_from_qnx(p)), p);
}
PGW_TEST(test_conversion_is_an_involution);

static void test_out_of_range_is_never_most_urgent(void)
{
    uint32_t most = pgw_prio_from_qnx(1);
    PGW_ASSERT(pgw_prio_from_qnx(0) < most);
    PGW_ASSERT(pgw_prio_from_qnx(256) < most);
    PGW_ASSERT(pgw_prio_from_qnx(0xFFFFFFFFu) < most);
    PGW_ASSERT_EQ_U(pgw_prio_from_qnx(0), 1u);
    PGW_ASSERT_EQ_U(pgw_prio_to_qnx(0), PGW_QNX_PRIO_MAX);
    PGW_ASSERT_EQ_U(pgw_prio_to_qnx(9999), PGW_QNX_PRIO_MAX);
}
PGW_TEST(test_out_of_range_is_never_most_urgent);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
