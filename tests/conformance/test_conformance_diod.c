/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/test_conformance_diod.c - the rules of diod's
 * description, checked against diod itself.
 *
 * The other benches of this directory check the description against our own
 * client and against the reference server of tests/ref9p/. This one checks it
 * against the third-party server the description belongs to: diod, the
 * 9P2000.L file server of the upstream project. It is the only bench that
 * can prove the rules about the bytes a real server sends (Rlerror with
 * ecode 2, a readdir that ends with an empty reply) and the rule that the
 * aname of the attach selects the export.
 *
 * diod is a package (Debian/Ubuntu: apt-get install diod). When it is not
 * installed the bench skips and says why: an absent dependency is never a
 * pass. The job that installs it and runs this bench for real is
 * .github/workflows/interop.yml.
 *
 * Covers P9-01, P9-02, P9-04, P9-07 and gap 60.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "conformance_table.h"

#include "interop_diod.h"
#include "interop_transport.h"
#include "ref9p_fields.h"
#include "ref9p_truth.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DX_MSIZE PGW_MSIZE_DEFAULT
#define DX_TAG 9u
#define DX_FID 30u
#define DX_RX 16384u

struct dfx {
    struct interop_diod *diod;
    pgw_p9_session *session;
    char root[PATH_MAX];
    uint32_t root_fid;
};

static uint8_t g_rx[DX_RX];
static uint8_t g_tx[4096];

/* The name of the user the client introduces itself with. */
static const char *diod_uname(void)
{
    struct passwd *pw = getpwuid(getuid());
    if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0')
        return pw->pw_name;
    return "passerelle";
}

static void dfx_up(struct dfx *fx)
{
    memset(fx, 0, sizeof *fx);
    const char *dir = pgw_test_tmpdir("conf-diod");
    PGW_ASSERT(dir != NULL);
    char real[PATH_MAX];
    const char *root = dir;
    if (realpath(dir, real) != NULL)
        root = real;
    snprintf(fx->root, sizeof fx->root, "%s", root);
    char why[1024];
    fx->diod = interop_diod_start(fx->root, why, sizeof why);
    if (fx->diod == NULL) {
        fprintf(stderr, "conformance: diod did not start: %s\n", why);
        pgw_fail(__FILE__, __LINE__, "diod did not start: %s", why);
    }
    fx->session =
            pgw_p9_session_open(interop_rpc, interop_diod_rpc_user(fx->diod), DX_MSIZE,
                                diod_uname(), fx->root, (uint32_t)getuid());
    if (fx->session == NULL) {
        pgw_fail(__FILE__, __LINE__, "the client could not attach to diod");
    }
    fx->root_fid = pgw_p9_session_root_fid(fx->session);
    PGW_ASSERT(fx->root_fid != PGW_FID_NONE);
}

static void dfx_down(struct dfx *fx)
{
    if (pgw_case_failed() && fx->diod != NULL) {
        char tail[2048];
        interop_diod_log_tail(fx->diod, tail, sizeof tail);
        fprintf(stderr, "conformance: diod said:\n%s\n", tail);
    }
    if (fx->session != NULL) {
        pgw_p9_session_close(fx->session);
        fx->session = NULL;
    }
    interop_diod_stop(fx->diod);
    fx->diod = NULL;
}

/* One raw exchange with diod: the reply lands in g_rx. */
static size_t diox_call(struct dfx *fx, const void *tx, size_t tx_len)
{
    size_t rx_len = 0;
    pgw_err e = interop_frame(interop_diod_fd(fx->diod), tx, tx_len, g_rx, sizeof g_rx,
                              &rx_len, 5000u);
    PGW_ASSERT_EQ_I(e, PGW_OK);
    PGW_ASSERT(rx_len >= PGW_HEADER_SIZE);
    return rx_len;
}

/* The rule: against the real diod server, version, attach and getattr
 * succeed. */
static void conf_diod_version_attach_getattr(void)
{
    struct dfx fx;
    dfx_up(&fx);
    uint32_t msize = pgw_p9_session_msize(fx.session);
    PGW_ASSERT(msize > 0);
    PGW_ASSERT(msize <= DX_MSIZE);
    pgw_p9_attr attr;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0), PGW_OK);
    PGW_ASSERT((attr.qid.type & PGW_P9_QTDIR) != 0);
    dfx_down(&fx);
}
PGW_TEST(conf_diod_version_attach_getattr);
PGW_CONF_CASE(conf_diod_version_attach_getattr);

