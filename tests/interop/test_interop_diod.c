/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/interop/test_interop_diod.c - our 9P2000.L client against diod.
 *
 * The benches of tests/ref9p/ already prove that the client talks to a
 * second implementation, but that implementation was written by us too: a
 * common misreading of the protocol would pass on both sides. This bench
 * closes that hole (gap 59) by driving the *production* client of
 * proto9p/ against diod, a third-party 9P2000.L server, and comparing every
 * result with the truth on disk - the very directory diod exports.
 *
 * The sequence is the realistic one of a client: version, attach, walk,
 * create, write, read, readdir, clunk. Nothing here re-implements 9P: the
 * messages are built by the codec of proto9p/ and the framing by the
 * helper of this directory.
 *
 * When diod is not installed, the bench skips and says why: an absent
 * dependency is never a pass (P9-01, P9-02, P9-03, P9-07, P9-08, P9-10).
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "interop_diod.h"
#include "interop_transport.h"
#include "ref9p_truth.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IX_MSIZE PGW_MSIZE_DEFAULT
#define IX_PATH 512
#define IX_BIG 20000u
#define IX_ENTS 64

struct ix_fixture {
    struct interop_diod *diod;
    pgw_p9_session *session;
    char root[PATH_MAX];
    uint32_t root_fid;
};

static uint8_t g_big[IX_BIG];
static uint8_t g_back[IX_BIG];

/* --- fixture --------------------------------------------------------------- */

static void fixture_up(struct ix_fixture *fx)
{
    memset(fx, 0, sizeof *fx);
    const char *dir = pgw_test_tmpdir("interop-diod");
    PGW_ASSERT(dir != NULL);
    char real[PATH_MAX];
    const char *root = dir;
    if (realpath(dir, real) != NULL)
        root = real;
    snprintf(fx->root, sizeof fx->root, "%s", root);
    char why[1024];
    fx->diod = interop_diod_start(fx->root, why, sizeof why);
    if (fx->diod == NULL) {
        fprintf(stderr, "interop: diod did not start: %s\n", why);
        pgw_fail(__FILE__, __LINE__, "diod did not start: %s", why);
    }
    const char *uname = "passerelle";
    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0')
        uname = pw->pw_name;
    fx->session = pgw_p9_session_open(interop_rpc, interop_diod_rpc_user(fx->diod),
                                      IX_MSIZE, uname, fx->root, (uint32_t)getuid());
    if (fx->session == NULL) {
        char tail[1024];
        interop_diod_log_tail(fx->diod, tail, sizeof tail);
        fprintf(stderr, "interop: no session: %s\n", tail);
        pgw_fail(__FILE__, __LINE__, "the client could not attach to diod");
    }
    fx->root_fid = pgw_p9_session_root_fid(fx->session);
    PGW_ASSERT(fx->root_fid != PGW_FID_NONE);
}

static void fixture_down(struct ix_fixture *fx)
{
    if (pgw_case_failed() && fx->diod != NULL) {
        char tail[2048];
        interop_diod_log_tail(fx->diod, tail, sizeof tail);
        fprintf(stderr, "interop: diod said:\n%s\n", tail);
    }
    if (fx->session != NULL) {
        pgw_p9_session_close(fx->session);
        fx->session = NULL;
    }
    interop_diod_stop(fx->diod);
    fx->diod = NULL;
}

/* The type a real directory entry has. */
static uint8_t truth_dt(const struct stat *st)
{
    if (S_ISLNK(st->st_mode))
        return PGW_DT_LNK;
    if (S_ISDIR(st->st_mode))
        return PGW_DT_DIR;
    return PGW_DT_REG;
}

/* --- cases ----------------------------------------------------------------- */

/* The version and the attach are accepted by a server we did not write, and
 * the msize it grants is the one we asked for or less (P9-01). */
static void test_diod_version_and_attach(void)
{
    struct ix_fixture fx;
    fixture_up(&fx);
    uint32_t msize = pgw_p9_session_msize(fx.session);
    PGW_ASSERT(msize > 0);
    PGW_ASSERT(msize <= IX_MSIZE);
    PGW_ASSERT(fx.root_fid != PGW_FID_NONE);
    fixture_down(&fx);
}
PGW_TEST(test_diod_version_and_attach);

/* A file of the export, read through 9P, is the file on disk (P9-02). */
static void test_diod_walk_and_read_match_truth(void)
{
    struct ix_fixture fx;
    fixture_up(&fx);
    static const char hello[] = "a third-party server\n";
    PGW_ASSERT(ref9p_truth_write(fx.root, "hello.txt", hello, sizeof hello - 1));
    uint32_t fid = 0;
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "hello.txt", &fid, &qid, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(qid.type & PGW_P9_QTDIR, 0u);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, fid, PGW_O_RDONLY, 0), PGW_OK);
    char got[64];
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(fx.session, fid, 0, got, sizeof got, &n, 0), PGW_OK);
    char want[64];
    size_t want_len = 0;
    PGW_ASSERT(ref9p_truth_read(fx.root, "hello.txt", want, sizeof want, &want_len));
    PGW_ASSERT_EQ_U(n, want_len);
    PGW_ASSERT_MEM_EQ(got, want, want_len);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_diod_walk_and_read_match_truth);

