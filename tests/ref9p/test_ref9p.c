/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/test_ref9p.c - the 9P client against a real 9P2000.L server.
 *
 * Until now the client of proto9p/ was only exercised against an in-memory
 * fake server written by the same author: P9-01..P9-10 were, in that setup,
 * self-referential. This bench replaces the fake by a second implementation -
 * the Python reference server of this directory - and checks the client
 * against the *real* file system, which both sides share:
 *
 *   - the full path (attach, walk, create, write, read, getattr, readdir,
 *     setattr, rename, unlink, mkdir, symlink, readlink, clunk) is compared
 *     with what the directory really contains (P9-02, P9-03, P9-08, P9-10);
 *   - the error codes a real server answers with are checked one by one:
 *     ENOENT, EEXIST, EBADF, ENOTDIR, ELOOP (P9-04);
 *   - the client's validation path is checked against a server that breaks
 *     the protocol on purpose: an unknown errno and a truncated message must
 *     both come back as an error, never as a crash (P9-09, ROB-06).
 *
 * Every case starts its own server and its own temporary directory, so the
 * bench is deterministic and independent from one case to the next.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "ref9p_harness.h"
#include "ref9p_truth.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define FX_MSIZE PGW_MSIZE_DEFAULT
#define FX_PATH 512
#define BIG_SIZE 20000u

struct fixture {
    struct ref9p_server *srv;
    pgw_p9_session *session;
    char root[FX_PATH];
    uint32_t root_fid;
};

static uint8_t g_big[BIG_SIZE];
static uint8_t g_big_back[BIG_SIZE];

/* --- fixture --------------------------------------------------------------- */

static void fixture_up(struct fixture *fx, const char *fault)
{
    memset(fx, 0, sizeof *fx);
    const char *dir = pgw_test_tmpdir("ref9p");
    PGW_ASSERT(dir != NULL);
    snprintf(fx->root, sizeof fx->root, "%s", dir);
    fx->srv = ref9p_start(fx->root, fault);
    PGW_ASSERT(fx->srv != NULL);
    fx->session = pgw_p9_session_open(ref9p_rpc, ref9p_rpc_user(fx->srv), FX_MSIZE,
                                      "baptiste", "", 0);
    PGW_ASSERT(fx->session != NULL);
    fx->root_fid = pgw_p9_session_root_fid(fx->session);
    PGW_ASSERT(fx->root_fid != PGW_FID_NONE);
}

static void fixture_down(struct fixture *fx)
{
    if (fx->session != NULL) {
        pgw_p9_session_close(fx->session);
        fx->session = NULL;
    }
    ref9p_stop(fx->srv);
    fx->srv = NULL;
}

/* The tree both sides see: a file, a directory, a link and a link loop. */
static void make_truth_tree(const char *root)
{
    static const char hello[] = "hello world\n";
    PGW_ASSERT(ref9p_truth_write(root, "hello.txt", hello, sizeof hello - 1));
    PGW_ASSERT(ref9p_truth_mkdir(root, "sub", 0755));
    PGW_ASSERT(ref9p_truth_write(root, "sub/nested.txt", hello, sizeof hello - 1));
    PGW_ASSERT(ref9p_truth_symlink(root, "hello.txt", "alias"));
    PGW_ASSERT(ref9p_truth_symlink(root, "loop", "loop"));
}

/* The permissions a file really ends up with, umask included. */
static uint32_t expected_perms(uint32_t mode)
{
    mode_t um = umask(0);
    umask(um);
    return mode & ~(uint32_t)um & 07777u;
}

static uint8_t truth_dt(const struct stat *st)
{
    if (S_ISLNK(st->st_mode))
        return PGW_DT_LNK;
    if (S_ISDIR(st->st_mode))
        return PGW_DT_DIR;
    return PGW_DT_REG;
}

/* --- cases ----------------------------------------------------------------- */

/* Reading through 9P returns exactly what the real file holds, and the
 * attributes agree with the real stat(). */