/* The readdirs of the description: the first one starts at offset zero,
 * the next ones at the offset of the last entry, and the one that returns
 * no data ends the directory. */
static bool diod_readdir_to_the_end(struct dfx *fx, uint32_t fid)
{
    pgw_p9_dirent ents[64];
    uint64_t offset = 0;
    for (unsigned i = 0; i < 16u; i++) {
        int32_t n = pgw_p9_enc_treaddir(g_tx, sizeof g_tx, DX_TAG, fid, offset, 4096u);
        PGW_ASSERT(n > 0);
        size_t len = diox_call(fx, g_tx, (size_t)n);
        PGW_ASSERT_EQ_U(g_rx[4], PGW_P9_RREADDIR);
        size_t count = 0;
        PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(g_rx, len, DX_TAG, ents, 64, &count),
                        PGW_OK);
        if (count == 0) {
            /* No data: the directory is over. */
            PGW_ASSERT_EQ_U(ref9p_o_u32(g_rx, 7), 0u);
            return true;
        }
        PGW_ASSERT(ents[count - 1].offset > offset);
        offset = ents[count - 1].offset;
    }
    return false;
}

/* The rule: against the real diod server, a directory is cloned, opened, read
 * and clunked, and the readdir that ends the directory returns no data. */
static void conf_diod_readdir_session(void)
{
    struct dfx fx;
    dfx_up(&fx);
    /* The export holds two files, so the directory is not empty. */
    PGW_ASSERT(ref9p_truth_write(fx.root, "a.txt", "a", 1));
    PGW_ASSERT(ref9p_truth_write(fx.root, "b.txt", "b", 1));
    /* Clone the root with a walk of zero names. */
    int32_t n =
            pgw_p9_enc_twalk(g_tx, sizeof g_tx, DX_TAG, fx.root_fid, DX_FID, 0u, NULL);
    PGW_ASSERT(n > 0);
    size_t len = diox_call(&fx, g_tx, (size_t)n);
    PGW_ASSERT_EQ_U(g_rx[4], PGW_P9_RWALK);
    PGW_ASSERT_EQ_U(g_rx[5], (uint8_t)DX_TAG);
    /* A clone answers with no qid at all. */
    PGW_ASSERT_EQ_U(len, 9u);
    /* Open the clone, with the Linux flags of the wire. */
    n = pgw_p9_enc_tlopen(g_tx, sizeof g_tx, DX_TAG, DX_FID, 0u);
    PGW_ASSERT(n > 0);
    len = diox_call(&fx, g_tx, (size_t)n);
    PGW_ASSERT_EQ_U(g_rx[4], PGW_P9_RLOPEN);
    PGW_ASSERT_EQ_U(len, 24u);

    /* The readdirs of the description, until the one that returns no data. */
    PGW_ASSERT(diod_readdir_to_the_end(&fx, DX_FID));
    /* And the fid is clunked. */
    n = pgw_p9_enc_tclunk(g_tx, sizeof g_tx, DX_TAG, DX_FID);
    PGW_ASSERT(n > 0);
    len = diox_call(&fx, g_tx, (size_t)n);
    PGW_ASSERT_EQ_U(g_rx[4], PGW_P9_RCLUNK);
    PGW_ASSERT_EQ_U(len, (size_t)PGW_HEADER_SIZE);
    dfx_down(&fx);
}
PGW_TEST(conf_diod_readdir_session);
PGW_CONF_CASE(conf_diod_readdir_session);

/* The rule: against the real diod server, a missing name is answered with
 * ecode 2. */
static void conf_diod_missing_name_is_enoent(void)
{
    struct dfx fx;
    dfx_up(&fx);
    const char *names[1] = {"nosuchfile"};
    int32_t n =
            pgw_p9_enc_twalk(g_tx, sizeof g_tx, DX_TAG, fx.root_fid, DX_FID, 1u, names);
    PGW_ASSERT(n > 0);
    (void)diox_call(&fx, g_tx, (size_t)n);
    PGW_ASSERT_EQ_U(g_rx[4], PGW_P9_RLERROR);
    PGW_ASSERT_EQ_U(g_rx[5], (uint8_t)DX_TAG);
    /* ecode[4] is a Linux errno: 2 is ENOENT. */
    PGW_ASSERT_EQ_U(ref9p_o_u32(g_rx, 7), 2u);
    dfx_down(&fx);
}
PGW_TEST(conf_diod_missing_name_is_enoent);
PGW_CONF_CASE(conf_diod_missing_name_is_enoent);

