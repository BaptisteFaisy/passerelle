/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/faulty/test_faulty.c - the real 9P client against a server that breaks
 * the protocol on purpose (ROB-07, P9-09, ROB-06, ROB-05).
 *
 * Gap 20 of docs/gap-analysis.md was "a deliberately faulty server must not
 * crash the client" and no faulty server existed to prove it. faulty_server.py
 * in this directory is that server: it answers correctly until the session is
 * attached, then breaks exactly one rule of 9P2000.L or of the transport, on
 * every reply. This bench drives the production client (proto9p/) against it
 * and asserts, for every armed fault:
 *
 *   1. the client answers with the error that rule deserves - never PGW_OK,
 *      never a partial result (P9-09);
 *   2. it does not hang - the operation stays inside a time budget while the
 *      session's own deadline is 300 ms, so a silent server is bounded
 *      (ROB-05);
 *   3. it leaks nothing - after a refused operation exactly one fid is still
 *      open, the root one (P9-07), and the session closes cleanly;
 *   4. it does not crash - under -DPGW_SANITIZERS=ON the same run proves that
 *      no reply is read out of bounds and that no allocation escapes
 *      (ROB-06). It already found one: a server closing the connection in the
 *      middle of a frame killed the whole client with SIGPIPE during the
 *      closing clunk, which the transport now reports as PGW_EIO.
 *
 * The transport is not written a second time: the harness of tests/ref9p/
 * starts the server, frames the messages and kills the child, and the CMake
 * file of this directory points it at faulty_server.py.
 */
#define _POSIX_C_SOURCE 200809L

#include "ref9p_harness.h"
#include "ref9p_truth.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/log.h"
#include "pgw/p9.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define FAULTY_MSIZE PGW_MSIZE_DEFAULT
/* Short on purpose: a fault that makes the server silent must be bounded by
 * the client's own deadline, not by the test's patience. */
#define CLIENT_TIMEOUT_MS 300u
/* One operation must be refused inside this budget, timeouts included. */
#define OP_BUDGET_S 15.0
#define OPEN_BUDGET_S 20.0

enum fault_cat { CAT_HANDSHAKE, CAT_MESSAGE, CAT_TRANSPORT };

struct fault_case {
    const char *mode; /* --fault of faulty_server.py */
    enum fault_cat cat;
    pgw_err want;     /* the error the client must answer with */
    pgw_err alt;      /* a second accepted error, PGW_OK when there is none */
    const char *rule; /* the protocol rule the server breaks */
};

/* Message faults are refused either by the envelope check (EMSGSIZE: size[4]
 * disagrees with the frame) or by the field validation (EINVAL: the message is
 * well framed but cannot be what it claims to be). Both are a refusal. */
static const struct fault_case g_cases[] = {
        {"version-mismatch", CAT_HANDSHAKE, PGW_OK, PGW_OK,
         "Rversion answers 9P2000, not .L"},
        {"truncate", CAT_MESSAGE, PGW_EINVAL, PGW_EMSGSIZE,
         "body shorter than its own type needs"},
        {"short-frame", CAT_MESSAGE, PGW_EMSGSIZE, PGW_OK,
         "size[4] larger than the frame received"},
        {"oversize", CAT_MESSAGE, PGW_EMSGSIZE, PGW_OK,
         "size[4] claims 2 GiB for a short message"},
        {"bad-tag", CAT_MESSAGE, PGW_EINVAL, PGW_OK,
         "reply to a tag the client never sent"},
        {"out-of-order", CAT_MESSAGE, PGW_EINVAL, PGW_OK,
         "reply carrying the tag of another request"},
        {"garbage", CAT_MESSAGE, PGW_EMSGSIZE, PGW_EINVAL,
         "bytes that are not a 9P message at all"},
        {"read-count-lie", CAT_MESSAGE, PGW_EMSGSIZE, PGW_EINVAL,
         "Rread count above the payload it carries"},
        {"walk-overflow", CAT_MESSAGE, PGW_EINVAL, PGW_EMSGSIZE,
         "Rwalk nwname above the qids it carries"},
        {"unsolicited", CAT_TRANSPORT, PGW_EINVAL, PGW_OK,
         "reply to a request that was never sent"},
        {"huge-frame", CAT_TRANSPORT, PGW_EMSGSIZE, PGW_OK, "frame prefix claims 4 GiB"},
        {"never-ends", CAT_TRANSPORT, PGW_ETIMEDOUT, PGW_OK,
         "frame whose promised bytes never arrive"},
        {"late-reply", CAT_TRANSPORT, PGW_ETIMEDOUT, PGW_OK,
         "reply sent after the client gave up"},
        {"close-mid-frame", CAT_TRANSPORT, PGW_EIO, PGW_OK,
         "connection dies inside a frame"},
};
#define CASE_COUNT (sizeof g_cases / sizeof g_cases[0])

static double since_s(const struct timespec *t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - t0->tv_sec) + (double)(now.tv_nsec - t0->tv_nsec) / 1e9;
}

/* The tree the faulty server serves: one file, enough for the data path. */
static void make_tree(const char *root)
{
    static const char hello[] = "hello world\n";
    PGW_ASSERT(ref9p_truth_write(root, "hello.txt", hello, sizeof hello - 1));
    PGW_ASSERT(ref9p_truth_mkdir(root, "sub", 0755));
}

