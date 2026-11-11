/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/test_session.c - the whole client against an in-memory server.
 *
 * This bench is the P9-10 proof: the protocol layer runs, unmodified, on the
 * PC, with no QNX, no QEMU and no disk, because it reaches the server only
 * through the pgw_p9_rpc_fn callback. Against tests/fake_server.c it checks:
 *
 *   - P9-01  version and msize negotiation (a server may lower msize only);
 *   - P9-02  the full namespace and data path of a session;
 *   - P9-04  open flags go out with their Linux values;
 *   - P9-07  no fid left open on the server after a long series of operations.
 *
 * The transfer-side properties (P9-05, P9-06, P9-08, P9-09) are in
 * test_session_io.c, on the same bench.
 */
#include "test_bench.h"

#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

static void test_negotiation(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    PGW_ASSERT_EQ_U(pgw_p9_session_msize(bench_session()), BENCH_MSIZE);
    PGW_ASSERT_EQ_U(pgw_p9_session_root_fid(bench_session()), 0u);
    /* Exactly two round trips to open: Tversion then Tattach. */
    PGW_ASSERT_EQ_U(fake_calls(bench_server()), 2u);
    PGW_ASSERT_EQ_U(fake_calls_of(bench_server(), PGW_P9_TVERSION), 1u);
    PGW_ASSERT_EQ_U(fake_calls_of(bench_server(), PGW_P9_TATTACH), 1u);
    bench_down();

    /* A server that lowers msize is accepted. */
    bench_up(131072u, 8192u);
    PGW_ASSERT_EQ_U(pgw_p9_session_msize(bench_session()), 8192u);
    bench_down();

    /* A server that answers with a dialect we do not speak is refused. */
    fake_srv *fs = fake_new(BENCH_MSIZE);
    PGW_ASSERT(fs != NULL);
    snprintf(fs->version, sizeof fs->version, "%s", "9P2000");
    PGW_ASSERT(pgw_p9_session_open(fake_rpc, fs, BENCH_MSIZE, "", "", 0) == NULL);
    fake_free(fs);
}
PGW_TEST(test_negotiation);

/* Walk `path`, open it read-only, read it and compare with `want`. */
static void read_expect(const char *path, const char *want, size_t want_len)
{
    pgw_p9_session *s = bench_session();
    uint32_t fid = bench_walk(path);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, fid, PGW_O_RDONLY, 0), PGW_OK);
    char buf[64];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(s, fid, 0, buf, sizeof buf, &got, 0), PGW_OK);
    PGW_ASSERT_EQ_U(got, want_len);
    PGW_ASSERT_MEM_EQ(buf, want, want_len);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
}

static void test_walk_and_read(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    uint32_t root = pgw_p9_session_root_fid(s);

    /* A multi-element path. */
    uint32_t fid = 0;
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(s, root, "sub/deeper/leaf.txt", &fid, &qid, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(qid.type & PGW_P9_QTDIR, 0);
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(s), 2u); /* root + one */
    pgw_p9_attr a;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(s, fid, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 4u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(s), 1u); /* root only */
    read_expect("sub/deeper/leaf.txt", "leaf", 4);

    /* "." and ".." are resolved by the client, never sent on the wire. */
    read_expect("./sub/../hello.txt", "hello\n", 6);

    /* An empty path clones the root fid. */
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(s, root, "", &fid, &qid, 0), PGW_OK);
    PGW_ASSERT(fid != root);
    PGW_ASSERT_EQ_U(qid.type & PGW_P9_QTDIR, PGW_P9_QTDIR);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);

    /* A missing component is ENOENT, a file used as a directory is ENOTDIR;
     * neither leaves a fid open. */
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(s, root, "nope/nothing", &fid, NULL, 0),
                    PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(s, root, "hello.txt/x", &fid, NULL, 0),
                    PGW_ENOTDIR);
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(s), 1u);
    PGW_ASSERT_EQ_U(fake_open_fids(bench_server()), 1u);
    bench_down();
}
PGW_TEST(test_walk_and_read);

