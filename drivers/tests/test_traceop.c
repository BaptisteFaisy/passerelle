/* SPDX-License-Identifier: Apache-2.0 */
/*
 * drivers/tests/test_traceop.c - what a trace says, checked on the PC.
 *
 * The kernel trace of QNX-09 is the evidence a reviewer reads: it has to say,
 * for every request, which operation it was, at which priority, paired between
 * submission and reception. Two ways for that evidence to be worthless, and both
 * are arithmetic this bench can check without a guest:
 *
 *   - a message the table does not know, labelled with a default operation. The
 *     first version of the 9P table answered PGW_OP__MAX for Twalk - the message
 *     a session sends most - and the transport trace therefore called every
 *     request an OPEN (test_walk_is_lookup, test_every_p9_message_is_labelled);
 *   - a payload the reader cannot decode: an operation or a priority truncated
 *     by the packing (test_pack_round_trip, test_pack_clamps).
 *
 * Covered requirements: QNX-03, QNX-09, PERF-01.
 */
#include "pgw/traceop.h" /* the numbering and the packing, no OS header */
#include "pgw/cookie.h"
#include "pgw/fs9p_op.h"
#include "pgw/fsfuse_op.h"
#include "pgw/p9.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define COOKIE_THREADS 8u
#define COOKIE_PER_THREAD 20000u
#define COOKIE_TOTAL (COOKIE_THREADS * COOKIE_PER_THREAD)

typedef struct cookie_worker {
    pgw_cookie *counter;
    uint32_t *values;
    size_t start;
} cookie_worker;

static void *draw_cookies(void *arg)
{
    cookie_worker *w = arg;
    for (size_t i = 0; i < COOKIE_PER_THREAD; i++)
        w->values[w->start + i] = pgw_cookie_next(w->counter);
    return NULL;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void test_cookie_is_unique_under_contention(void)
{
    pgw_cookie counter = PGW_COOKIE_INITIALIZER;
    pthread_t threads[COOKIE_THREADS];
    cookie_worker workers[COOKIE_THREADS];
    uint32_t *values = malloc(COOKIE_TOTAL * sizeof(*values));
    PGW_ASSERT(values != NULL);
    if (values == NULL)
        return;

    for (size_t i = 0; i < COOKIE_THREADS; i++) {
        workers[i].counter = &counter;
        workers[i].values = values;
        workers[i].start = i * COOKIE_PER_THREAD;
        PGW_ASSERT(pthread_create(&threads[i], NULL, draw_cookies, &workers[i]) == 0);
    }
    for (size_t i = 0; i < COOKIE_THREADS; i++)
        PGW_ASSERT(pthread_join(threads[i], NULL) == 0);

    qsort(values, COOKIE_TOTAL, sizeof(*values), cmp_u32);
    PGW_ASSERT_EQ_U(values[0], 1u);
    for (size_t i = 1; i < COOKIE_TOTAL; i++)
        PGW_ASSERT(values[i] != 0u && values[i] != values[i - 1u]);
    PGW_ASSERT_EQ_U(values[COOKIE_TOTAL - 1u], COOKIE_TOTAL);
    free(values);
}

/* --- the payload ---------------------------------------------------------- */

static void test_pack_round_trip(void)
{
    for (int op = 0; op < (int)PGW_OP__MAX; op++) {
        for (int prio = 1; prio <= 255; prio++) {
            int packed = pgw_trace_pack((enum pgw_op)op, prio);
            PGW_ASSERT_EQ_I(pgw_trace_unpack_op(packed), op);
            PGW_ASSERT_EQ_I(pgw_trace_unpack_prio(packed), prio);
        }
    }
}

static void test_pack_clamps(void)
{
    /* A priority outside the QNX range is clamped, never wrapped: a wrapped one
     * would show the wrong thread and a reader cannot tell. */
    PGW_ASSERT_EQ_I(pgw_trace_unpack_prio(pgw_trace_pack(PGW_OP_READ, -5)), 0);
    PGW_ASSERT_EQ_I(pgw_trace_unpack_prio(pgw_trace_pack(PGW_OP_READ, 300)), 255);
    /* An operation the numbering does not have becomes PGW_OP_OTHER, which is a
     * label, not a silent drop. */
    PGW_ASSERT_EQ_I(pgw_trace_unpack_op(pgw_trace_pack(PGW_OP__MAX, 10)),
                    (int)PGW_OP_OTHER);
    PGW_ASSERT_EQ_I(pgw_trace_unpack_op(pgw_trace_pack((enum pgw_op) - 1, 10)),
                    (int)PGW_OP_OTHER);
}

static void test_prio_now_is_in_range(void)
{
    int prio = pgw_trace_prio_now();
    PGW_ASSERT(prio >= 0 && prio <= 255);
    /* Whatever the bench thread runs at, the packing survives it. */
    PGW_ASSERT_EQ_I(pgw_trace_unpack_prio(pgw_trace_pack(PGW_OP_OPEN, prio)), prio);
}

/* --- the 9P table --------------------------------------------------------- */

static void test_walk_is_lookup(void)
{
    /* The regression this file exists for: Twalk is path resolution, not an
     * open, and it is the message a session sends most. */
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TWALK), (int)PGW_OP_LOOKUP);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_LOOKUP), (int)PGW_OP_LOOKUP);
}

