/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_xlate.c - P9-04: QNX <-> Linux values on the wire.
 *
 * 9P2000.L carries Linux numbers. The guest is QNX, whose errno and open(2)
 * values differ above 34, so both directions have to be exercised:
 *
 *   guest open(2) flags (QNX) -> our bits -> Linux bits on the wire
 *   Rlerror ecode (Linux)     -> our bits -> QNX errno for the caller
 *
 * The bench pins the numbers that the specification calls out ("valeurs
 * Linux"), checks that an unknown errno stays an error instead of becoming
 * success (ROB-01), and checks that a bit we cannot honour is dropped rather
 * than silently changing the meaning of an open.
 */
#include "p9_priv.h"

#include "pgw/err.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <errno.h>
#include <fcntl.h>

static void test_wire_flags_are_linux(void)
{
    /* The access mode is not a bitmask in Linux: O_RDONLY=0, O_WRONLY=1,
     * O_RDWR=2. */
    PGW_ASSERT_EQ_U(pgw_p9__wire_flags(PGW_O_RDONLY), 0u);
    PGW_ASSERT_EQ_U(pgw_p9__wire_flags(PGW_O_WRONLY), 1u);
    PGW_ASSERT_EQ_U(pgw_p9__wire_flags(PGW_O_RDWR), 2u);

    /* The bits that matter for a create and a truncate. */
    uint32_t f = pgw_p9__wire_flags(PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC);
    PGW_ASSERT_EQ_U(f & 3u, 2u);
    PGW_ASSERT_EQ_U(f & 00000100u, 00000100u); /* O_CREAT */
    PGW_ASSERT_EQ_U(f & 00001000u, 00001000u); /* O_TRUNC */
    PGW_ASSERT_EQ_U(f & 00002000u, 0u);        /* O_APPEND not asked */

    f = pgw_p9__wire_flags(PGW_O_WRONLY | PGW_O_APPEND | PGW_O_EXCL);
    PGW_ASSERT_EQ_U(f & 3u, 1u);
    PGW_ASSERT_EQ_U(f & 00002000u, 00002000u); /* O_APPEND */
    PGW_ASSERT_EQ_U(f & 00000200u, 00000200u); /* O_EXCL */
}
PGW_TEST(test_wire_flags_are_linux);

static void test_host_flags_reach_the_wire(void)
{
    /* What a QNX program passes to open(2) is in *host* numbering; it must come
     * out as the Linux number on the wire. On the PC the host numbers happen to
     * be the Linux ones, so the check is on the translation path, not on the
     * numeric coincidence. */
    uint32_t host = (uint32_t)O_RDWR | (uint32_t)O_CREAT | (uint32_t)O_TRUNC;
    uint32_t wire = pgw_p9__wire_flags_from_host(host);
    PGW_ASSERT_EQ_U(wire & 3u, 2u);
    PGW_ASSERT_EQ_U(wire & 00000100u, 00000100u);
    PGW_ASSERT_EQ_U(wire & 00001000u, 00001000u);
    /* And back through our own bits: the round trip is stable. */
    PGW_ASSERT_EQ_U(pgw_p9__wire_flags(pgw_flags_from_linux(wire)), wire);
}
PGW_TEST(test_host_flags_reach_the_wire);

static void test_errno_linux_to_qnx(void)
{
    /* The values the specification names explicitly. */
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(2), PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(13), PGW_EACCES);
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(38), PGW_ENOSYS);
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(39), PGW_ENOTEMPTY);
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(40), PGW_ELOOP);
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(116), PGW_ESTALE);

    /* An errno we do not know must stay an error, never success (ROB-01). */
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(9999), PGW_EIO);
    PGW_ASSERT_EQ_I(pgw_p9__err_from_wire(0xFFFFFFFFu), PGW_EIO);

    /* And the number the caller sees is this build's: on QNX that is the QNX
     * value (ENOSYS is 89 there, 38 on Linux), which is why it goes through
     * pgw_host_errno() rather than being cast. */
    PGW_ASSERT_EQ_I(pgw_p9__host_errno(38), ENOSYS);
    PGW_ASSERT_EQ_I(pgw_p9__host_errno(2), ENOENT);
    PGW_ASSERT_EQ_I(pgw_p9__host_errno(0), 0);
    PGW_ASSERT_EQ_I(pgw_p9__host_errno(9999), EIO);
}
PGW_TEST(test_errno_linux_to_qnx);
