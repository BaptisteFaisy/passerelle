/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/test_refs.c - Node reference-count tests (VFS-04).
 *
 * Implements requirements:
 *   - VFS-04: LOOKUP/CREATE add one reference, FORGET gives back exactly the
 *     accumulated count, BATCH_FORGET is used when the server supports it,
 *     closing the session gives back whatever is left, and 100 000
 *     lookup/forget cycles leave neither a reference nor a leak behind.
 */
#include "fake_virtiofsd.h"

#include "fusep_session.h"

#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/log.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

/* --- the table on its own ------------------------------------------------- */

static void test_table_add_get_take(void)
{
    pgw_fuse_refs t;
    pgw_fusep_refs_init(&t);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_count(&t), 0);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_get(&t, 7), 0);

    PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, 7, 1), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, 7, 2), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, 3, 5), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, 9, 1), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_get(&t, 7), 3);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_get(&t, 3), 5);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_count(&t), 3);

    /* The array stays sorted by node id: 3, 7, 9. */
    PGW_ASSERT_EQ_U(pgw_fusep_refs_at(&t, 0)->nodeid, 3);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_at(&t, 1)->nodeid, 7);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_at(&t, 2)->nodeid, 9);

    uint64_t taken = 0;
    PGW_ASSERT_EQ_I(pgw_fusep_refs_take(&t, 7, &taken), PGW_OK);
    PGW_ASSERT_EQ_U(taken, 3);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_count(&t), 2);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_get(&t, 7), 0);

    /* Forgetting an unknown node is a no-op, not an error. */
    PGW_ASSERT_EQ_I(pgw_fusep_refs_take(&t, 12345, &taken), PGW_OK);
    PGW_ASSERT_EQ_U(taken, 0);
    PGW_ASSERT_EQ_U(pgw_fusep_refs_count(&t), 2);

    /* Node 0 is not a node. */
    PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, 0, 1), PGW_EINVAL);
    PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, 5, UINT64_MAX), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, 5, 1), PGW_EOVERFLOW);
    pgw_fusep_refs_free(&t);
}

static void test_table_grows_and_empties(void)
{
    pgw_fuse_refs t;
    pgw_fusep_refs_init(&t);
    for (uint64_t i = 1; i <= 1000; i++) {
        PGW_ASSERT_EQ_I(pgw_fusep_refs_add(&t, i, 1), PGW_OK);
    }
    PGW_ASSERT_EQ_U(pgw_fusep_refs_count(&t), 1000);
    for (uint64_t i = 1; i <= 1000; i++) {
        uint64_t n = 0;
        PGW_ASSERT_EQ_I(pgw_fusep_refs_take(&t, i, &n), PGW_OK);
        PGW_ASSERT_EQ_U(n, 1);
    }
    PGW_ASSERT_EQ_U(pgw_fusep_refs_count(&t), 0);
    pgw_fusep_refs_free(&t);
}

/* --- through a session and the in-memory server --------------------------- */

static fake_virtiofsd *make_server(uint32_t minor)
{
    fake_virtiofsd *f = fv_new(minor, 65536);
    if (f == NULL)
        return NULL;
    fv_node *file = fv_add(f, f->root, "file", false);
    if (file != NULL)
        PGW_ASSERT(fv_set_size(file, 8));
    PGW_ASSERT(fv_add(f, f->root, "dir", true) != NULL);
    return f;
}

static void test_lookup_accumulates_references(void)
{
    fake_virtiofsd *f = make_server(PGW_FUSE_MINOR);
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 0);

    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "file", &e, 0), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_get(s, e.nodeid), 1);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 1);

    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "file", &e, 0), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_get(s, e.nodeid), 2);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 1);

    /* FORGET carries the accumulated count, in one message. */
    unsigned before = f->forget_msgs;
    PGW_ASSERT_EQ_I(pgw_fuse_forget_node(s, e.nodeid), PGW_OK);
    PGW_ASSERT_EQ_U(f->forget_msgs, before + 1u);
    PGW_ASSERT_EQ_U(f->forgotten_refs, 2);
    PGW_ASSERT_EQ_U(f->forgotten_nodes, 1);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 0);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_get(s, e.nodeid), 0);

    /* Forgetting twice is harmless. */
    PGW_ASSERT_EQ_I(pgw_fuse_forget_node(s, e.nodeid), PGW_OK);
    PGW_ASSERT_EQ_U(f->forgotten_refs, 2);

    /* A negative lookup creates no reference. */
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "missing", &e, 0),
                    PGW_ENOENT);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 0);

    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_create_adds_reference(void)
{
    fake_virtiofsd *f = make_server(PGW_FUSE_MINOR);
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    pgw_fuse_entry e;
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_create(s, PGW_FUSE_ROOT_ID, "new", PGW_O_RDWR, 0644,
                                    &e, &fh, 0),
                    PGW_OK);
    PGW_ASSERT(e.nodeid != 0);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_get(s, e.nodeid), 1);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 1);
    PGW_ASSERT_EQ_I(pgw_fuse_release(s, e.nodeid, fh, false, 0), PGW_OK);

    pgw_fuse_session_close(s);
    PGW_ASSERT_EQ_U(f->forgotten_refs, 1);
    fv_free(f);
}

