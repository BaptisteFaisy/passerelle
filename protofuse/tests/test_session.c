/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/test_session.c - End-to-end session tests against the
 * in-memory virtiofs server (protofuse/tests/fake_virtiofsd.c).
 *
 * Implements requirements:
 *   - VFS-02: FORGET and INTERRUPT reach the transport with `hiprio = true` and
 *     are submitted before ordinary requests; ordinary requests are ordered by
 *     caller priority
 *   - VFS-03: INIT negotiation, including the fallback to an older server, and
 *     the whole file operation set
 *   - VFS-04: references are added and given back
 *   - ROB-06: a corrupted, truncated or empty reply is refused
 */
#include "fake_virtiofsd.h"

#include "fusep_reply.h"
#include "fusep_session.h"

#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/log.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

/* --- INIT negotiation ----------------------------------------------------- */

static void test_negotiation_uses_server_minor(void)
{
    fake_virtiofsd *f = fv_new(12, 65536); /* an old virtiofsd */
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    PGW_ASSERT_EQ_U(f->init_calls, 1);
    PGW_ASSERT_EQ_U(s->minor, 12);
    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_negotiation_caps_at_our_minor(void)
{
    fake_virtiofsd *f = fv_new(40, 65536); /* newer than our header */
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    PGW_ASSERT_EQ_U(s->minor, PGW_FUSE_MINOR);
    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_negotiation_refuses_other_major(void)
{
    fake_virtiofsd *f = fv_new(31, 65536);
    PGW_ASSERT(f != NULL);
    f->major = 8; /* we only speak 7.x */
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT_EQ_PTR(s, NULL);
    fv_free(f);
}

static void test_negotiation_refuses_too_old_minor(void)
{
    fake_virtiofsd *f = fv_new(4, 65536); /* before blksize, unusable */
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT_EQ_PTR(s, NULL);
    fv_free(f);
}

static void test_negotiation_takes_max_write_from_server(void)
{
    fake_virtiofsd *f = fv_new(31, 4096);
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    PGW_ASSERT_EQ_U(s->max_write, 4096); /* the smaller of the two wins */
    pgw_fuse_session_close(s);
    fv_free(f);
}

/* --- priority lane -------------------------------------------------------- */

/* Encode a GETATTR and queue it with the given caller priority. The request
 * buffer must outlive the submission, so it comes from a static pool. */
static uint64_t queue_getattr(pgw_fuse_session *s, uint32_t prio)
{
    static uint8_t pool[4][128];
    static size_t slot;
    uint8_t *tx = pool[slot++ % 4u];
    uint64_t u = pgw_fusep_next_unique(s);
    int32_t n = pgw_fuse_enc_getattr(tx, sizeof(pool[0]), u, PGW_FUSE_ROOT_ID,
                                     0, false);
    PGW_ASSERT(n > 0);
    static pgw_fuse_attr attr[4];
    static pgw_fusep_rep1 c[4];
    size_t k = (slot - 1u) % 4u;
    c[k].unique = u;
    c[k].a = &attr[k];
    PGW_ASSERT_EQ_I(pgw_fusep_submit(s, tx, (size_t)n, false, true, prio,
                                     pgw_fusep_done_attr, &c[k]),
                    PGW_OK);
    return u;
}

static void test_priority_lane_first(void)
{
    fake_virtiofsd *f = fv_new(31, 65536);
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    uint8_t forget[128];
    uint64_t u_fg = pgw_fusep_next_unique(s);
    int32_t n_fg = pgw_fuse_enc_forget(forget, sizeof(forget), u_fg, 42, 1);
    PGW_ASSERT(n_fg > 0);

    f->op_log_len = 0;
    /* INIT was the first request: start the lane counters from here. */
    s->stat_hiprio = 0;
    s->stat_normal = 0;
    /* Enqueue in a deliberately bad order: low priority, high priority, then
     * the priority-lane FORGET last. */
    uint64_t u_low = queue_getattr(s, 5);
    uint64_t u_hi = queue_getattr(s, 20);
    PGW_ASSERT_EQ_I(pgw_fusep_submit(s, forget, (size_t)n_fg, true, false, 0,
                                     NULL, NULL),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fusep_pump(s), PGW_OK);

    PGW_ASSERT_EQ_U(f->op_log_len, 3);
    /* FORGET first, because it is on the priority lane. */
    PGW_ASSERT_EQ_U(f->op_log[0], PGW_FUSE_FORGET);
    PGW_ASSERT(f->hiprio_log[0]);
    /* Then the higher caller priority, then the lower one. */
    PGW_ASSERT_EQ_U(f->unique_log[1], u_hi);
    PGW_ASSERT_FALSE(f->hiprio_log[1]);
    PGW_ASSERT_EQ_U(f->unique_log[2], u_low);
    PGW_ASSERT_EQ_U(s->stat_hiprio, 1);
    PGW_ASSERT_EQ_U(s->stat_normal, 2);

    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_forget_and_interrupt_are_hiprio(void)
{
    fake_virtiofsd *f = fv_new(31, 65536);
    PGW_ASSERT(f != NULL);
    PGW_ASSERT(fv_add(f, f->root, "file", false) != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "file", &e, 0), PGW_OK);
    f->op_log_len = 0;

    PGW_ASSERT_EQ_I(pgw_fuse_forget_node(s, e.nodeid), PGW_OK);
    PGW_ASSERT_EQ_U(f->op_log[0], PGW_FUSE_FORGET);
    PGW_ASSERT(f->hiprio_log[0]);

    PGW_ASSERT_EQ_I(pgw_fuse_interrupt(s, 1234), PGW_OK);
    PGW_ASSERT_EQ_U(f->op_log[1], PGW_FUSE_INTERRUPT);
    PGW_ASSERT(f->hiprio_log[1]);

    pgw_fuse_session_close(s);
    fv_free(f);
}

/* --- full session --------------------------------------------------------- */

static fake_virtiofsd *make_tree(void)
{
    fake_virtiofsd *f = fv_new(31, 65536);
    if (f == NULL)
        return NULL;
    fv_node *file = fv_add(f, f->root, "hello.txt", false);
    if (file == NULL)
        return f;
    PGW_ASSERT(fv_set_size(file, 5));
    memcpy(file->data, "hello", 5);
    PGW_ASSERT(fv_add(f, f->root, "sub", true) != NULL);
    PGW_ASSERT(fv_add(f, f->root, "lnk", false) != NULL);
    return f;
}

/* LOOKUP + GETATTR + OPEN + READ + RELEASE on a regular file. */
static void full_session_metadata(pgw_fuse_session *s, uint64_t *file_id)
{
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "hello.txt", &e, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I((int)pgw_ftype_from_linux_mode(e.attr.mode), PGW_FT_REG);
    pgw_fuse_attr a;
    PGW_ASSERT_EQ_I(pgw_fuse_getattr(s, e.nodeid, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 5);
    *file_id = e.nodeid;

    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(s, e.nodeid, PGW_O_RDONLY, false, &fh, 0),
                    PGW_OK);
    char buf[16];
    size_t got = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_read(s, e.nodeid, fh, 0, buf, 5, &got, 0), PGW_OK);
    PGW_ASSERT_EQ_U(got, 5);
    PGW_ASSERT_MEM_EQ(buf, "hello", 5);
    PGW_ASSERT_EQ_I(pgw_fuse_release(s, e.nodeid, fh, false, 0), PGW_OK);
}

/* CREATE + WRITE + SETATTR + RELEASE. */
static void full_session_create(pgw_fuse_session *s, uint64_t *new_id)
{
    pgw_fuse_entry ce;
    uint64_t cfh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_create(s, PGW_FUSE_ROOT_ID, "out.bin", PGW_O_RDWR,
                                    0644, &ce, &cfh, 0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_write(s, ce.nodeid, cfh, 0, "abcdef", 6, &written,
                                   0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, 6);
    pgw_fuse_setattr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.valid = PGW_FUSE_FATTR_MODE | PGW_FUSE_FATTR_SIZE;
    sa.mode = 0600;
    sa.size = 3;
    pgw_fuse_attr a;
    PGW_ASSERT_EQ_I(pgw_fuse_setattr(s, ce.nodeid, &sa, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 3);
    PGW_ASSERT_EQ_I(pgw_fuse_release(s, ce.nodeid, cfh, false, 0), PGW_OK);
    *new_id = ce.nodeid;
}

/* MKDIR + READDIR + RMDIR. */
static void full_session_directory(pgw_fuse_session *s)
{
    pgw_fuse_entry de;
    PGW_ASSERT_EQ_I(pgw_fuse_mkdir(s, PGW_FUSE_ROOT_ID, "newdir", 0755, &de, 0),
                    PGW_OK);
    uint64_t dfh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(s, PGW_FUSE_ROOT_ID, PGW_O_RDONLY, true, &dfh,
                                  0),
                    PGW_OK);
    pgw_fuse_dirent ents[16];
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_readdir(s, PGW_FUSE_ROOT_ID, dfh, 0, ents, 16, &n,
                                     0),
                    PGW_OK);
    PGW_ASSERT(n >= 4);
    PGW_ASSERT_EQ_I(pgw_fuse_release(s, PGW_FUSE_ROOT_ID, dfh, true, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_unlink(s, PGW_FUSE_ROOT_ID, "newdir", true, 0),
                    PGW_OK);
}

/* SYMLINK + READLINK, LINK, RENAME, STATFS, FSYNC and UNLINK. */
static void full_session_links(fake_virtiofsd *f, pgw_fuse_session *s,
                               uint64_t file_id, uint64_t new_id)
{
    pgw_fuse_entry se;
    PGW_ASSERT_EQ_I(pgw_fuse_symlink(s, PGW_FUSE_ROOT_ID, "sym", "/target",
                                     &se, 0),
                    PGW_OK);
    char target[64];
    PGW_ASSERT_EQ_I(pgw_fuse_readlink(s, se.nodeid, target, sizeof(target), 0),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, "/target");

    pgw_fuse_entry le;
    PGW_ASSERT_EQ_I(pgw_fuse_link(s, PGW_FUSE_ROOT_ID, file_id, "hard", &le, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_rename(s, PGW_FUSE_ROOT_ID, "hard",
                                    PGW_FUSE_ROOT_ID, "hard2", 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(fv_path_id(f, "/hard2"), le.nodeid);

    pgw_fuse_kstatfs st;
    PGW_ASSERT_EQ_I(pgw_fuse_statfs(s, PGW_FUSE_ROOT_ID, &st, 0), PGW_OK);
    PGW_ASSERT_EQ_U(st.bsize, 4096);
    PGW_ASSERT_EQ_U(st.namelen, 255);
    PGW_ASSERT_EQ_I(pgw_fuse_fsync(s, new_id, 0, false, 0), PGW_OK);

    PGW_ASSERT_EQ_I(pgw_fuse_unlink(s, PGW_FUSE_ROOT_ID, "out.bin", false, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(fv_path_id(f, "/out.bin"), 0);
}

static void test_full_session(void)
{
    fake_virtiofsd *f = make_tree();
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    uint64_t file_id = 0;
    uint64_t new_id = 0;
    full_session_metadata(s, &file_id);
    full_session_create(s, &new_id);
    full_session_directory(s);
    full_session_links(f, s, file_id, new_id);

    pgw_fuse_session_close(s);
    fv_free(f);
}

/* --- chunking ------------------------------------------------------------- */

static void test_read_and_write_are_split(void)
{
    fake_virtiofsd *f = fv_new(31, 4096);
    PGW_ASSERT(f != NULL);
    fv_node *file = fv_add(f, f->root, "big", false);
    PGW_ASSERT(file != NULL);
    PGW_ASSERT(fv_set_size(file, 10000));
    pgw_test_pattern(file->data, 10000, 0xC0FFEEu, 0);

    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 4096);
    PGW_ASSERT(s != NULL);
    PGW_ASSERT_EQ_U(s->max_write, 4096);

    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(s, file->nodeid, PGW_O_RDONLY, false, &fh, 0),
                    PGW_OK);
    static uint8_t got[10000];
    memset(got, 0, sizeof(got));
    size_t n = 0;
    f->op_log_len = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_read(s, file->nodeid, fh, 0, got, sizeof(got), &n,
                                  0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(n, 10000);
    PGW_ASSERT(pgw_test_pattern_ok(got, 10000, 0xC0FFEEu, 0));
    /* 4096 + 4096 + 1808: three READ requests. */
    size_t reads = 0;
    for (size_t i = 0; i < f->op_log_len; i++) {
        if (f->op_log[i] == (uint32_t)PGW_FUSE_READ)
            reads++;
    }
    PGW_ASSERT_EQ_U(reads, 3);

    /* The same in the other direction. */
    static uint8_t payload[10000];
    pgw_test_pattern(payload, sizeof(payload), 0x1234u, 0);
    PGW_ASSERT_EQ_I(pgw_fuse_write(s, file->nodeid, fh, 0, payload,
                                   sizeof(payload), &n, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(n, 10000);
    PGW_ASSERT(pgw_test_pattern_ok(file->data, 10000, 0x1234u, 0));
    PGW_ASSERT_EQ_U(fv_node_by_id(f, file->nodeid)->size, 10000);

    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_read_stops_at_eof(void)
{
    fake_virtiofsd *f = fv_new(31, 65536);
    PGW_ASSERT(f != NULL);
    fv_node *file = fv_add(f, f->root, "short", false);
    PGW_ASSERT(file != NULL);
    PGW_ASSERT(fv_set_size(file, 3));
    memcpy(file->data, "abc", 3);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(s, file->nodeid, PGW_O_RDONLY, false, &fh, 0),
                    PGW_OK);
    char buf[64];
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_read(s, file->nodeid, fh, 0, buf, sizeof(buf), &n,
                                  0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(n, 3);
    PGW_ASSERT_MEM_EQ(buf, "abc", 3);
    /* Past the end: an empty answer, not an error. */
    PGW_ASSERT_EQ_I(pgw_fuse_read(s, file->nodeid, fh, 100, buf, sizeof(buf),
                                  &n, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(n, 0);

    pgw_fuse_session_close(s);
    fv_free(f);
}

/* --- readdir -------------------------------------------------------------- */

static void test_readdir_resumes_on_offset(void)
{
    fake_virtiofsd *f = fv_new(31, 65536);
    PGW_ASSERT(f != NULL);
    for (int i = 0; i < 5; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "e%d", i);
        PGW_ASSERT(fv_add(f, f->root, name, false) != NULL);
    }
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(s, PGW_FUSE_ROOT_ID, PGW_O_RDONLY, true, &fh,
                                  0),
                    PGW_OK);

    pgw_fuse_dirent ents[8];
    size_t total = 0;
    uint64_t off = 0;
    unsigned rounds = 0;
    for (;;) {
        size_t n = 0;
        PGW_ASSERT_EQ_I(pgw_fuse_readdir(s, PGW_FUSE_ROOT_ID, fh, off, ents, 2,
                                         &n, 0),
                        PGW_OK);
        if (n == 0)
            break;
        PGW_ASSERT(n <= 2);
        total += n;
        off = ents[n - 1u].off;
        PGW_ASSERT(++rounds <= 16u); /* the listing must terminate */
    }
    PGW_ASSERT_EQ_U(total, 5);
    PGW_ASSERT_EQ_U(rounds, 3); /* 2 + 2 + 1 */

    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_unique_is_monotonic_and_never_zero(void)
{
    fake_virtiofsd *f = fv_new(31, 65536);
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    uint64_t prev = pgw_fusep_next_unique(s);
    PGW_ASSERT(prev != 0);
    for (int i = 0; i < 1000; i++) {
        uint64_t u = pgw_fusep_next_unique(s);
        PGW_ASSERT(u > prev); /* strictly increasing */
        prev = u;
    }
    /* A wrapped counter must skip 0, which means "no request" on the wire. */
    s->next_unique = UINT64_MAX;
    PGW_ASSERT_EQ_U(pgw_fusep_next_unique(s), UINT64_MAX);
    PGW_ASSERT_EQ_U(pgw_fusep_next_unique(s), 1);

    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_open_refuses_missing_transport(void)
{
    PGW_ASSERT_EQ_PTR(pgw_fuse_session_open(NULL, NULL, 65536), NULL);
    /* A session with no reference table entry reports zero, not a crash. */
    PGW_ASSERT_EQ_U(pgw_fuse_ref_get(NULL, 1), 0);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(NULL), 0);
    PGW_ASSERT_EQ_I(pgw_fuse_forget_node(NULL, 1), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_fuse_interrupt(NULL, 1), PGW_EINVAL);
    pgw_fuse_session_close(NULL); /* idempotent */
}

PGW_TEST(test_unique_is_monotonic_and_never_zero);
PGW_TEST(test_open_refuses_missing_transport);
PGW_TEST(test_negotiation_uses_server_minor);
PGW_TEST(test_negotiation_caps_at_our_minor);
PGW_TEST(test_negotiation_refuses_other_major);
PGW_TEST(test_negotiation_refuses_too_old_minor);
PGW_TEST(test_negotiation_takes_max_write_from_server);
PGW_TEST(test_priority_lane_first);
PGW_TEST(test_forget_and_interrupt_are_hiprio);
PGW_TEST(test_full_session);
PGW_TEST(test_read_and_write_are_split);
PGW_TEST(test_read_stops_at_eof);
PGW_TEST(test_readdir_resumes_on_offset);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