static void test_ref9p_read_matches_truth(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);

    uint32_t fid = 0;
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "hello.txt", &fid, &qid, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(qid.type & PGW_P9_QTDIR, 0u);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, fid, PGW_O_RDONLY, 0), PGW_OK);

    char got[64];
    memset(got, 0, sizeof got);
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(fx.session, fid, 0, got, sizeof got, &n, 0), PGW_OK);
    char want[64];
    size_t want_len = 0;
    PGW_ASSERT(ref9p_truth_read(fx.root, "hello.txt", want, sizeof want, &want_len));
    PGW_ASSERT_EQ_U(n, want_len);
    PGW_ASSERT_MEM_EQ(got, want, want_len);

    pgw_p9_attr attr;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(fx.session, fid, &attr, 0), PGW_OK);
    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(fx.root, "hello.txt", &st));
    PGW_ASSERT_EQ_U(attr.size, (uint64_t)st.st_size);
    PGW_ASSERT_EQ_U(attr.mode & 07777u, (uint32_t)(st.st_mode & 07777));
    PGW_ASSERT_EQ_U(attr.mode & S_IFMT, (uint32_t)(st.st_mode & S_IFMT));
    PGW_ASSERT_EQ_U(attr.nlink, (uint64_t)st.st_nlink);
    PGW_ASSERT_EQ_U(attr.uid, (uint32_t)st.st_uid);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_read_matches_truth);

/* A write larger than one message is split (P9-08) and lands in the real
 * file, byte for byte. */
static void test_ref9p_write_is_chunked_and_lands_in_truth(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);
    uint32_t dir = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);

    pgw_test_pattern(g_big, sizeof g_big, 0x9E3779B9u, 0);
    uint64_t splits_before = pgw_stat_get(PGW_STAT_SPLIT);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(fx.session, dir, "big.bin",
                                   PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644, 0, &qid,
                                   0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_p9_write(fx.session, dir, 0, g_big, sizeof g_big, &written, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, sizeof g_big);
    PGW_ASSERT(pgw_stat_get(PGW_STAT_SPLIT) > splits_before);

    /* The real file holds exactly those bytes. */
    size_t truth_len = 0;
    PGW_ASSERT(ref9p_truth_read(fx.root, "big.bin", g_big_back, sizeof g_big_back,
                                &truth_len));
    PGW_ASSERT_EQ_U(truth_len, sizeof g_big);
    PGW_ASSERT(pgw_test_pattern_ok(g_big_back, sizeof g_big_back, 0x9E3779B9u, 0));

    /* And the client reads back what it wrote. */
    size_t read_back = 0;
    PGW_ASSERT_EQ_I(
            pgw_p9_read(fx.session, dir, 0, g_big_back, sizeof g_big_back, &read_back, 0),
            PGW_OK);
    PGW_ASSERT_EQ_U(read_back, sizeof g_big);
    PGW_ASSERT(pgw_test_pattern_ok(g_big_back, sizeof g_big_back, 0x9E3779B9u, 0));
    PGW_ASSERT_EQ_I(pgw_p9_fsync(fx.session, dir, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_write_is_chunked_and_lands_in_truth);

/* The directory listing is the real one, names and types. */
static void test_ref9p_readdir_matches_truth(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);
    uint32_t dir = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);

    pgw_p9_dirent ents[32];
    size_t count = 0;
    uint64_t next = 0;
    PGW_ASSERT_EQ_I(pgw_p9_readdir(fx.session, dir, 0, ents, 32, &count, &next, 0),
                    PGW_OK);
    char names[16][256];
    size_t want = ref9p_truth_names(fx.root, "", names, 16);
    PGW_ASSERT(want != (size_t)-1);
    PGW_ASSERT_EQ_U(count, want);
    for (size_t i = 0; i < want; i++) {
        struct stat st;
        PGW_ASSERT(ref9p_truth_stat(fx.root, names[i], &st));
        bool found = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(ents[j].name, names[i]) != 0)
                continue;
            found = true;
            PGW_ASSERT_EQ_U(ents[j].type, truth_dt(&st));
        }
        PGW_ASSERT(found);
    }
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_readdir_matches_truth);