/* Walk, open and read: the three steps a malformed reply can break. Returns
 * the first error, or PGW_OK when the client accepted everything. */
static pgw_err drive_data_path(pgw_p9_session *s)
{
    uint32_t fid = PGW_FID_NONE;
    pgw_p9_qid qid;
    pgw_err e =
            pgw_p9_walk_path(s, pgw_p9_session_root_fid(s), "hello.txt", &fid, &qid, 0);
    if (e != PGW_OK)
        return e;
    e = pgw_p9_lopen(s, fid, PGW_O_RDONLY, 0);
    if (e == PGW_OK) {
        char buf[64];
        size_t got = 0;
        e = pgw_p9_read(s, fid, 0, buf, sizeof buf, &got, 0);
        if (e == PGW_OK && got == 0)
            e = PGW_EIO; /* the file is not empty: a silent 0 is a lie */
    }
    (void)pgw_p9_clunk(s, fid, 0);
    return e;
}

static pgw_err start_session(const struct fault_case *c, const char *root,
                             pgw_p9_session **out, struct ref9p_server **srv)
{
    *srv = ref9p_start(root, c->mode);
    if (*srv == NULL)
        return PGW_EIO; /* the server itself refused to start: not a result */
    *out = pgw_p9_session_open(ref9p_rpc, ref9p_rpc_user(*srv), FAULTY_MSIZE, "tester",
                               "", 0);
    return PGW_OK;
}

/* A handshake fault must leave the caller with no session at all. */
static void check_open_refused(const struct fault_case *c, const char *root)
{
    pgw_p9_session *s = NULL;
    struct ref9p_server *srv = NULL;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    PGW_ASSERT_EQ_I(start_session(c, root, &s, &srv), PGW_OK);
    double dt = since_s(&t0);
    if (s != NULL)
        pgw_p9_session_close(s);
    ref9p_stop(srv);
    if (s != NULL)
        pgw_fail(__FILE__, __LINE__,
                 "fault '%s' (%s) was accepted: a session "
                 "opened",
                 c->mode, c->rule);
    if (dt > OPEN_BUDGET_S)
        pgw_fail(__FILE__, __LINE__, "fault '%s' took %.1fs to refuse", c->mode, dt);
}

/* The assertions of check_op_refused(), out of line to keep it readable. */
static void report_violation(const struct fault_case *c, pgw_err e, double dt,
                             uint32_t fids)
{
    bool wanted = (e == c->want) || (c->alt != PGW_OK && e == c->alt);
    if (!wanted)
        pgw_fail(__FILE__, __LINE__, "fault '%s' (%s): got %s, want %s", c->mode, c->rule,
                 pgw_err_name(e), pgw_err_name(c->want));
    if (dt > OP_BUDGET_S)
        pgw_fail(__FILE__, __LINE__,
                 "fault '%s' took %.1fs: the client hung "
                 "instead of giving up",
                 c->mode, dt);
    if (fids != 1)
        pgw_fail(__FILE__, __LINE__,
                 "fault '%s' left %u fid(s) open, want 1 "
                 "(P9-07)",
                 c->mode, fids);
}

/* A data-path fault must be refused with the right error, bounded and
 * leak-free. */
static void check_op_refused(const struct fault_case *c, const char *root)
{
    pgw_p9_session *s = NULL;
    struct ref9p_server *srv = NULL;
    PGW_ASSERT_EQ_I(start_session(c, root, &s, &srv), PGW_OK);
    if (s == NULL) {
        ref9p_stop(srv);
        pgw_fail(__FILE__, __LINE__, "fault '%s': the clean handshake failed", c->mode);
    }
    pgw_p9_session_set_timeout(s, CLIENT_TIMEOUT_MS);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pgw_err e = drive_data_path(s);
    double dt = since_s(&t0);
    uint32_t fids = pgw_p9_session_open_fids(s);
    pgw_p9_session_close(s);
    ref9p_stop(srv);
    report_violation(c, e, dt, fids);
}

static void run_category(enum fault_cat cat)
{
    const char *root = pgw_test_tmpdir("faulty");
    PGW_ASSERT(root != NULL);
    make_tree(root);
    for (size_t i = 0; i < CASE_COUNT; i++) {
        if (g_cases[i].cat != cat)
            continue;
        if (cat == CAT_HANDSHAKE)
            check_open_refused(&g_cases[i], root);
        else
            check_op_refused(&g_cases[i], root);
    }
}

/* P9-01 / ROB-07: a server that does not speak 9P2000.L gets no session. */
static void test_faulty_handshake_violation_is_refused(void)
{
    run_category(CAT_HANDSHAKE);
}
PGW_TEST(test_faulty_handshake_violation_is_refused);

/* P9-09 / ROB-07: a message that breaks its own rules is refused. */
static void test_faulty_message_violations_are_refused(void)
{
    run_category(CAT_MESSAGE);
}
PGW_TEST(test_faulty_message_violations_are_refused);

/* ROB-05 / ROB-07: a transport that lies, stalls or dies is refused. */
static void test_faulty_transport_violations_are_refused(void)
{
    run_category(CAT_TRANSPORT);
}
PGW_TEST(test_faulty_transport_violations_are_refused);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
