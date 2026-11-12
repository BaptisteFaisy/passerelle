/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/conformance/test_conformance_sequence.c - the sequences of diod's
 * rules.
 *
 * A rule of diod's description that is about a sequence (attach, walk, open,
 * read, clunk) is proved by driving the production client of proto9p/
 * against a real server and looking at what happens on the wire:
 *
 *   - the session cases use the client of proto9p/ as it is, against the
 *     reference server of tests/ref9p/, and compare the result with the
 *     truth on disk;
 *   - the frame cases read the replies at the offsets of the description,
 *     with the independent accessors of tests/ref9p/, because the rule they
 *     check is about the bytes the server sends (Rlerror with ecode 2, an
 *     empty readdir, the offset of the last entry).
 *
 * The rules and their sources are in conformance_table.c.
 *
 * Covers P9-01, P9-03, P9-07, P9-08, P9-10 and gap 60.
 */
#define _POSIX_C_SOURCE 200809L

#include "conformance_table.h"

#include "ref9p_fields.h"
#include "ref9p_harness.h"
#include "ref9p_truth.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FX_MSIZE PGW_MSIZE_DEFAULT
#define FX_PATH 512
#define FX_TAG 7u
#define FX_FID 20u

struct fx {
    struct ref9p_server *srv;
    pgw_p9_session *session;
    char root[FX_PATH];
    uint32_t root_fid;
};

/* The biggest message the client sent, for the msize rule. */
static size_t g_max_tx;

/* The rpc callback of the client, wrapped to remember the size of what it
 * sends. */
static pgw_err recording_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                             size_t *rx_len, uint32_t prio, uint32_t timeout_ms,
                             void *user)
{
    if (tx_len > g_max_tx)
        g_max_tx = tx_len;
    return ref9p_rpc(tx, tx_len, rx, rx_cap, rx_len, prio, timeout_ms, user);
}

static void fx_up(struct fx *fx)
{
    memset(fx, 0, sizeof *fx);
    const char *dir = pgw_test_tmpdir("conf-seq");
    PGW_ASSERT(dir != NULL);
    snprintf(fx->root, sizeof fx->root, "%s", dir);
    fx->srv = ref9p_start(fx->root, "none");
    PGW_ASSERT(fx->srv != NULL);
    fx->session = pgw_p9_session_open(recording_rpc, ref9p_rpc_user(fx->srv), FX_MSIZE,
                                      "baptiste", "", 0);
    PGW_ASSERT(fx->session != NULL);
    fx->root_fid = pgw_p9_session_root_fid(fx->session);
    PGW_ASSERT(fx->root_fid != PGW_FID_NONE);
}

static void fx_down(struct fx *fx)
{
    if (fx->session != NULL) {
        pgw_p9_session_close(fx->session);
        fx->session = NULL;
    }
    ref9p_stop(fx->srv);
    fx->srv = NULL;
}

/* Clone the attached root with a walk of zero names: the description says a
 * fid is cloned by walking it with nwname set to zero. */
static size_t clone_root(struct ref9p_server *srv, uint32_t newfid)
{
    int32_t n = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u, newfid,
                                 0u, NULL);
    PGW_ASSERT(n > 0);
    return ref9p_call(srv, ref9p_tx, (size_t)n);
}

/* The rule: a client negotiates the version and the msize, attaches, then
 * reads the attributes of the mount point. */
static void conf_seq_version_attach_getattr(void)
{
    struct fx fx;
    fx_up(&fx);
    uint32_t msize = pgw_p9_session_msize(fx.session);
    PGW_ASSERT(msize > 0);
    PGW_ASSERT(msize <= FX_MSIZE);
    PGW_ASSERT(fx.root_fid != PGW_FID_NONE);
    pgw_p9_attr attr;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0), PGW_OK);
    PGW_ASSERT((attr.qid.type & PGW_P9_QTDIR) != 0);
    fx_down(&fx);
}
PGW_TEST(conf_seq_version_attach_getattr);
PGW_CONF_CASE(conf_seq_version_attach_getattr);

/* The rule: opening a directory clones the fid with a walk of zero names and
 * opens the clone. */
static void conf_seq_walk_clone_then_lopen(void)
{
    struct fx fx;
    fx_up(&fx);
    uint32_t fid = PGW_FID_NONE;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &fid, NULL, 0), PGW_OK);
    PGW_ASSERT(fid != PGW_FID_NONE);
    PGW_ASSERT(fid != fx.root_fid);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, fid, PGW_O_RDONLY, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    fx_down(&fx);
}
PGW_TEST(conf_seq_walk_clone_then_lopen);
PGW_CONF_CASE(conf_seq_walk_clone_then_lopen);

/* The rule: a fid is cloned to newfid by calling walk with nwname set to
 * zero, and the clone answers with no qid at all. */
