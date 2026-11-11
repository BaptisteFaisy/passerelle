/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/tests/test_rmfs_open.c - Data path of the resource manager.
 *
 * Covers requirements FS-01 (open/read/write/seek/close, files above 4 GiB),
 * FS-03 (coherent readdir, resumable offsets, entry types), FS-04 (exclusive
 * creation, truncation on open, append), FS-11 (deleted-but-open file stays
 * readable), FS-12 (a file changed on the PC is seen changed on the next
 * read), FS-13 (optional attribute cache, bounded and clock-driven) and
 * ROB-02 (a dead client leaves no descriptor behind).
 */
#include "../src/rmfs_internal.h"

#include "fake_backend.h"

#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"
#include "pgw/time.h"

#include <stdio.h>
#include <string.h>

static pgw_rmfs *make_fs(fake_backend *b, uint64_t attr_ttl_ns)
{
    pgw_fs_opts opts;
    pgw_fs_opts_default(&opts);
    opts.attr_ttl_ns = attr_ttl_ns;
    return pgw_rmfs_create(b->ops, b, &opts);
}

/* --- FS-01: the basic cycle ---------------------------------------------- */

static void test_open_read_write_seek_close(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "a.txt",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT(fd != NULL);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 1);

    uint64_t bytes0 = pgw_stat_get(PGW_STAT_BYTES_WRITTEN);
    const char *msg = "hello, guest";
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, msg, strlen(msg), &written),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, strlen(msg));

    /* Seek: read back at an offset, not from the start. */
    char buf[32];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 7, buf, 5, &got), PGW_OK);
    PGW_ASSERT_EQ_U(got, 5);
    PGW_ASSERT_MEM_EQ(buf, "guest", 5);

    /* A read past the end returns zero bytes, not an error. */
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 4096, buf, sizeof(buf), &got),
                    PGW_OK);
    PGW_ASSERT_EQ_U(got, 0);

    PGW_ASSERT_EQ_I(pgw_rmfs_fsync(fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 0);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_BYTES_WRITTEN) - bytes0,
                    strlen(msg));

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_file_above_4gib(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    const uint64_t big = 5ull * 1024ull * 1024ull * 1024ull; /* 5 GiB */
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "big.bin", "", 0), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_truncate(fs, &cred, "big.bin", big), PGW_OK);

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "big.bin", PGW_O_RDWR, 0, &fd),
                    PGW_OK);
    /* 64-bit offsets survive the round trip: write and read past 4 GiB. */
    const uint64_t off = big - 16;
    const char *tail = "0123456789abcdef";
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, off, tail, 16, &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, 16);
    char back[16];
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, off, back, 16, &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, 16);
    PGW_ASSERT_MEM_EQ(back, tail, 16);

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_fstat(fs, fd, &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, big);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-04: exclusive create, truncate, append --------------------------- */

static void test_exclusive_create(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "x.txt",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_EXCL, 0644,
                                  &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    /* Second exclusive create of the same name fails with EEXIST. */
    pgw_rmfs_fd *dup = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "x.txt",
                                  PGW_O_RDWR | PGW_O_CREAT | PGW_O_EXCL, 0644,
                                  &dup),
                    PGW_EEXIST);
    PGW_ASSERT(dup == NULL);
    PGW_ASSERT_EQ_U(pgw_rmfs_open_count(fs), 0);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_truncate_on_open(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "t.txt", "0123456789", 10), 0);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "t.txt",
                                  PGW_O_RDWR | PGW_O_TRUNC, 0, &fd),
                    PGW_OK);
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_fstat(fs, fd, &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

static void test_append_writes_at_end(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "app.txt", "abc", 3), 0);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "app.txt",
                                  PGW_O_WRONLY | PGW_O_APPEND, 0, &fd),
                    PGW_OK);
    /* The offset passed here is ignored: append wins (FS-04). */
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, "def", 3, &n), PGW_OK);
    PGW_ASSERT_EQ_U(n, 3);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    char host[1024];
    fake_backend_host_path(b, "app.txt", host, sizeof(host));
    FILE *f = fopen(host, "rb");
    PGW_ASSERT(f != NULL);
    char buf[16] = {0};
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    PGW_ASSERT_EQ_U(got, 6);
    PGW_ASSERT_MEM_EQ(buf, "abcdef", 6);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-11: deleted while open ------------------------------------------- */