static void test_p9_data_operations(void)
{
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TREAD), (int)PGW_OP_READ);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TWRITE), (int)PGW_OP_WRITE);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TLOPEN), (int)PGW_OP_OPEN);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TLCREATE), (int)PGW_OP_OPEN);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TGETATTR), (int)PGW_OP_STAT);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TSETATTR), (int)PGW_OP_CHMOD);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TREADDIR), (int)PGW_OP_READDIR);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TCLUNK), (int)PGW_OP_CLOSE);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TFSYNC), (int)PGW_OP_SYNC);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TSTATFS), (int)PGW_OP_DEVCTL);
}

static void test_p9_name_operations(void)
{
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TUNLINKAT), (int)PGW_OP_UNLINK);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TREMOVE), (int)PGW_OP_UNLINK);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TRENAMEAT), (int)PGW_OP_RENAME);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TRENAME), (int)PGW_OP_RENAME);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TMKDIR), (int)PGW_OP_MKDIR);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TSYMLINK), (int)PGW_OP_SYMLINK);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TLINK), (int)PGW_OP_LINK);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TREADLINK), (int)PGW_OP_READLINK);
}

static void test_p9_session_messages(void)
{
    /* The session's own messages have no file operation behind them; saying OPEN
     * would invent one. */
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TVERSION), (int)PGW_OP_SESSION);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TAUTH), (int)PGW_OP_SESSION);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TATTACH), (int)PGW_OP_SESSION);
    PGW_ASSERT_EQ_I(fs9p_op_of((uint8_t)PGW_P9_TFLUSH), (int)PGW_OP_SESSION);
}

/* Every message type the protocol defines, so the check below cannot rot when a
 * type is added: an unknown one must still answer with a label. */
static const uint8_t g_p9_all[] = {
        PGW_P9_TLERROR,   PGW_P9_TSTATFS,   PGW_P9_TLOPEN,   PGW_P9_TLCREATE,
        PGW_P9_TSYMLINK,  PGW_P9_TMKNOD,    PGW_P9_TRENAME,  PGW_P9_TREADLINK,
        PGW_P9_TGETATTR,  PGW_P9_TSETATTR,  PGW_P9_TREADDIR, PGW_P9_TFSYNC,
        PGW_P9_TLOCK,     PGW_P9_TGETLOCK,  PGW_P9_TLINK,    PGW_P9_TMKDIR,
        PGW_P9_TRENAMEAT, PGW_P9_TUNLINKAT, PGW_P9_TVERSION, PGW_P9_TAUTH,
        PGW_P9_TATTACH,   PGW_P9_TFLUSH,    PGW_P9_TWALK,    PGW_P9_TREAD,
        PGW_P9_TWRITE,    PGW_P9_TCLUNK,    PGW_P9_TREMOVE,
};

static void test_every_p9_message_is_labelled(void)
{
    /* QNX-09 says "each request": no message may answer PGW_OP__MAX, which is
     * what the emitter drops. */
    for (size_t i = 0; i < sizeof g_p9_all; i++) {
        enum pgw_op op = fs9p_op_of(g_p9_all[i]);
        PGW_ASSERT(op != PGW_OP__MAX);
        PGW_ASSERT(op < PGW_OP__MAX);
    }
}

/* --- the FUSE table ------------------------------------------------------- */

static void test_fuse_operations(void)
{
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_READ), (int)PGW_OP_READ);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_WRITE), (int)PGW_OP_WRITE);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_OPEN), (int)PGW_OP_OPEN);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_CREATE), (int)PGW_OP_OPEN);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_GETATTR), (int)PGW_OP_STAT);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_SETATTR), (int)PGW_OP_CHMOD);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_READDIR), (int)PGW_OP_READDIR);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_RELEASE), (int)PGW_OP_CLOSE);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_FORGET), (int)PGW_OP_CLOSE);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_UNLINK), (int)PGW_OP_UNLINK);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_RENAME), (int)PGW_OP_RENAME);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_MKDIR), (int)PGW_OP_MKDIR);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_SYMLINK), (int)PGW_OP_SYMLINK);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_LINK), (int)PGW_OP_LINK);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_READLINK), (int)PGW_OP_READLINK);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_FSYNC), (int)PGW_OP_SYNC);
    PGW_ASSERT_EQ_I(fsfuse_op_of(PGW_FUSE_STATFS), (int)PGW_OP_DEVCTL);
}

static void test_every_fuse_opcode_is_labelled(void)
{
    /* INIT and DESTROY have no file operation either; they must not be dropped
     * from the trace, and must not be called OPEN. */
    PGW_ASSERT(fsfuse_op_of(PGW_FUSE_INIT) != PGW_OP__MAX);
    PGW_ASSERT(fsfuse_op_of(PGW_FUSE_DESTROY) != PGW_OP__MAX);
    PGW_ASSERT(fsfuse_op_of(PGW_FUSE_INIT) != PGW_OP_OPEN);
    /* An opcode this project does not know still answers. */
    PGW_ASSERT_EQ_I(fsfuse_op_of(0xFFFFu), (int)PGW_OP_OTHER);
}

PGW_TEST(test_cookie_is_unique_under_contention);
PGW_TEST(test_pack_round_trip);
PGW_TEST(test_pack_clamps);
PGW_TEST(test_prio_now_is_in_range);
PGW_TEST(test_walk_is_lookup);
PGW_TEST(test_p9_data_operations);
PGW_TEST(test_p9_name_operations);
PGW_TEST(test_p9_session_messages);
PGW_TEST(test_every_p9_message_is_labelled);
PGW_TEST(test_fuse_operations);
PGW_TEST(test_every_fuse_opcode_is_labelled);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
