/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/tests/test_opts.c - the mount option parser, on the PC.
 *
 * The parser is portable, so its rules are checked here rather than on a
 * target: a wrong bound would either silently change the security of a mount
 * (SEC-01) or make a legitimate option fail (FS-13, P9-01, ROB-05).
 *
 * Covered requirements: FS-09, FS-13, P9-01, ROB-05, SEC-01, QNX-07.
 */
#include "pgw/drvopts.h"
#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/test.h"

#include <string.h>

static void test_defaults(void)
{
    pgw_mount_opts o;
    pgw_opts_default(&o);
    PGW_ASSERT(o.fs.nosuid);          /* SEC-01: nosuid by default */
    PGW_ASSERT(!o.fs.readonly);
    PGW_ASSERT(!o.fs.noexec);
    PGW_ASSERT(o.fs.attr_ttl_ns == 0); /* FS-13: no cache unless asked */
    PGW_ASSERT(o.msize == PGW_MSIZE_DEFAULT);
    PGW_ASSERT(o.timeout_ms == PGW_TIMEOUT_MS_DEFAULT);
    PGW_ASSERT(o.irq == -1); /* VIO-04: let layer 1 choose (MSI-X first) */
    PGW_ASSERT(o.tag[0] == '\0');
}

static void test_flags(void)
{
    pgw_mount_opts o;
    pgw_opts_default(&o);
    PGW_ASSERT(pgw_opts_parse("ro,noexec,nosuid", &o, NULL) == PGW_OK);
    PGW_ASSERT(o.fs.readonly);
    PGW_ASSERT(o.fs.noexec);
    PGW_ASSERT(o.fs.nosuid);

    /* `suid` is the explicit opt-out, `rw` the explicit opt-in. */
    PGW_ASSERT(pgw_opts_parse("rw,suid", &o, NULL) == PGW_OK);
    PGW_ASSERT(!o.fs.readonly);
    PGW_ASSERT(!o.fs.nosuid);
}

static void test_values(void)
{
    pgw_mount_opts o;
    pgw_opts_default(&o);
    PGW_ASSERT(pgw_opts_parse("tag=hostshare,msize=8192,timeout=1500", &o, NULL) ==
               PGW_OK);
    PGW_ASSERT(strcmp(o.tag, "hostshare") == 0);
    PGW_ASSERT(o.msize == 8192u);
    PGW_ASSERT(o.timeout_ms == 1500u);
}

static void test_irq_override(void)
{
    /* VIO-04 documents `-o irq=N` as the way to force the classic line. The
     * default must stay -1 ("let layer 1 pick"), or every mount would silently
     * ignore MSI-X. */
    pgw_mount_opts o;
    pgw_opts_default(&o);
    PGW_ASSERT(o.irq == -1);
    PGW_ASSERT(pgw_opts_parse("ro,nosuid,irq=10", &o, NULL) == PGW_OK);
    PGW_ASSERT(o.irq == 10);
    /* A bare `irq` has no value to take: it is a typo, not a default. */
    PGW_ASSERT(pgw_opts_parse("irq", &o, NULL) != PGW_OK);
    /* Zero is a legal line number on x86, and it must not be read as "unset". */
    PGW_ASSERT(pgw_opts_parse("irq=0", &o, NULL) == PGW_OK);
    PGW_ASSERT(o.irq == 0);
}

static void test_bounds(void)
{
    pgw_mount_opts o;
    pgw_opts_default(&o);
    /* msize below the minimum is refused: a 9P message that small cannot carry
     * a header and a path (P9-01). */
    PGW_ASSERT(pgw_opts_parse("msize=1024", &o, NULL) != PGW_OK);
    PGW_ASSERT(pgw_opts_parse("msize=99999999", &o, NULL) != PGW_OK);
    /* An unknown option is an error: a typo must not change the behaviour. */
    PGW_ASSERT(pgw_opts_parse("msiez=8192", &o, NULL) != PGW_OK);
    PGW_ASSERT(pgw_opts_parse("ro,", &o, NULL) != PGW_OK);
    PGW_ASSERT(pgw_opts_parse("=8192", &o, NULL) != PGW_OK);
    /* A bare flag with a value is refused. */
    PGW_ASSERT(pgw_opts_parse("ro=1", &o, NULL) != PGW_OK);
}

static void test_attrttl(void)
{
    pgw_mount_opts o;
    pgw_opts_default(&o);
    /* attrttl is given in milliseconds and stored in nanoseconds (FS-13). */
    PGW_ASSERT(pgw_opts_parse("attrttl=2000", &o, NULL) == PGW_OK);
    PGW_ASSERT(o.fs.attr_ttl_ns == 2000ull * 1000000ull);
    /* Beyond the hard ceiling the option is refused, not clamped. */
    PGW_ASSERT(pgw_opts_parse("attrttl=999999999", &o, NULL) != PGW_OK);
}

static void test_u32(void)
{
    uint32_t v = 0;
    PGW_ASSERT(pgw_opts_u32("4096", 4, 0, 100000, &v) == PGW_OK);
    PGW_ASSERT(v == 4096u);
    PGW_ASSERT(pgw_opts_u32("0x1000", 6, 0, 100000, &v) == PGW_OK);
    PGW_ASSERT(v == 0x1000u);
    PGW_ASSERT(pgw_opts_u32("4096", 4, 4097, 100000, &v) != PGW_OK);
    PGW_ASSERT(pgw_opts_u32("12x", 3, 0, 100000, &v) != PGW_OK);
    PGW_ASSERT(pgw_opts_u32("", 0, 0, 100000, &v) != PGW_OK);
    /* An overflowing number is refused, never wrapped into a plausible one. */
    PGW_ASSERT(pgw_opts_u32("99999999999999", 14, 0, 100000, &v) != PGW_OK);
}

/* The harness walks a section the cases register themselves in, so the
 * registrations live at file scope (see common/include/pgw/test.h). */
PGW_TEST(test_defaults);
PGW_TEST(test_flags);
PGW_TEST(test_values);
PGW_TEST(test_irq_override);
PGW_TEST(test_bounds);
PGW_TEST(test_attrttl);
PGW_TEST(test_u32);

/* The runner's main is provided by common (pgw_test_main). */
int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