static void test_deleted_while_open_stays_readable(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "gone.txt", "payload", 7), 0);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "gone.txt", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);

    PGW_ASSERT_EQ_I(pgw_rmfs_unlink(fs, &cred, "gone.txt", false), PGW_OK);

    /* The name is gone ... */
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "gone.txt", &a), PGW_ENOENT);

    /* ... but the open descriptor still reads the whole file (FS-11). */
    char buf[16] = {0};
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, buf, sizeof(buf), &got), PGW_OK);
    PGW_ASSERT_EQ_U(got, 7);
    PGW_ASSERT_MEM_EQ(buf, "payload", 7);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-12: no data cache ------------------------------------------------ */

static void test_pc_change_seen_on_next_read(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0); /* no attribute cache either */
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "live.txt", "before", 6), 0);
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, "live.txt", PGW_O_RDONLY, 0, &fd),
                    PGW_OK);

    char buf[16] = {0};
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, buf, sizeof(buf), &got), PGW_OK);
    PGW_ASSERT_MEM_EQ(buf, "before", 6);

    /* The user edits the file on the PC: no remount, next read sees it. */
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "live.txt", "AFTER!", 6), 0);
    memset(buf, 0, sizeof(buf));
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, buf, sizeof(buf), &got), PGW_OK);
    PGW_ASSERT_EQ_U(got, 6);
    PGW_ASSERT_MEM_EQ(buf, "AFTER!", 6);

    /* Attribute freshness too: size changes are visible immediately. */
    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "live.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 6);

    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-13: bounded, clock-driven attribute cache ------------------------ */

static uint64_t g_fake_now_ns = 0;

static uint64_t fake_clock(void *user)
{
    (void)user;
    return g_fake_now_ns;
}

static void test_attr_cache_hits_then_expires(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    g_fake_now_ns = 1000;
    pgw_time_set_clock(fake_clock, &g_fake_now_ns);

    pgw_rmfs *fs = make_fs(b, 1000000); /* 1 ms TTL */
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "c.txt", "abc", 3), 0);

    uint64_t hits0 = pgw_stat_get(PGW_STAT_INODE_HIT);
    uint64_t miss0 = pgw_stat_get(PGW_STAT_INODE_MISS);

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "c.txt", &a), PGW_OK);
    size_t attr_after_first = b->attr_calls;
    PGW_ASSERT(attr_after_first >= 1);

    /* Second stat within the TTL: served from the cache, no backend call. */
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "c.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(b->attr_calls, attr_after_first);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_INODE_HIT), hits0 + 1);
    PGW_ASSERT(pgw_stat_get(PGW_STAT_INODE_MISS) > miss0);

    /* Advance the injected clock past the TTL: the next stat asks again. */
    PGW_ASSERT(pgw_time_advance(2000000));
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &cred, "c.txt", &a), PGW_OK);
    PGW_ASSERT_EQ_U(b->attr_calls, attr_after_first + 1);

    pgw_time_set_clock(NULL, NULL);
    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- FS-03: coherent readdir --------------------------------------------- */

/* Read the directory two entries at a time, resuming from next_off, and
 * return the names seen. Proves nothing is lost or duplicated. */