/* The rule: against the real diod server, a file is created, written and
 * clunked, and the bytes land in the export. */
static void conf_diod_create_write_clunk(void)
{
    struct dfx fx;
    dfx_up(&fx);
    uint32_t dir = PGW_FID_NONE;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);
    static const char hello[] = "hello\n";
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(fx.session, dir, "foo", PGW_O_RDWR | PGW_O_CREAT, 0644,
                                   (uint32_t)getgid(), &qid, 0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(
            pgw_p9_write(fx.session, dir, 0, hello, sizeof hello - 1, &written, 0),
            PGW_OK);
    PGW_ASSERT_EQ_U(written, sizeof hello - 1);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    char got[64];
    size_t got_len = 0;
    PGW_ASSERT(ref9p_truth_read(fx.root, "foo", got, sizeof got, &got_len));
    PGW_ASSERT_EQ_U(got_len, sizeof hello - 1);
    PGW_ASSERT_MEM_EQ(got, hello, got_len);
    dfx_down(&fx);
}
PGW_TEST(conf_diod_create_write_clunk);
PGW_CONF_CASE(conf_diod_create_write_clunk);

/* The rule: the ecode diod sends is a Linux errno the client can report: the
 * walk fails, and the session keeps working. */
static void conf_diod_rlerror_is_a_linux_errno(void)
{
    struct dfx fx;
    dfx_up(&fx);
    uint32_t fid = PGW_FID_NONE;
    pgw_err e = pgw_p9_walk_path(fx.session, fx.root_fid, "nosuchfile", &fid, NULL, 0);
    PGW_ASSERT(e != PGW_OK);
    /* The session is still usable: the error was reported, not swallowed. */
    pgw_p9_attr attr;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0), PGW_OK);
    dfx_down(&fx);
}
PGW_TEST(conf_diod_rlerror_is_a_linux_errno);
PGW_CONF_CASE(conf_diod_rlerror_is_a_linux_errno);

/* The rule: the attach selects the export named by aname: the files of that
 * export are the ones the client sees. */
static void conf_diod_attach_selects_the_export(void)
{
    struct dfx fx;
    dfx_up(&fx);
    /* A file of the export is visible through 9P... */
    static const char hello[] = "in the export\n";
    PGW_ASSERT(ref9p_truth_write(fx.root, "in-export.txt", hello, sizeof hello - 1));
    uint32_t fid = PGW_FID_NONE;
    PGW_ASSERT_EQ_I(
            pgw_p9_walk_path(fx.session, fx.root_fid, "in-export.txt", &fid, NULL, 0),
            PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, fid, PGW_O_RDONLY, 0), PGW_OK);
    char got[64];
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(fx.session, fid, 0, got, sizeof got, &n, 0), PGW_OK);
    PGW_ASSERT_EQ_U(n, sizeof hello - 1);
    PGW_ASSERT_MEM_EQ(got, hello, n);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    /* ...and a file the client creates lands in that very export. */
    uint32_t dir = PGW_FID_NONE;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(fx.session, dir, "by-client.txt",
                                   PGW_O_RDWR | PGW_O_CREAT, 0644, (uint32_t)getgid(),
                                   &qid, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    PGW_ASSERT(ref9p_truth_exists(fx.root, "by-client.txt"));
    dfx_down(&fx);
}
PGW_TEST(conf_diod_attach_selects_the_export);
PGW_CONF_CASE(conf_diod_attach_selects_the_export);

/* --- entry point ----------------------------------------------------------- */

int main(int argc, char **argv)
{
    char path[INTEROP_DIOD_PATH_MAX];
    char why[512];
    if (!interop_diod_find(path, sizeof path, why, sizeof why)) {
        printf("PGW_SKIP: %s\n", why);
        printf("1..0 # SKIP the diod conformance rules: %s\n", why);
        fflush(stdout);
        return 77; /* the CTest SKIP_RETURN_CODE of this bench */
    }
    printf("# diod: %s\n", path);
    fflush(stdout);
    return pgw_test_main(argc, argv);
}
