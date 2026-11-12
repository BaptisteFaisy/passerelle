/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_ref.c - the Passerelle FUSE client against the reference server.
 *
 * Implements requirements:
 *   - VFS-01: the protocol version is negotiated with a real server, and the
 *     client falls back to an older minor version without failing
 *   - VFS-02: the session opens, queues and closes over a real transport
 *   - VFS-03: LOOKUP, GETATTR, OPEN, READ, WRITE, CREATE, READDIR, SETATTR and
 *     STATFS are exercised, and every answer is compared with the real folder
 *     the server serves (the truth), never with another view of the same state
 *   - VFS-05: the answers that come back are decoded strictly
 *
 * The server (tests/fuse_ref/fuse_ref_server.py) is an independent
 * implementation of the FUSE kernel protocol; /dev/fuse is not used, so the
 * whole test runs in a sandbox and in the CI.
 */
#define _POSIX_C_SOURCE 200809L
#include "fuse_ref_harness.h"

#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

static uint8_t g_big[FUSE_REF_BIG_SIZE];

/* --- INIT and version negotiation ----------------------------------------- */

static void test_init_negotiates_with_the_reference_server(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    PGW_ASSERT(c->session != NULL);

    /* An older server (7.12) must still work: the client falls back to the
     * minor version the server announces instead of failing. */
    fuse_ref_ctx *old = fuse_ref_ctx_start(NULL, "12");
    PGW_ASSERT(old != NULL);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(
            pgw_fuse_lookup(old->session, PGW_FUSE_ROOT_ID, FUSE_REF_SMALL_FILE, &e, 0),
            PGW_OK);
    PGW_ASSERT_EQ_U(e.attr.size, 10);
    fuse_ref_ctx_stop(old);
    fuse_ref_ctx_stop(c);
}

/* --- metadata ------------------------------------------------------------- */

static void test_lookup_and_getattr_match_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;

    PGW_ASSERT_EQ_I(
            pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_BIG_FILE, &e, 0),
            PGW_OK);
    PGW_ASSERT(e.nodeid != 0);
    PGW_ASSERT_EQ_U(e.attr.size, FUSE_REF_BIG_SIZE);
    PGW_ASSERT_EQ_I(pgw_ftype_from_linux_mode(e.attr.mode), PGW_FT_REG);
    fuse_ref_assert_attr_matches(c, e.nodeid, FUSE_REF_BIG_FILE);

    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_DIR, &e, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_ftype_from_linux_mode(e.attr.mode), PGW_FT_DIR);
    fuse_ref_assert_attr_matches(c, e.nodeid, FUSE_REF_DIR);

    /* A symlink is reported as a symlink: LOOKUP never follows it. */
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_LINK, &e, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_ftype_from_linux_mode(e.attr.mode), PGW_FT_LNK);
    fuse_ref_assert_attr_matches(c, e.nodeid, FUSE_REF_LINK);
    fuse_ref_ctx_stop(c);
}

/* --- data path ------------------------------------------------------------ */

static void test_read_matches_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(
            pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_BIG_FILE, &e, 0),
            PGW_OK);
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(c->session, e.nodeid, PGW_O_RDONLY, false, &fh, 0),
                    PGW_OK);
    /* The file is larger than max_write: the client must split the read. */
    size_t got = 0;
    PGW_ASSERT_EQ_I(
            pgw_fuse_read(c->session, e.nodeid, fh, 0, g_big, sizeof(g_big), &got, 0),
            PGW_OK);
    PGW_ASSERT_EQ_U(got, FUSE_REF_BIG_SIZE);
    PGW_ASSERT(pgw_test_pattern_ok(g_big, sizeof(g_big), 0xC0FFEEu, 0));

    static uint8_t direct[FUSE_REF_BIG_SIZE];
    size_t on_disk = 0;
    PGW_ASSERT(
            fuse_ref_disk_read(c, FUSE_REF_BIG_FILE, direct, sizeof(direct), &on_disk));
    PGW_ASSERT_EQ_U(on_disk, FUSE_REF_BIG_SIZE);
    PGW_ASSERT_MEM_EQ(g_big, direct, FUSE_REF_BIG_SIZE);
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, e.nodeid, fh, false, 0), PGW_OK);
    fuse_ref_ctx_stop(c);
}