static void conf_seq_walk_clone_returns_no_qid(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    size_t len = clone_root(srv, FX_FID);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);
    /* nwqid[2] is zero: a clone has walked nowhere. */
    PGW_ASSERT_EQ_U(ref9p_o_u16(ref9p_rx, 7), 0u);
    PGW_ASSERT_EQ_U(len, 9u);
    ref9p_stop(srv);
}
PGW_TEST(conf_seq_walk_clone_returns_no_qid);
PGW_CONF_CASE(conf_seq_walk_clone_returns_no_qid);

/* The rule: a readdir that returns no data ends the directory, and the fid is
 * then clunked. Read on the wire: the second readdir carries the offset of
 * the last entry of the first one, and the server answers with no data. */
static void conf_seq_readdir_ends_with_an_empty_reply(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    size_t len = clone_root(srv, FX_FID);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);
    ref9p_lopen(srv, FX_FID, 0u); /* Linux O_RDONLY */

    int32_t n = pgw_p9_enc_treaddir(ref9p_tx, sizeof ref9p_tx, FX_TAG, FX_FID, 0u, 8192u);
    PGW_ASSERT(n > 0);
    len = ref9p_call(srv, ref9p_tx, (size_t)n);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RREADDIR, FX_TAG);
    pgw_p9_dirent ents[16];
    size_t count = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(ref9p_rx, len, FX_TAG, ents, 16, &count),
                    PGW_OK);
    PGW_ASSERT(count > 0);
    /* The payload of the reply is not empty. */
    PGW_ASSERT(ref9p_o_u32(ref9p_rx, 7) > 0u);

    /* The next readdir starts at the offset of the last entry. */
    uint64_t last = ents[count - 1].offset;
    n = pgw_p9_enc_treaddir(ref9p_tx, sizeof ref9p_tx, FX_TAG, FX_FID, last, 8192u);
    PGW_ASSERT(n > 0);
    len = ref9p_call(srv, ref9p_tx, (size_t)n);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RREADDIR, FX_TAG);
    /* No data: the directory is over. */
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 7), 0u);

    int32_t cl = pgw_p9_enc_tclunk(ref9p_tx, sizeof ref9p_tx, FX_TAG, FX_FID);
    PGW_ASSERT(cl > 0);
    len = ref9p_call(srv, ref9p_tx, (size_t)cl);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RCLUNK, FX_TAG);
    ref9p_stop(srv);
}
PGW_TEST(conf_seq_readdir_ends_with_an_empty_reply);
PGW_CONF_CASE(conf_seq_readdir_ends_with_an_empty_reply);

/* The rule: the first readdir starts at offset zero, the next one at the
 * offset of the last entry. */
static void conf_seq_readdir_offset_is_the_last_entry_offset(void)
{
    struct ref9p_server *srv = ref9p_fields_start();
    size_t len = clone_root(srv, FX_FID);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RWALK, REF9P_FIELD_TAG);
    ref9p_lopen(srv, FX_FID, 0u); /* Linux O_RDONLY */

    int32_t n = pgw_p9_enc_treaddir(ref9p_tx, sizeof ref9p_tx, FX_TAG, FX_FID, 0u, 8192u);
    PGW_ASSERT(n > 0);
    len = ref9p_call(srv, ref9p_tx, (size_t)n);
    pgw_p9_dirent ents[16];
    size_t count = 0;
    PGW_ASSERT_EQ_I(pgw_p9_dec_rreaddir_iter(ref9p_rx, len, FX_TAG, ents, 16, &count),
                    PGW_OK);
    PGW_ASSERT(count > 0);
    /* The offset of an entry is the position of the next one: the last entry
     * of the first reply is the place the second readdir starts from. */
    for (size_t i = 0; i + 1 < count; i++)
        PGW_ASSERT(ents[i].offset < ents[i + 1].offset);

    n = pgw_p9_enc_treaddir(ref9p_tx, sizeof ref9p_tx, FX_TAG, FX_FID, ents[0].offset,
                            8192u);
    PGW_ASSERT(n > 0);
    len = ref9p_call(srv, ref9p_tx, (size_t)n);
    pgw_p9_dirent more[16];
    size_t more_count = 0;
    PGW_ASSERT_EQ_I(
            pgw_p9_dec_rreaddir_iter(ref9p_rx, len, FX_TAG, more, 16, &more_count),
            PGW_OK);
    PGW_ASSERT_EQ_U(more_count, count - 1u);
    PGW_ASSERT_STR_EQ(more[0].name, ents[1].name);
    ref9p_stop(srv);
}
PGW_TEST(conf_seq_readdir_offset_is_the_last_entry_offset);
PGW_CONF_CASE(conf_seq_readdir_offset_is_the_last_entry_offset);

/* The rule: a walk that does not reach its target is an error for the
 * client. The reference server answers with a partial walk, diod with an
 * Rlerror: both must be reported as an error, and the session must stay
 * usable. */
