/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/test_session_errors.c - What the session does with a reply it
 * refuses to trust.
 *
 * Implements requirements:
 *   - ROB-06 / P9-09: a reply carrying the wrong `unique`, a reply whose
 *     declared length is larger than the bytes received, and an empty reply to a
 *     request that needs one are all refused, and the session keeps working
 *   - VFS-03: a server-side error travels back as the matching pgw_err
 */
#include "fake_virtiofsd.h"

#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include <stdio.h>
#include <string.h>

/* A small tree: one regular file of five bytes, a directory and a plain node. */
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
    return f;
}

/* --- strict reply validation --------------------------------------------- */

static void test_bad_replies_are_refused(void)
{
    fake_virtiofsd *f = make_tree();
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    pgw_fuse_entry e;

    f->corrupt_unique = true;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "hello.txt", &e, 0),
                    PGW_EIO);

    f->truncate_reply = true;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "hello.txt", &e, 0),
                    PGW_EIO);

    f->drop_reply = true;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "hello.txt", &e, 0),
                    PGW_EIO);

    /* The session still works afterwards. */
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "hello.txt", &e, 0),
                    PGW_OK);

    pgw_fuse_session_close(s);
    fv_free(f);
}

static void test_server_error_is_translated(void)
{
    fake_virtiofsd *f = make_tree();
    PGW_ASSERT(f != NULL);
    pgw_fuse_session *s = pgw_fuse_session_open(fv_rpc, f, 65536);
    PGW_ASSERT(s != NULL);
    pgw_fuse_entry e;

    f->force_error = -(int32_t)pgw_linux_errno(PGW_EACCES);
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "hello.txt", &e, 0),
                    PGW_EACCES);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_count_nodes(s), 0);

    f->force_error = -(int32_t)pgw_linux_errno(PGW_ENOSPC);
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(s, PGW_FUSE_ROOT_ID, "hello.txt", &e, 0),
                    PGW_ENOSPC);

    /* A server-side EINVAL on a non-symlink stays an error, not a success. */
    char target[8];
    PGW_ASSERT_EQ_I(pgw_fuse_readlink(s, PGW_FUSE_ROOT_ID, target,
                                      sizeof(target), 0),
                    PGW_EINVAL); /* root is not a symlink */

    pgw_fuse_session_close(s);
    fv_free(f);
}

PGW_TEST(test_bad_replies_are_refused);
PGW_TEST(test_server_error_is_translated);