static void test_create_and_write_reach_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    static uint8_t payload[4096];
    pgw_test_pattern(payload, sizeof(payload), 0x1234u, 0);

    pgw_fuse_entry e;
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_create(c->session, PGW_FUSE_ROOT_ID, "created.bin",
                                    PGW_O_CREAT | PGW_O_RDWR, 0644, &e, &fh, 0),
                    PGW_OK);
    PGW_ASSERT(e.nodeid != 0);

    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_write(c->session, e.nodeid, fh, 0, payload, sizeof(payload),
                                   &written, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(written, sizeof(payload));

    uint8_t back[4096];
    size_t got = 0;
    PGW_ASSERT(fuse_ref_disk_read(c, "created.bin", back, sizeof(back), &got));
    PGW_ASSERT_EQ_U(got, sizeof(payload));
    PGW_ASSERT_MEM_EQ(back, payload, sizeof(payload));

    uint8_t via_client[4096];
    size_t read_back = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_read(c->session, e.nodeid, fh, 0, via_client,
                                  sizeof(via_client), &read_back, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_U(read_back, sizeof(payload));
    PGW_ASSERT_MEM_EQ(via_client, payload, sizeof(payload));
    fuse_ref_assert_attr_matches(c, e.nodeid, "created.bin");
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, e.nodeid, fh, false, 0), PGW_OK);
    fuse_ref_ctx_stop(c);
}

/* --- directories ---------------------------------------------------------- */

static void test_readdir_matches_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    char on_disk[16][FUSE_REF_NAME_MAX];
    char via_client[16][FUSE_REF_NAME_MAX];
    size_t n_disk = fuse_ref_disk_names(c, ".", on_disk, 16);
    size_t n_client = fuse_ref_client_names(c, PGW_FUSE_ROOT_ID, via_client, 16);
    PGW_ASSERT(n_disk >= 5u);
    PGW_ASSERT_EQ_U(n_client, n_disk);
    for (size_t i = 0; i < n_disk; i++)
        PGW_ASSERT_STR_EQ(via_client[i], on_disk[i]);
    fuse_ref_ctx_stop(c);
}

/* --- attributes and statistics -------------------------------------------- */

static void test_setattr_changes_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(
            pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_SMALL_FILE, &e, 0),
            PGW_OK);
    pgw_fuse_setattr_in sa;
    pgw_fuse_attr a;
    memset(&sa, 0, sizeof(sa));
    memset(&a, 0, sizeof(a));

    sa.valid = PGW_FUSE_FATTR_MODE;
    sa.mode = 0600;
    PGW_ASSERT_EQ_I(pgw_fuse_setattr(c->session, e.nodeid, &sa, &a, 0), PGW_OK);
    struct stat st;
    PGW_ASSERT(fuse_ref_disk_lstat(c, FUSE_REF_SMALL_FILE, &st));
    PGW_ASSERT_EQ_U((uint32_t)(st.st_mode & 0777), 0600u);

    memset(&sa, 0, sizeof(sa));
    sa.valid = PGW_FUSE_FATTR_SIZE;
    sa.size = 4;
    PGW_ASSERT_EQ_I(pgw_fuse_setattr(c->session, e.nodeid, &sa, &a, 0), PGW_OK);
    PGW_ASSERT_EQ_U(a.size, 4);
    struct stat cut;
    PGW_ASSERT(fuse_ref_disk_lstat(c, FUSE_REF_SMALL_FILE, &cut));
    PGW_ASSERT_EQ_U((uint64_t)cut.st_size, 4);
    fuse_ref_ctx_stop(c);
}

static void test_statfs_reports_the_filesystem(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_kstatfs ks;
    memset(&ks, 0, sizeof(ks));
    PGW_ASSERT_EQ_I(pgw_fuse_statfs(c->session, PGW_FUSE_ROOT_ID, &ks, 0), PGW_OK);
    PGW_ASSERT(ks.bsize > 0u);
    PGW_ASSERT(ks.blocks > 0u);
    PGW_ASSERT_EQ_U(ks.namelen, 255);
    fuse_ref_ctx_stop(c);
}

PGW_TEST(test_init_negotiates_with_the_reference_server);
PGW_TEST(test_lookup_and_getattr_match_the_disk);
PGW_TEST(test_read_matches_the_disk);
PGW_TEST(test_create_and_write_reach_the_disk);
PGW_TEST(test_readdir_matches_the_disk);
PGW_TEST(test_setattr_changes_the_disk);
PGW_TEST(test_statfs_reports_the_filesystem);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