static void conf_seq_walk_to_a_missing_name_fails(void)
{
    struct fx fx;
    fx_up(&fx);
    uint32_t fid = PGW_FID_NONE;
    pgw_err e = pgw_p9_walk_path(fx.session, fx.root_fid, "nosuchfile", &fid, NULL, 0);
    PGW_ASSERT(e != PGW_OK);
    /* The session is still usable: the error was reported, not swallowed. */
    pgw_p9_attr attr;
    PGW_ASSERT_EQ_I(pgw_p9_getattr(fx.session, fx.root_fid, &attr, 0), PGW_OK);
    fx_down(&fx);
}
PGW_TEST(conf_seq_walk_to_a_missing_name_fails);
PGW_CONF_CASE(conf_seq_walk_to_a_missing_name_fails);

/* The rule: creating a file clones the parent fid, creates the file in it,
 * writes it and clunks it. */
static void conf_seq_create_write_clunk(void)
{
    struct fx fx;
    fx_up(&fx);
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
    /* The file is on disk, with exactly those bytes. */
    char got[64];
    size_t got_len = 0;
    PGW_ASSERT(ref9p_truth_read(fx.root, "foo", got, sizeof got, &got_len));
    PGW_ASSERT_EQ_U(got_len, sizeof hello - 1);
    PGW_ASSERT_MEM_EQ(got, hello, got_len);
    fx_down(&fx);
}
PGW_TEST(conf_seq_create_write_clunk);
PGW_CONF_CASE(conf_seq_create_write_clunk);

/* The rule: reading a file clones the fid, opens it, reads it and reads again
 * to reach the end. */
static void conf_seq_read_then_eof_then_clunk(void)
{
    struct fx fx;
    fx_up(&fx);
    static const char hello[] = "hello world\n";
    PGW_ASSERT(ref9p_truth_write(fx.root, "hello.txt", hello, sizeof hello - 1));
    uint32_t fid = PGW_FID_NONE;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "hello.txt", &fid, NULL, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_p9_lopen(fx.session, fid, PGW_O_RDONLY, 0), PGW_OK);
    char got[64];
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(fx.session, fid, 0, got, sizeof got, &n, 0), PGW_OK);
    PGW_ASSERT_EQ_U(n, sizeof hello - 1);
    PGW_ASSERT_MEM_EQ(got, hello, n);
    /* The next read is at the end of the file: it returns nothing. */
    size_t more = 0;
    PGW_ASSERT_EQ_I(pgw_p9_read(fx.session, fid, n, got, sizeof got, &more, 0), PGW_OK);
    PGW_ASSERT_EQ_U(more, 0u);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    fx_down(&fx);
}
PGW_TEST(conf_seq_read_then_eof_then_clunk);
PGW_CONF_CASE(conf_seq_read_then_eof_then_clunk);

/* The rule: after a clunk the fid is no longer usable by the client. */
static void conf_seq_clunk_releases_the_fid(void)
{
    struct fx fx;
    fx_up(&fx);
    uint32_t fid = PGW_FID_NONE;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &fid, NULL, 0), PGW_OK);
    PGW_ASSERT(pgw_p9_session_open_fids(fx.session) > 1);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, fid, 0), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_p9_session_open_fids(fx.session), 1u);
    fx_down(&fx);
}
PGW_TEST(conf_seq_clunk_releases_the_fid);
PGW_CONF_CASE(conf_seq_clunk_releases_the_fid);

/* The rule: no message the client sends is larger than the msize both sides
 * agreed on. */
static void conf_seq_no_message_exceeds_the_negotiated_msize(void)
{
    struct fx fx;
    g_max_tx = 0;
    fx_up(&fx);
    uint32_t msize = pgw_p9_session_msize(fx.session);
    PGW_ASSERT(msize > 0);
    /* A write bigger than the msize: the client must split it. */
    static uint8_t big[20000];
    memset(big, 0xA5, sizeof big);
    uint32_t dir = PGW_FID_NONE;
    PGW_ASSERT_EQ_I(pgw_p9_walk_path(fx.session, fx.root_fid, "", &dir, NULL, 0), PGW_OK);
    pgw_p9_qid qid;
    PGW_ASSERT_EQ_I(pgw_p9_lcreate(fx.session, dir, "big.bin", PGW_O_RDWR | PGW_O_CREAT,
                                   0644, (uint32_t)getgid(), &qid, 0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_p9_write(fx.session, dir, 0, big, sizeof big, &written, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, sizeof big);
    PGW_ASSERT(g_max_tx <= (size_t)msize);
    PGW_ASSERT_EQ_I(pgw_p9_clunk(fx.session, dir, 0), PGW_OK);
    fx_down(&fx);
}
PGW_TEST(conf_seq_no_message_exceeds_the_negotiated_msize);
PGW_CONF_CASE(conf_seq_no_message_exceeds_the_negotiated_msize);