/* chmod and truncate through Tsetattr reach the real file. */
static void test_ref9p_setattr_matches_truth(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);
    uint32_t fid = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "hello.txt", &fid, NULL, 0),
                    PGW_OK);

    pgw_p9_setattr_t sa;
    memset(&sa, 0, sizeof sa);
    sa.valid = PGW_P9_SETATTR_MODE;
    sa.mode = 0600;
    PGW_ASSERT_EQ_I(pgw_p9_setattr(fx.session, fid, &sa, 0), PGW_OK);
    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(fx.root, "hello.txt", &st));
    PGW_ASSERT_EQ_U((uint32_t)(st.st_mode & 07777), 0600u);

    memset(&sa, 0, sizeof sa);
    sa.valid = PGW_P9_SETATTR_SIZE;
    sa.size = 5;
    PGW_ASSERT_EQ_I(pgw_p9_setattr(fx.session, fid, &sa, 0), PGW_OK);
    PGW_ASSERT(ref9p_truth_stat(fx.root, "hello.txt", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_size, 5u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_setattr_matches_truth);

/* mkdir, symlink, readlink and link all leave exactly what they promise in
 * the real directory. */
static void test_ref9p_create_matches_truth(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);
    uint32_t dir = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);

    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_mkdir(fx.session, dir, "newdir", 0755, 0, &qid, 0), PGW_OK);
    PGW_ASSERT_EQ_U(qid.type & PGW_P9_QTDIR, PGW_P9_QTDIR);
    struct stat st;
    PGW_ASSERT(ref9p_truth_stat(fx.root, "newdir", &st));
    PGW_ASSERT(S_ISDIR(st.st_mode));
    PGW_ASSERT_EQ_U((uint32_t)(st.st_mode & 07777), expected_perms(0755));

    PGW_ASSERT_EQ_I(pgw_p9_symlink(fx.session, dir, "link", "hello.txt", 0, &qid, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(qid.type & PGW_P9_QTSYMLINK, PGW_P9_QTSYMLINK);
    PGW_ASSERT(ref9p_truth_stat(fx.root, "link", &st));
    PGW_ASSERT(S_ISLNK(st.st_mode));

    uint32_t link_fid = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "link", &link_fid, NULL, 0),
                    PGW_OK);
    char target[256];
    PGW_ASSERT_EQ_I(pgw_p9_readlink(fx.session, link_fid, target, sizeof target, 0),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, "hello.txt");
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, link_fid, 0), PGW_OK);

    uint32_t src = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "hello.txt", &src, NULL, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_link(fx.session, dir, src, "hard", 0), PGW_OK);
    PGW_ASSERT(ref9p_truth_stat(fx.root, "hard", &st));
    PGW_ASSERT_EQ_U((uint64_t)st.st_nlink, 2u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, src, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_create_matches_truth);

/* rename, unlink and rmdir are visible in the real directory. */
static void test_ref9p_remove_matches_truth(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);
    PGW_ASSERT(ref9p_truth_write(fx.root, "gone.txt", "x", 1));
    PGW_ASSERT(ref9p_truth_mkdir(fx.root, "emptydir", 0755));
    uint32_t dir = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);

    PGW_ASSERT_EQ_I(pgw_p9_renameat(fx.session, dir, "gone.txt", dir, "moved.txt", 0),
                    PGW_OK);
    PGW_ASSERT_FALSE(ref9p_truth_exists(fx.root, "gone.txt"));
    PGW_ASSERT(ref9p_truth_exists(fx.root, "moved.txt"));

    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(fx.session, dir, "moved.txt", false, 0), PGW_OK);
    PGW_ASSERT_FALSE(ref9p_truth_exists(fx.root, "moved.txt"));

    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(fx.session, dir, "emptydir", true, 0), PGW_OK);
    PGW_ASSERT_FALSE(ref9p_truth_exists(fx.root, "emptydir"));

    /* A symlink created over 9P is removed like any other name. */
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_symlink(fx.session, dir, "tmp", "hello.txt", 0, &qid, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(fx.session, dir, "tmp", false, 0), PGW_OK);
    PGW_ASSERT_FALSE(ref9p_truth_exists(fx.root, "tmp"));

    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    /* P9-07: nothing is left open but the root fid. */
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(fx.session), 1u);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_remove_matches_truth);

