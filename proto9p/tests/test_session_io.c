/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_session_io.c - transfers, concurrency and failure paths.
 *
 * The second half of the session bench (the first is test_session.c), focused
 * on the properties that a single well-formed call does not exercise:
 *
 *   - P9-05  several requests in flight, including a nested call issued from
 *            inside the RPC callback (impossible for a turn-taking client);
 *   - P9-06  Tflush emitted when a request times out;
 *   - P9-07  no fid left open after thousands of operations;
 *   - P9-08  a 300 KiB read, a 100 KB write and a multi-message readdir, each
 *            split at msize - 24 and resumed at the next offset;
 *   - P9-09  a malformed reply (wrong type, a byte no field accounts for) is
 *            refused, and the session keeps working afterwards;
 *   - ROB-05 the timeout handed to the transport is the configured one.
 */
#include "test_bench.h"

#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

static void test_chunked_read_300k(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    uint32_t fid = bench_walk("big.bin");
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, fid, PGW_O_RDONLY, 0), PGW_OK);

    static uint8_t data[FAKE_BIG_SIZE];
    size_t got = 0;
    fake_reset_counters(fs);
    PGW_ASSERT_EQ_I(pgw_p9_read(s, fid, 0, data, sizeof data, &got, 0), PGW_OK);
    PGW_ASSERT_EQ_U(got, FAKE_BIG_SIZE);
    PGW_ASSERT(pgw_test_pattern_ok(data, got, 4242u, 0));

    /* Exactly ceil(size / (msize - 24)) Tread, one round trip per chunk. */
    uint32_t chunk = BENCH_MSIZE - 24u;
    size_t expect = (FAKE_BIG_SIZE + chunk - 1u) / chunk;
    PGW_ASSERT_EQ_U(fake_calls_of(fs, PGW_P9_TREAD), expect);
    PGW_ASSERT_EQ_U(fake_calls(fs), expect); /* nothing else was sent */
    PGW_ASSERT(pgw_stat_get(PGW_STAT_SPLIT) > 0);

    /* Reading past the end returns a short read, not an error. */
    got = 12345;
    PGW_ASSERT_EQ_I(pgw_p9_read(s, fid, FAKE_BIG_SIZE - 10u, data, sizeof data,
                                &got, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(got, 10u);
    PGW_ASSERT(pgw_test_pattern_ok(data, got, 4242u, FAKE_BIG_SIZE - 10u));
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_chunked_read_300k);

static void test_chunked_write(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    uint32_t dfid = bench_walk("");
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(s, dfid, "big.out",
                                   PGW_O_WRONLY | PGW_O_CREAT | PGW_O_TRUNC,
                                   0100644u, 0, &qid, 0),
                    PGW_OK);
    static uint8_t data[100000];
    pgw_test_pattern(data, sizeof data, 77u, 0);
    size_t written = 0;
    fake_reset_counters(fs);
    PGW_ASSERT_EQ_I(pgw_p9_write(s, dfid, 0, data, sizeof data, &written, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, sizeof data);
    uint32_t chunk = BENCH_MSIZE - 24u;
    size_t expect = (sizeof data + chunk - 1u) / chunk;
    PGW_ASSERT_EQ_U(fake_calls_of(fs, PGW_P9_TWRITE), expect);

    /* The bytes reached the server, in order, at the right offsets. */
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, dfid, 0), PGW_OK);
    uint32_t rfid = bench_walk("big.out");
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, rfid, PGW_O_RDONLY, 0), PGW_OK);
    static uint8_t back[100000];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(s, rfid, 0, back, sizeof back, &got, 0), PGW_OK);
    PGW_ASSERT_EQ_U(got, sizeof data);
    PGW_ASSERT_MEM_EQ(back, data, sizeof data);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, rfid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_chunked_write);

/* Names are e000.., e001.. in creation order, and never repeated: each one is
 * strictly greater than the previous.
 *
 * `prev` holds a whole protocol name (pgw_p9_dirent.name), not the fake
 * server's shorter one: a smaller buffer would silently truncate a long name
 * and make the comparison below lie. */
static void check_entries(const pgw_p9_dirent *ents, size_t got, char *prev)
{
    for (size_t i = 0; i < got; i++) {
        PGW_ASSERT_EQ_I(ents[i].name[0], 'e');
        PGW_ASSERT_EQ_U(ents[i].type, PGW_DT_REG);
        if (prev[0] != '\0')
            PGW_ASSERT(strcmp(prev, ents[i].name) < 0);
        snprintf(prev, sizeof ents[0].name, "%s", ents[i].name);
    }
}