static void test_create_write_and_rename(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    pgw_p9_qid qid;
    uint32_t dfid = bench_walk("");
    PGW_ASSERT_EQ_I(pgw_p9_mkdir(s, dfid, "newdir", 040755u, 0, &qid, 0), PGW_OK);
    PGW_ASSERT_EQ_U(qid.type & PGW_P9_QTDIR, PGW_P9_QTDIR);

    uint32_t nfid = bench_walk("newdir");
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(s, nfid, "file.txt",
                                   PGW_O_WRONLY | PGW_O_CREAT | PGW_O_TRUNC,
                                   0100644u, 0, &qid, 0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_p9_write(s, nfid, 0, "written", 7, &written, 0), PGW_OK);
    PGW_ASSERT_EQ_U(written, 7u);
    PGW_ASSERT_EQ_I(pgw_p9_fsync(s, nfid, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, nfid, 0), PGW_OK);

    PGW_ASSERT_EQ_I(pgw_p9_renameat(s, dfid, "newdir", dfid, "renamed", 0),
                    PGW_OK);
    uint32_t rfid = bench_walk("renamed/file.txt");
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, rfid, PGW_O_RDONLY, 0), PGW_OK);
    char buf[64];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(s, rfid, 0, buf, sizeof buf, &got, 0), PGW_OK);
    PGW_ASSERT_EQ_U(got, 7u);
    PGW_ASSERT_MEM_EQ(buf, "written", 7);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, rfid, 0), PGW_OK);

    /* setattr truncates, and getattr sees it. */
    uint32_t tfid = bench_walk("renamed/file.txt");
    pgw_p9_setattr_t sa;
    memset(&sa, 0, sizeof sa);
    sa.valid = PGW_P9_SETATTR_SIZE;
    sa.size = 3;
    PGW_ASSERT_EQ_I(pgw_p9_setattr(s, tfid, &sa, 0), PGW_OK);
    pgw_p9_attr a;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(s, tfid, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 3u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, tfid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_create_write_and_rename);

static void test_links_statfs_and_unlink(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    uint32_t root = pgw_p9_session_root_fid(s);
    pgw_p9_qid qid;
    uint32_t dfid = bench_walk("");

    /* symlink and readlink. */
    PGW_ASSERT_EQ_I(pgw_p9_symlink(s, dfid, "lnk", "sub/nested.txt", 0, &qid, 0),
                    PGW_OK);
    uint32_t lfid = bench_walk("lnk");
    char target[64];
    PGW_ASSERT_EQ_I(pgw_p9_readlink(s, lfid, target, sizeof target, 0), PGW_OK);
    PGW_ASSERT_STR_EQ(target, "sub/nested.txt");
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, lfid, 0), PGW_OK);

    /* statfs and a hard link. */
    pgw_p9_statfs_t st;
    PGW_ASSERT_EQ_I(pgw_p9_statfs(s, root, &st, 0), PGW_OK);
    PGW_ASSERT_EQ_U(st.bsize, 4096u);
    PGW_ASSERT_EQ_U(st.namelen, 255u);

    uint32_t hfid = bench_walk("hello.txt");
    PGW_ASSERT_EQ_I(pgw_p9_link(s, dfid, hfid, "hard.txt", 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, hfid, 0), PGW_OK);

    /* unlinkat: a file goes, a non-empty directory does not, a missing name
     * is ENOENT. */
    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(s, dfid, "hard.txt", false, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(s, dfid, "sub", true, 0), PGW_ENOTEMPTY);
    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(s, dfid, "nope", false, 0), PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(s, dfid, "lnk", false, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, dfid, 0), PGW_OK);

    /* Nothing leaked: the server holds the root fid and nothing else. */
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(s), 1u);
    PGW_ASSERT_EQ_U(fake_open_fids(bench_server()), 1u);
    bench_down();
}
PGW_TEST(test_links_statfs_and_unlink);

static void test_flags_are_linux_on_the_wire(void)
{
    bench_up(BENCH_MSIZE, BENCH_MSIZE);
    pgw_p9_session *s = bench_session();
    uint32_t fid = bench_walk("hello.txt");
    PGW_ASSERT_EQ_I(pgw_p9_lopen(s, fid,
                                 PGW_O_RDWR | PGW_O_APPEND | PGW_O_CREAT, 0),
                    PGW_OK);
    /* The fake server stores the flags it received: they must be the Linux
     * numbers of 9P2000.L, not the host's (P9-04). */
    uint32_t wire = 0;
    fake_srv *fs = bench_server();
    for (uint32_t i = 0; i < FAKE_FIDS; i++) {
        if (fs->fids[i].used && fs->fids[i].flags != 0) {
            wire = fs->fids[i].flags;
            break;
        }
    }
    PGW_ASSERT_EQ_U(wire & 3u, 2u);      /* O_RDWR == 2 on Linux */
    PGW_ASSERT((wire & 00002000u) != 0); /* O_APPEND */
    PGW_ASSERT((wire & 00000100u) != 0); /* O_CREAT */
    PGW_ASSERT_EQ_I(pgw_p9_clunk(s, fid, 0), PGW_OK);
    bench_down();
}
PGW_TEST(test_flags_are_linux_on_the_wire);