/* The five error codes a real server answers with, checked one by one
 * (P9-04: the Linux number must come back as the right internal code). */
static void test_ref9p_error_codes_namespace(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);
    uint32_t dir = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);

    /* ENOENT: a name that is not there, walked and removed. */
    uint32_t missing = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "nope", &missing, NULL, 0),
                    PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_p9_unlinkat(fx.session, dir, "nope", false, 0), PGW_ENOENT);

    /* EEXIST: exclusive creation of a name that already exists. */
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(fx.session, dir, "hello.txt",
                                   PGW_O_RDWR | PGW_O_CREAT | PGW_O_EXCL, 0644, 0, NULL,
                                   0),
                    PGW_EEXIST);

    /* ENOTDIR: walking through a regular file. */
    uint32_t bad = 0;
    PGW_ASSERT_EQ_I(
            pgw_p9_walk_path(fx.session, fx.root_fid, "hello.txt/child", &bad, NULL, 0),
            PGW_ENOTDIR);

    /* ELOOP: a link that points at itself, walked through and opened. */
    PGW_ASSERT_EQ_I(
            pgw_p9_walk_path(fx.session, fx.root_fid, "loop/child", &bad, NULL, 0),
            PGW_ELOOP);
    uint32_t loop = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "loop", &loop, NULL, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, loop, PGW_O_RDONLY, 0), PGW_ELOOP);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, loop, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_error_codes_namespace);

/* EBADF: a fid the server has already forgotten. */
static void test_ref9p_error_codes_ebadf(void)
{
    struct fixture fx;
    fixture_up(&fx, "none");
    make_truth_tree(fx.root);
    uint32_t stale = 0;
    PGW_ASSERT_EQ_I(
            pgw_p9_walk_path(fx.session, fx.root_fid, "hello.txt", &stale, NULL, 0),
            PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, stale, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, stale, PGW_O_RDONLY, 0), PGW_EBADF);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_error_codes_ebadf);

/* P9-09 / ROB-01: an errno no table knows must stay an error. The server
 * answers every request with Rlerror(4095) once the session is attached. */
static void test_ref9p_unknown_errno_is_an_error(void)
{
    struct fixture fx;
    fixture_up(&fx, "unknown-errno");
    pgw_p9_attr attr;
    pgw_err e = pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0);
    PGW_ASSERT(e != PGW_OK);
    PGW_ASSERT_EQ_I(e, PGW_EIO);
    /* The session survived: the next request is refused the same way. */
    e = pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0);
    PGW_ASSERT(e != PGW_OK);
    uint32_t fid = 0;
    e = pgw_p9_walk_path(fx.session, fx.root_fid, "anything", &fid, NULL, 0);
    PGW_ASSERT(e != PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_unknown_errno_is_an_error);

/* P9-09: a message that is well framed but too short for its own type must be
 * rejected, not decoded halfway. The server drops the last byte of every
 * reply body once the session is attached. */
static void test_ref9p_truncated_reply_is_an_error(void)
{
    struct fixture fx;
    fixture_up(&fx, "truncate");
    pgw_p9_attr attr;
    pgw_err e = pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0);
    PGW_ASSERT(e != PGW_OK);
    PGW_ASSERT_EQ_I(e, PGW_EMSGSIZE);
    /* And again: the client keeps refusing, it never crashes. */
    e = pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0);
    PGW_ASSERT(e != PGW_OK);
    char buf[16];
    size_t n = 0;
    uint32_t fid = 0;
    e = pgw_p9_walk_path(fx.session, fx.root_fid, "", &fid, NULL, 0);
    PGW_ASSERT(e != PGW_OK);
    if (e == PGW_OK)
        e = pgw_p9_read(fx.session, fid, 0, buf, sizeof buf, &n, 0);
    PGW_ASSERT(e != PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_ref9p_truncated_reply_is_an_error);