static void test_readdir_multi_chunk_resume(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    uint32_t fid = bench_walk("many");
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, fid, PGW_O_RDONLY, 0), PGW_OK);

    /* Read the directory in small batches: each call must resume from the
     * cookie of the last entry it returned (P9-08, FS-03). */
    static pgw_p9_dirent ents[32];
    uint64_t off = 0;
    size_t total = 0;
    unsigned rounds = 0;
    char prev[sizeof ents[0].name];
    prev[0] = '\0';
    for (;;) {
        size_t got = 0;
        uint64_t next = 0;
        PGW_ASSERT_EQ_I(pgw_p9_readdir(s, fid, off, ents, 32, &got, &next, 0),
                        PGW_OK);
        rounds++;
        if (got == 0)
            break;
        check_entries(ents, got, prev);
        total += got;
        off = next;
        if (next == 0)
            break; /* a zero cookie means the directory is exhausted */
    }
    PGW_ASSERT_EQ_U(total, FAKE_MANY_COUNT);
    PGW_ASSERT(rounds > 1); /* more than one message was needed */

    /* The same directory in one big call returns everything, in order. */
    size_t got = 0;
    uint64_t next = 0;
    static pgw_p9_dirent all[FAKE_MANY_COUNT];
    PGW_ASSERT_EQ_I(pgw_p9_readdir(s, fid, 0, all, FAKE_MANY_COUNT, &got, &next,
                                   0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(got, FAKE_MANY_COUNT);
    for (size_t i = 1; i < got; i++)
        PGW_ASSERT(strcmp(all[i - 1].name, all[i].name) < 0);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_readdir_multi_chunk_resume);

static void test_many_twaks_for_a_deep_path(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    fake_srv *fs = bench_server();
    /* Build a 20-level chain: more than PGW_MAXWELEM names, so the client has
     * to issue several Twalk messages. */
    uint32_t cur = 0;
    for (unsigned i = 0; i < 20; i++) {
        char name[16];
        snprintf(name, sizeof name, "d%u", i);
        uint32_t n = fake_node_add(fs, cur, name, true, 040755u);
        PGW_ASSERT(n != UINT32_MAX);
        cur = n;
    }
    char path[256];
    size_t len = 0;
    for (unsigned i = 0; i < 19; i++)
        len += (size_t)snprintf(path + len, sizeof path - len, "d%u/", i);
    (void)snprintf(path + len, sizeof path - len, "%s", "d19");

    fake_reset_counters(fs);
    uint32_t fid = bench_walk(path);
    PGW_ASSERT(fake_calls_of(fs, PGW_P9_TWALK) >= 2u);
    /* Every intermediate fid was clunked: only root and the result are open. */
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(bench_session()), 2u);
    PGW_ASSERT_EQ_U(fake_open_fids(fs), 2u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(bench_session(), fid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_many_twaks_for_a_deep_path);

static void test_timeout_sends_flush(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    uint32_t fid = bench_walk("hello.txt");
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, fid, PGW_O_RDONLY, 0), PGW_OK);
    pgw_stat_reset();
    fs->drop_type = PGW_P9_TREAD; /* the server never answers this one */
    char buf[16];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(s, fid, 0, buf, sizeof buf, &got, 0),
                    PGW_ETIMEDOUT);
    PGW_ASSERT_EQ_U(fake_calls_of(fs, PGW_P9_TFLUSH), 1u); /* P9-06 */
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_TIMEOUT), 1u);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_FLUSH), 1u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_timeout_sends_flush);

