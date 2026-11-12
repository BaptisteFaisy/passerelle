/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_ref_robust.c - the safety valve of the client, against a reference
 * server that deliberately answers badly.
 *
 * Implements requirements:
 *   - VFS-05 / ROB-06: a reply carrying an unknown FUSE error number, an
 *     inconsistent declared length, a truncated header, a `unique` that does
 *     not match the request, or a positive error field is refused. The client
 *     reports PGW_EIO, keeps its state consistent and stays usable.
 *   - P9-09 (applied to virtiofs): nothing the server sends is trusted.
 *
 * The faults are injected by the reference server itself (--fault), one per
 * request after INIT, in a fixed order: the test is deterministic and the
 * server keeps serving the real directory once the list is exhausted.
 */
#define _POSIX_C_SOURCE 200809L
#include "fuse_ref_harness.h"

#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include <string.h>

/* One malformed answer per request, in this order. */
#define FUSE_REF_FAULT_LIST                                                              \
    "unknown-errno,bad-length,short-header,bad-unique,positive-error"
#define FUSE_REF_FAULT_COUNT 5

static void burn_faults(fuse_ref_ctx *c, pgw_fuse_entry *out)
{
    for (int i = 0; i < FUSE_REF_FAULT_COUNT; i++) {
        pgw_err rc = pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_SMALL_FILE,
                                     out, 0);
        /* Every malformed reply is refused as an I/O error, and none of them
         * may leave a reference behind. */
        PGW_ASSERT_EQ_I(rc, PGW_EIO);
        PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(c->session), 0);
    }
}

static void test_malformed_replies_are_refused(void)
{
    char faults[] = FUSE_REF_FAULT_LIST;
    fuse_ref_ctx *c = fuse_ref_ctx_start(faults, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    memset(&e, 0, sizeof(e));
    burn_faults(c, &e);

    /* The session is still alive: the same lookup now succeeds, and what it
     * reports is still exactly what the disk says. */
    PGW_ASSERT_EQ_I(
            pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_SMALL_FILE, &e, 0),
            PGW_OK);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(c->session), 1);
    fuse_ref_assert_attr_matches(c, e.nodeid, FUSE_REF_SMALL_FILE);
    PGW_ASSERT_EQ_I(pgw_fuse_forget_node(c->session, e.nodeid), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(c->session), 0);
    fuse_ref_ctx_stop(c);
}

static void test_data_path_still_works_after_bad_replies(void)
{
    char faults[] = FUSE_REF_FAULT_LIST;
    fuse_ref_ctx *c = fuse_ref_ctx_start(faults, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    memset(&e, 0, sizeof(e));
    burn_faults(c, &e);

    static uint8_t payload[8192];
    pgw_test_pattern(payload, sizeof(payload), 0xBEEFu, 7);
    pgw_fuse_entry created;
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_create(c->session, PGW_FUSE_ROOT_ID, "after-faults.bin",
                                    PGW_O_CREAT | PGW_O_RDWR, 0644, &created, &fh, 0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_write(c->session, created.nodeid, fh, 0, payload,
                                   sizeof(payload), &written, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, sizeof(payload));

    uint8_t back[8192];
    size_t got = 0;
    PGW_ASSERT(fuse_ref_disk_read(c, "after-faults.bin", back, sizeof(back), &got));
    PGW_ASSERT_EQ_U(got, sizeof(payload));
    PGW_ASSERT_MEM_EQ(back, payload, sizeof(payload));

    uint8_t via_client[8192];
    size_t read_back = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_read(c->session, created.nodeid, fh, 0, via_client,
                                  sizeof(via_client), &read_back, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(read_back, sizeof(payload));
    PGW_ASSERT_MEM_EQ(via_client, payload, sizeof(payload));
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, created.nodeid, fh, false, 0), PGW_OK);
    fuse_ref_ctx_stop(c);
}

PGW_TEST(test_malformed_replies_are_refused);
PGW_TEST(test_data_path_still_works_after_bad_replies);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