/* A file created and written through 9P lands in the export with exactly
 * those bytes, and the client reads them back (P9-03, P9-08). */
static void test_diod_create_write_read_match_truth(void)
{
    struct ix_fixture fx;
    fixture_up(&fx);
    uint32_t dir = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);
    pgw_test_pattern(g_big, sizeof g_big, 0x9E3779B9u, 0);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(fx.session, dir, "big.bin",
                                   PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644, 0, &qid,
                                   0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_p9_write(fx.session, dir, 0, g_big, sizeof g_big, &written, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, sizeof g_big);
    size_t truth_len = 0;
    PGW_ASSERT(ref9p_truth_read(fx.root, "big.bin", g_back, sizeof g_back, &truth_len));
    PGW_ASSERT_EQ_U(truth_len, sizeof g_big);
    PGW_ASSERT(pgw_test_pattern_ok(g_back, sizeof g_back, 0x9E3779B9u, 0));
    size_t back = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(fx.session, dir, 0, g_back, sizeof g_back, &back, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(back, sizeof g_big);
    PGW_ASSERT(pgw_test_pattern_ok(g_back, sizeof g_back, 0x9E3779B9u, 0));
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_diod_create_write_read_match_truth);

/* The listing a third-party server sends is the listing of the directory
 * (P9-02). */
static void test_diod_readdir_matches_truth(void)
{
    struct ix_fixture fx;
    fixture_up(&fx);
    PGW_ASSERT(ref9p_truth_mkdir(fx.root, "sub", 0755));
    PGW_ASSERT(ref9p_truth_write(fx.root, "sub/one.txt", "1", 1));
    PGW_ASSERT(ref9p_truth_write(fx.root, "sub/two.txt", "2", 1));
    uint32_t dir = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "sub", &dir, NULL, 0),
                    PGW_OK);
    /* A directory is read through an open fid, as the protocol requires. */
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, dir, PGW_O_RDONLY, 0), PGW_OK);
    pgw_p9_dirent ents[IX_ENTS];
    size_t count = 0;
    uint64_t next = 0;
    PGW_ASSERT_EQ_I(pgw_p9_readdir(fx.session, dir, 0, ents, IX_ENTS, &count, &next, 0),
                    PGW_OK);
    char names[16][256];
    size_t want = ref9p_truth_names(fx.root, "sub", names, 16);
    PGW_ASSERT(want != (size_t)-1);
    /* diod sends the "." and ".." entries too, the truth does not count
     * them: only the real names are compared. */
    size_t seen = 0;
    for (size_t j = 0; j < count; j++) {
        if (strcmp(ents[j].name, ".") == 0 || strcmp(ents[j].name, "..") == 0)
            continue;
        seen++;
    }
    PGW_ASSERT_EQ_U(seen, want);
    for (size_t i = 0; i < want; i++) {
        struct stat st;
        PGW_ASSERT(ref9p_truth_stat(fx.root, "sub", &st));
        char rel[IX_PATH];
        snprintf(rel, sizeof rel, "sub/%s", names[i]);
        PGW_ASSERT(ref9p_truth_stat(fx.root, rel, &st));
        bool found = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(ents[j].name, names[i]) != 0)
                continue;
            if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0)
                continue;
            found = true;
            PGW_ASSERT_EQ_U(ents[j].type, truth_dt(&st));
        }
        PGW_ASSERT(found);
    }
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    fixture_down(&fx);
}
PGW_TEST(test_diod_readdir_matches_truth);

/* After the session, the only fid left open is the root (P9-07). */
static void test_diod_clunk_releases_the_fids(void)
{
    struct ix_fixture fx;
    fixture_up(&fx);
    uint32_t fid = 0;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &fid, NULL, 0), PGW_OK);
    PGW_ASSERT(pgw_p9_session_open_fids(fx.session) > 1);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(fx.session), 1u);
    fixture_down(&fx);
}
PGW_TEST(test_diod_clunk_releases_the_fids);

/* --- entry point ----------------------------------------------------------- */

int main(int argc, char **argv)
{
    char path[INTEROP_DIOD_PATH_MAX];
    char why[512];
    if (!interop_diod_find(path, sizeof path, why, sizeof why)) {
        printf("PGW_SKIP: %s\n", why);
        printf("1..0 # SKIP diod interoperability: %s\n", why);
        fflush(stdout);
        return 77; /* the CTest SKIP_RETURN_CODE of this bench */
    }
    printf("# diod: %s\n", path);
    fflush(stdout);
    return pgw_test_main(argc, argv);
}