static void test_bad_replies_refused(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    uint32_t fid = bench_walk("hello.txt");
    pgw_p9_attr a;
    /* A reply of the wrong type is an error, not a silent success. */
    fs->wrong_type = PGW_P9_TGETATTR;
    fs->wrong_reply = PGW_P9_RCLUNK;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(s, fid, &a, 0), PGW_EINVAL);
    /* A reply carrying a byte no field accounts for is refused (P9-09). */
    fs->trailing = true;
    PGW_ASSERT(pgw_p9_getattr(s, fid, &a, 0) != PGW_OK);
    /* And the session still works afterwards. */
    PGW_ASSERT_EQ_I(pgw_p9_getattr(s, fid, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 6u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_bad_replies_refused);

/* P9-05: a request may be issued while another is in flight. The hook fires
 * inside the RPC callback - that is, while the outer request is on the wire -
 * and issues a full nested getattr. A client that serialised requests with a
 * single tag would fail here (or deadlock). */
static unsigned g_nested;

static void nested_hook(fake_srv *fs, uint8_t type, void *user)
{
    (void)fs;
    (void)user;
    if (type != PGW_P9_TGETATTR || g_nested > 0)
        return;
    g_nested++;
    pgw_p9_attr a;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(bench_session(),
                                   pgw_p9_session_root_fid(bench_session()), &a,
                                   0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(a.qid.type & PGW_P9_QTDIR, PGW_P9_QTDIR);
}

static void test_nested_request(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    g_nested = 0;
    fs->hook = nested_hook;
    uint32_t fid = bench_walk("hello.txt");
    pgw_p9_attr a;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(s, fid, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(g_nested, 1u);
    /* No request ever used NOTAG, which is reserved for Tversion. */
    PGW_ASSERT(fake_max_tag(fs) < PGW_TAG_NONE);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    fs->hook = NULL;
    bench_down();
}
PGW_TEST(test_nested_request);

static void test_no_fid_leak_over_many_operations(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    for (unsigned i = 0; i < 5000; i++) {
        uint32_t fid = bench_walk("sub/deeper/leaf.txt");
        PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    }
    /* A failed lookup leaks nothing either. */
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t fid = 0;
        PGW_ASSERT_EQ_I(pgw_p9_walk_path(s, pgw_p9_session_root_fid(s), "nope",
                                         &fid, NULL, 0),
                        PGW_ENOENT);
    }
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(s), 1u);
    PGW_ASSERT_EQ_U(fake_open_fids(bench_server()), 1u);
    bench_down();
}
PGW_TEST(test_no_fid_leak_over_many_operations);

static void test_timeout_is_configurable(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    pgw_p9_session_set_timeout(s, 250);
    uint32_t fid = bench_walk("hello.txt");
    PGW_ASSERT_EQ_U(fs->last_timeout_ms, 250u);
    /* Beyond PGW_TIMEOUT_MS_MAX the value is clamped, never wrapped. */
    pgw_p9_session_set_timeout(s, 0xFFFFFFFFu);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    PGW_ASSERT_EQ_U(fs->last_timeout_ms, PGW_TIMEOUT_MS_MAX);
    bench_down();
}
PGW_TEST(test_timeout_is_configurable);

static void test_open_rejects_bad_arguments(void)
{
    PGW_ASSERT(pgw_p9_session_open(NULL, NULL, 4096, "", "", 0) == NULL);
    pgw_p9_session_close(NULL);
    PGW_ASSERT_EQ_U(pgw_p9_session_msize(NULL), 0u);
    PGW_ASSERT_EQ_U(pgw_p9_session_root_fid(NULL), PGW_FID_NONE);
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(NULL), 0u);
    pgw_p9_session_set_timeout(NULL, 10);

    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    uint32_t fid = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(NULL, 0, "x", &fid, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(s, 0, NULL, &fid, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_getattr(s, 0, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_setattr(s, 0, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_statfs(s, 0, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_read(s, 0, 0, NULL, 4, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_write(s, 0, 0, NULL, 4, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_mkdir(s, 0, NULL, 0755, 0, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_renameat(s, 0, NULL, 0, "b", 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_symlink(s, 0, NULL, "t", 0, NULL, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_readlink(s, 0, NULL, 0, 0), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_p9_link(s, 0, 0, NULL, 0), PGW_EINVAL);
    bench_down();
}
PGW_TEST(test_open_rejects_bad_arguments);

/* PERF-01: every round trip is visible in the shared counter table, so a
 * driver can export where the requests go without instrumenting the client. */
static void test_round_trips_are_counted(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    /* Opening the session costs exactly two round trips. */
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_REQ), 2u);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_RESP), 2u);

    uint32_t fid = bench_walk("hello.txt");
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_REQ), 3u);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_RESP), 3u);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, fid, PGW_O_RDONLY, 0), PGW_OK);
    char buf[16];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(s, fid, 0, buf, sizeof buf, &got, 0), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_BYTES_READ), 6u);

    /* An Rlerror bumps the error counter and is reported as a pgw_err. */
    fs->fail_type = PGW_P9_TGETATTR;
    fs->fail_errno = 13; /* EACCES */
    pgw_p9_attr a;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(s, fid, &a, 0), PGW_EACCES);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_ERR), 1u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_round_trips_are_counted);

/* P9-06, explicit form: a caller that abandons a request (an interrupted
 * readdir, say) can cancel it by tag and gets the server's Rflush. */
static void test_explicit_flush(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    fake_srv *fs = bench_server();
    fake_reset_counters(fs);
    PGW_ASSERT_EQ_I(pgw_p9_flush(s, 42, 0), PGW_OK);
    PGW_ASSERT_EQ_U(fake_calls_of(fs, PGW_P9_TFLUSH), 1u);
    PGW_ASSERT_EQ_U(fake_calls_of(fs, PGW_P9_RFLUSH), 0u);
    PGW_ASSERT_EQ_U(pgw_stat_get(PGW_STAT_FLUSH), 1u);
    bench_down();
}
PGW_TEST(test_explicit_flush);