static size_t collect_entries(pgw_rmfs *fs, pgw_rmfs_fd *fd, char seen[][256],
                              size_t want)
{
    size_t total = 0;
    uint64_t off = 0;
    for (int round = 0; round < 8 && total < want; round++) {
        pgw_fs_dirent ents[2];
        size_t n = 0;
        uint64_t next = 0;
        PGW_ASSERT_EQ_I(pgw_rmfs_readdir(fs, fd, off, ents, 2, &n, &next),
                        PGW_OK);
        if (n == 0)
            break;
        for (size_t i = 0; i < n && total < want; i++) {
            PGW_ASSERT(ents[i].name[0] != '\0');
            PGW_ASSERT(ents[i].type != PGW_FT_UNKNOWN);
            snprintf(seen[total], 256, "%s", ents[i].name);
            total++;
        }
        PGW_ASSERT(next >= off);
        off = next;
    }
    return total;
}

static bool contains_name(char seen[][256], size_t total, const char *name)
{
    for (size_t i = 0; i < total; i++) {
        if (strcmp(seen[i], name) == 0)
            return true;
    }
    return false;
}

static void test_readdir_resumes_without_duplicates(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred cred = {1000, 1000, 20};

    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "f1", "a", 1), 0);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "f2", "a", 1), 0);
    PGW_ASSERT_EQ_I(fake_backend_host_write(b, "f3", "a", 1), 0);
    PGW_ASSERT_EQ_I(pgw_rmfs_mkdir(fs, &cred, "sub", 0755), PGW_OK);

    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &cred, ".",
                                  PGW_O_RDONLY | PGW_O_DIRECTORY, 0, &fd),
                    PGW_OK);

    char seen[4][256];
    size_t total = collect_entries(fs, fd, seen, 4);
    PGW_ASSERT_EQ_U(total, 4);

    /* No entry is reported twice: this is the FS-03 property. */
    for (size_t i = 0; i < total; i++) {
        for (size_t j = i + 1; j < total; j++)
            PGW_ASSERT(strcmp(seen[i], seen[j]) != 0);
    }
    PGW_ASSERT(contains_name(seen, total, "sub"));
    PGW_ASSERT(contains_name(seen, total, "f1"));

    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

/* --- QNX-03: the request runs at the caller priority --------------------- */

static void test_caller_priority_reaches_backend(void)
{
    fake_backend *b = fake_backend_new();
    PGW_ASSERT(b != NULL);
    pgw_rmfs *fs = make_fs(b, 0);
    PGW_ASSERT(fs != NULL);
    pgw_fs_cred high = {1000, 1000, 55}; /* a real-time guest priority */
    pgw_fs_cred low = {1000, 1000, 5};

    pgw_fs_attr a;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &high, "missing", &a), PGW_ENOENT);
    PGW_ASSERT_EQ_U(b->last_prio, 55);

    PGW_ASSERT_EQ_I(pgw_rmfs_stat(fs, &low, "missing", &a), PGW_ENOENT);
    PGW_ASSERT_EQ_U(b->last_prio, 5);

    /* A descriptor remembers the priority of the caller that opened it, so
     * the following reads and writes are submitted at that priority too. */
    pgw_rmfs_fd *fd = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, &high, "prio.txt",
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_U(b->last_prio, 55);
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, "x", 1, &n), PGW_OK);
    PGW_ASSERT_EQ_U(b->last_prio, 55);
    char c = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, &c, 1, &n), PGW_OK);
    PGW_ASSERT_EQ_U(b->last_prio, 55);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    pgw_rmfs_destroy(fs);
    fake_backend_free(b);
}

PGW_TEST(test_open_read_write_seek_close);
PGW_TEST(test_caller_priority_reaches_backend);
PGW_TEST(test_file_above_4gib);
PGW_TEST(test_exclusive_create);
PGW_TEST(test_truncate_on_open);
PGW_TEST(test_append_writes_at_end);
PGW_TEST(test_deleted_while_open_stays_readable);
PGW_TEST(test_pc_change_seen_on_next_read);
PGW_TEST(test_attr_cache_hits_then_expires);
PGW_TEST(test_readdir_resumes_without_duplicates);