static void test_close_returns_everything(void)
{
    fake_virtiofsd *f = make_server(PGW_FUSE_MINOR);
    PGW_ASSERT(f != NULL);
    for (int i = 0; i < 20; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "n%d", i);
        PGW_ASSERT(fv_add(f, f->root, name, false) != NULL);
    }
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);

    pgw_fuse_entry e;
    for (int i = 0; i < 20; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "n%d", i);
        PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, name, &e, 0),
                        PGW_OK);
        PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, name, &e, 0),
                        PGW_OK);
    }
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 20);

    pgw_fuse_session_close(s);
    PGW_ASSERT_EQ_U(f->forgotten_refs, 40); /* 20 nodes x 2 references */
    fv_free(f);
}

static void test_batch_forget_is_used_on_modern_servers(void)
{
    fake_virtiofsd *f = make_server(31);
    PGW_ASSERT(f != NULL);
    for (int i = 0; i < 40; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "b%d", i);
        PGW_ASSERT(fv_add(f, f->root, name, false) != NULL);
    }
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    pgw_fuse_entry e;
    for (int i = 0; i < 40; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "b%d", i);
        PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, name, &e, 0),
                        PGW_OK);
    }
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 40);
    pgw_fuse_session_close(s);
    PGW_ASSERT_EQ_U(f->forgotten_refs, 40);
    /* 32 nodes per message: two BATCH_FORGET messages for 40 nodes. */
    PGW_ASSERT_EQ_U(f->forget_msgs, 2);
    PGW_ASSERT_EQ_U(f->forget_batches, 2);
    PGW_ASSERT_EQ_U(f->forgotten_nodes, 40);
    fv_free(f);
}

static void test_old_server_gets_plain_forgets(void)
{
    fake_virtiofsd *f = make_server(12); /* below 7.16: no BATCH_FORGET */
    PGW_ASSERT(f != NULL);
    for (int i = 0; i < 5; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "o%d", i);
        PGW_ASSERT(fv_add(f, f->root, name, false) != NULL);
    }
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    pgw_fuse_entry e;
    for (int i = 0; i < 5; i++) {
        char name[16];
        (void)snprintf(name, sizeof(name), "o%d", i);
        PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, name, &e, 0),
                        PGW_OK);
    }
    pgw_fuse_session_close(s);
    PGW_ASSERT_EQ_U(f->forgotten_refs, 5);
    PGW_ASSERT_EQ_U(f->forget_msgs, 5); /* one FORGET per node */
    PGW_ASSERT_EQ_U(f->forget_batches, 0);
    fv_free(f);
}

static void test_hundred_thousand_cycles(void)
{
    fake_virtiofsd *f = make_server(PGW_FUSE_MINOR);
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    pgw_log_set_level(PGW_LOG_ERROR); /* 100k round trips, keep the output quiet */

    pgw_fuse_entry e;
    for (int i = 0; i < 100000; i++) {
        PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "file", &e, 0),
                        PGW_OK);
        PGW_ASSERT_EQ_I(pgw_fuse_forget_node(s, e.nodeid), PGW_OK);
        PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 0);
    }
    PGW_ASSERT_EQ_U(f->forgotten_refs, 100000);
    PGW_ASSERT_EQ_U(f->forget_msgs, 100000);
    pgw_fuse_session_close(s);
    fv_free(f);
}

PGW_TEST(test_table_add_get_take);
PGW_TEST(test_table_grows_and_empties);
PGW_TEST(test_lookup_accumulates_references);
PGW_TEST(test_create_adds_reference);
PGW_TEST(test_close_returns_everything);
PGW_TEST(test_batch_forget_is_used_on_modern_servers);
PGW_TEST(test_old_server_gets_plain_forgets);
PGW_TEST(test_hundred_thousand_cycles);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
