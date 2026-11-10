/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/tests/test_common.c - unit tests for buf, err, flags, log, time.
 */
#define _POSIX_C_SOURCE 200809L
#include "pgw/buf.h"
#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/test.h"
#include "pgw/time.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

static uint64_t g_fake_ns = 1000000ULL;
static uint64_t fake_clock(void *u)
{
    return *(uint64_t *)u;
}

static void test_buf_roundtrip(void)
{
    uint8_t mem[64];
    pgw_out w;
    pgw_out_init(&w, mem, sizeof(mem));
    size_t len_off = pgw_written(&w);
    pgw_put_u32(&w, 0);
    pgw_put_u8(&w, 100);
    pgw_put_u16(&w, 0x1234u);
    pgw_put_u64(&w, 0x0102030405060708ULL);
    PGW_ASSERT(pgw_put_str(&w, "9P2000.L", 8));
    PGW_ASSERT(pgw_patch_len_u32(&w, len_off));
    PGW_ASSERT(pgw_out_ok(&w));

    pgw_buf r;
    pgw_buf_init(&r, mem, pgw_written(&w));
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), pgw_written(&w));
    PGW_ASSERT_EQ_U(pgw_get_u8(&r), 100);
    PGW_ASSERT_EQ_U(pgw_get_u16(&r), 0x1234u);
    PGW_ASSERT_EQ_U(pgw_get_u64(&r), 0x0102030405060708ULL);
    char ver[16];
    size_t slen = 0;
    PGW_ASSERT_EQ_I(pgw_get_str(&r, ver, sizeof(ver), &slen), PGW_STR_OK);
    PGW_ASSERT_EQ_U(slen, 8);
    PGW_ASSERT_STR_EQ(ver, "9P2000.L");
    PGW_ASSERT_EQ_U(pgw_left(&r), 0);
    PGW_ASSERT(pgw_buf_ok(&r));
}
PGW_TEST(test_buf_roundtrip);

static void test_buf_sticky_fail_and_clamp(void)
{
    uint8_t data[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    pgw_buf r;
    pgw_buf_init(&r, data, sizeof(data));
    pgw_buf_clamp(&r, 4);
    PGW_ASSERT_EQ_U(pgw_get_u32(&r), 0x04030201u);
    PGW_ASSERT(pgw_buf_ok(&r));
    PGW_ASSERT_EQ_U(pgw_get_u8(&r), 0);
    PGW_ASSERT_FALSE(pgw_buf_ok(&r));

    uint8_t small[3];
    pgw_out w;
    pgw_out_init(&w, small, sizeof(small));
    pgw_put_u32(&w, 42);
    PGW_ASSERT_FALSE(pgw_out_ok(&w));
}
PGW_TEST(test_buf_sticky_fail_and_clamp);

static void test_err_linux_table(void)
{
    PGW_ASSERT_EQ_U(pgw_linux_errno(PGW_ENOENT), 2u);
    PGW_ASSERT_EQ_U(pgw_linux_errno(PGW_ENOSYS), 38u);
    PGW_ASSERT_EQ_U(pgw_linux_errno(PGW_ENOTEMPTY), 39u);
    PGW_ASSERT_EQ_U(pgw_linux_errno(PGW_ELOOP), 40u);
    PGW_ASSERT_EQ_U(pgw_linux_errno(PGW_ESTALE), 116u);
    PGW_ASSERT_EQ_I(pgw_code_from_linux(0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_code_from_linux(38), PGW_ENOSYS);
    PGW_ASSERT_EQ_I(pgw_code_from_linux(99999), PGW_EIO);
    PGW_ASSERT_EQ_I(pgw_host_errno(PGW_ENOENT), ENOENT);
    PGW_ASSERT_EQ_I(pgw_code_from_host(ENOENT), PGW_ENOENT);
    PGW_ASSERT_STR_EQ(pgw_err_name(PGW_EACCES), "EACCES");
}
PGW_TEST(test_err_linux_table);

static void test_flags_and_modes(void)
{
    uint32_t lf = pgw_linux_oflags(PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC);
    PGW_ASSERT_EQ_U(lf, 2u | 0100u | 01000u);
    uint32_t back = pgw_flags_from_linux(lf);
    PGW_ASSERT((back & PGW_O_RDWR) != 0);
    PGW_ASSERT((back & PGW_O_CREAT) != 0);
    PGW_ASSERT((back & PGW_O_TRUNC) != 0);

    uint32_t hf = pgw_host_oflags(PGW_O_WRONLY | PGW_O_CREAT);
    PGW_ASSERT((hf & O_WRONLY) == O_WRONLY);
    PGW_ASSERT((hf & O_CREAT) == O_CREAT);

    uint32_t lm = pgw_linux_mode(PGW_FT_LNK, 0777u);
    PGW_ASSERT_EQ_I(pgw_ftype_from_linux_mode(lm), PGW_FT_LNK);
    PGW_ASSERT_EQ_U(pgw_dt_from_ftype(PGW_FT_DIR), PGW_DT_DIR);
    PGW_ASSERT_EQ_I(pgw_ftype_from_dt(PGW_DT_REG), PGW_FT_REG);
}
PGW_TEST(test_flags_and_modes);

static void test_time_fake_clock(void)
{
    g_fake_ns = 5000000ULL;
    pgw_time_set_clock(fake_clock, &g_fake_ns);
    pgw_deadline d = pgw_deadline_from_ms(10);
    PGW_ASSERT_FALSE(pgw_deadline_expired(d));
    PGW_ASSERT_EQ_I(pgw_deadline_left_ms(d), 10);
    PGW_ASSERT(pgw_time_advance(pgw_ms_to_ns(10)));
    PGW_ASSERT(pgw_deadline_expired(d));
    PGW_ASSERT_EQ_U(pgw_deadline_left_ns(d), 0);
    pgw_time_set_clock(NULL, NULL);
}
PGW_TEST(test_time_fake_clock);

static void test_log_stats_and_pattern(void)
{
    pgw_stat_reset();
    pgw_stat_add(PGW_STAT_REQ, 3);
    pgw_stat_add(PGW_STAT_BYTES_READ, 4096);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_REQ), 3);
    char buf[512];
    size_t n = pgw_stat_format(buf, sizeof(buf));
    PGW_ASSERT(n > 0 && n < sizeof(buf));

    uint8_t p[128];
    pgw_test_pattern(p, sizeof(p), 77, 1024);
    PGW_ASSERT(pgw_test_pattern_ok(p, sizeof(p), 77, 1024));
    p[64] ^= 1u;
    PGW_ASSERT_FALSE(pgw_test_pattern_ok(p, sizeof(p), 77, 1024));
}
PGW_TEST(test_log_stats_and_pattern);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
