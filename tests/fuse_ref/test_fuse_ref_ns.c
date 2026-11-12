/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_ref_ns.c - namespace operations and error codes of the Passerelle
 * FUSE client, checked against the reference server and the real folder.
 *
 * Implements requirements:
 *   - VFS-03: MKDIR, RMDIR, UNLINK, RENAME, SYMLINK, READLINK, LINK, RELEASE
 *   - VFS-04: a LOOKUP takes a reference, FORGET gives it back, and the server
 *     really forgets the node (a later GETATTR reports ENOENT)
 *   - FS-05 / FS-08: the error codes the server reports travel back unchanged:
 *     ENOENT, EEXIST, ENOTDIR, EISDIR, ENOTEMPTY and ELOOP on a symlink loop
 */
#define _POSIX_C_SOURCE 200809L
#include "fuse_ref_harness.h"

#include "pgw/err.h"
#include "pgw/fusep.h"
#include "pgw/test.h"

#include <string.h>
#include <unistd.h>

/* --- namespace mutations --------------------------------------------------- */

static void test_mkdir_and_rmdir_reach_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_mkdir(c->session, PGW_FUSE_ROOT_ID, "newdir",
                                   0755, &e, 0),
                    PGW_OK);
    struct stat st;
    PGW_ASSERT(fuse_ref_disk_lstat(c, "newdir", &st));
    PGW_ASSERT(S_ISDIR(st.st_mode));
    fuse_ref_assert_attr_matches(c, e.nodeid, "newdir");

    /* A file inside makes RMDIR fail with ENOTEMPTY, exactly as on disk. */
    pgw_fuse_entry f;
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_create(c->session, e.nodeid, "inside.txt",
                                    PGW_O_CREAT | PGW_O_WRONLY, 0644, &f, &fh,
                                    0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, f.nodeid, fh, false, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_unlink(c->session, PGW_FUSE_ROOT_ID, "newdir",
                                    true, 0),
                    PGW_ENOTEMPTY);
    PGW_ASSERT_EQ_I(pgw_fuse_unlink(c->session, e.nodeid, "inside.txt", false,
                                    0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_unlink(c->session, PGW_FUSE_ROOT_ID, "newdir",
                                    true, 0),
                    PGW_OK);
    PGW_ASSERT_FALSE(fuse_ref_disk_lstat(c, "newdir", &st));
    fuse_ref_ctx_stop(c);
}

static void test_rename_and_unlink_reach_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_create(c->session, PGW_FUSE_ROOT_ID, "a.txt",
                                    PGW_O_CREAT | PGW_O_WRONLY, 0644, &e, &fh,
                                    0),
                    PGW_OK);
    size_t written = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_write(c->session, e.nodeid, fh, 0, "abc", 3,
                                   &written, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, e.nodeid, fh, false, 0),
                    PGW_OK);

    /* Rename inside the root, then into the sub-directory. */
    pgw_fuse_entry dir;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_DIR,
                                    &dir, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_rename(c->session, PGW_FUSE_ROOT_ID, "a.txt",
                                    PGW_FUSE_ROOT_ID, "b.txt", 0),
                    PGW_OK);
    struct stat st;
    PGW_ASSERT(fuse_ref_disk_lstat(c, "b.txt", &st));
    PGW_ASSERT_FALSE(fuse_ref_disk_lstat(c, "a.txt", &st));
    PGW_ASSERT_EQ_I(pgw_fuse_rename(c->session, PGW_FUSE_ROOT_ID, "b.txt",
                                    dir.nodeid, "b.txt", 0),
                    PGW_OK);
    PGW_ASSERT(fuse_ref_disk_lstat(c, FUSE_REF_DIR "/b.txt", &st));

    PGW_ASSERT_EQ_I(pgw_fuse_unlink(c->session, dir.nodeid, "b.txt", false, 0),
                    PGW_OK);
    PGW_ASSERT_FALSE(fuse_ref_disk_lstat(c, FUSE_REF_DIR "/b.txt", &st));
    fuse_ref_ctx_stop(c);
}

static void test_symlink_and_readlink_match_the_disk(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry link;
    PGW_ASSERT_EQ_I(pgw_fuse_symlink(c->session, PGW_FUSE_ROOT_ID, "made.lnk",
                                     FUSE_REF_SMALL_FILE, &link, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_ftype_from_linux_mode(link.attr.mode), PGW_FT_LNK);
    char target[64];
    PGW_ASSERT_EQ_I(pgw_fuse_readlink(c->session, link.nodeid, target,
                                      sizeof(target), 0),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, FUSE_REF_SMALL_FILE);

    /* The truth on disk agrees, and the pre-seeded link reads back too. */
    char on_disk[64];
    char path[FUSE_REF_PATH_MAX];
    fuse_ref_disk_path(c, "made.lnk", path, sizeof(path));
    ssize_t n = readlink(path, on_disk, sizeof(on_disk) - 1);
    PGW_ASSERT(n > 0);
    on_disk[n] = '\0';
    PGW_ASSERT_STR_EQ(on_disk, FUSE_REF_SMALL_FILE);

    pgw_fuse_entry seeded;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_LINK, &seeded, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_readlink(c->session, seeded.nodeid, target,
                                      sizeof(target), 0),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, FUSE_REF_BIG_FILE);
    fuse_ref_ctx_stop(c);
}

static void test_link_raises_the_real_link_count(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry small;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_SMALL_FILE, &small, 0),
                    PGW_OK);
    struct stat before;
    PGW_ASSERT(fuse_ref_disk_lstat(c, FUSE_REF_SMALL_FILE, &before));
    pgw_fuse_entry hard;
    PGW_ASSERT_EQ_I(pgw_fuse_link(c->session, PGW_FUSE_ROOT_ID, small.nodeid,
                                  "hard.lnk", &hard, 0),
                    PGW_OK);
    struct stat after;
    PGW_ASSERT(fuse_ref_disk_lstat(c, FUSE_REF_SMALL_FILE, &after));
    PGW_ASSERT_EQ_U((uint64_t)after.st_nlink, (uint64_t)before.st_nlink + 1u);
    /* A hard link is the same inode, and the disk says so too. */
    PGW_ASSERT_EQ_U(hard.attr.ino, small.attr.ino);
    PGW_ASSERT_EQ_U(hard.attr.ino, (uint64_t)after.st_ino);
    fuse_ref_ctx_stop(c);
}

/* --- error codes ----------------------------------------------------------- */

static void test_missing_names_report_enoent(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID, "absent",
                                    &e, 0),
                    PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_fuse_unlink(c->session, PGW_FUSE_ROOT_ID, "absent",
                                    false, 0),
                    PGW_ENOENT);
    PGW_ASSERT_EQ_I(pgw_fuse_unlink(c->session, PGW_FUSE_ROOT_ID, "absent",
                                    true, 0),
                    PGW_ENOENT);
    /* A node the server does not know is ENOENT too. */
    pgw_fuse_attr a;
    PGW_ASSERT_EQ_I(pgw_fuse_getattr(c->session, 999999, &a, 0), PGW_ENOENT);
    fuse_ref_ctx_stop(c);
}

static void test_conflicts_and_type_errors(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    uint64_t fh = 0;

    /* EEXIST on an exclusive create over an existing name. */
    PGW_ASSERT_EQ_I(pgw_fuse_create(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_SMALL_FILE,
                                    PGW_O_CREAT | PGW_O_EXCL | PGW_O_WRONLY,
                                    0644, &e, &fh, 0),
                    PGW_EEXIST);
    /* EEXIST on mkdir over an existing directory. */
    PGW_ASSERT_EQ_I(pgw_fuse_mkdir(c->session, PGW_FUSE_ROOT_ID, FUSE_REF_DIR,
                                   0755, &e, 0),
                    PGW_EEXIST);
    /* EISDIR: opening a directory as a file. */
    PGW_ASSERT_EQ_I(pgw_fuse_open(c->session, PGW_FUSE_ROOT_ID, PGW_O_RDONLY,
                                  false, &fh, 0),
                    PGW_EISDIR);
    /* ENOTDIR: opening a file as a directory, and looking up below a file. */
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_SMALL_FILE, &e, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_open(c->session, e.nodeid, PGW_O_RDONLY, true,
                                  &fh, 0),
                    PGW_ENOTDIR);
    pgw_fuse_entry child;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, e.nodeid, "anything", &child,
                                    0),
                    PGW_ENOTDIR);
    fuse_ref_ctx_stop(c);
}

static void test_symlink_loop_reports_eloop(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry loop;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_LOOP, &loop, 0),
                    PGW_OK);
    /* The loop is visible as a symlink, and its target is itself. */
    char target[32];
    PGW_ASSERT_EQ_I(pgw_fuse_readlink(c->session, loop.nodeid, target,
                                      sizeof(target), 0),
                    PGW_OK);
    PGW_ASSERT_STR_EQ(target, FUSE_REF_LOOP);
    /* Opening it follows the link and the server reports ELOOP. */
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(c->session, loop.nodeid, PGW_O_RDONLY, false,
                                  &fh, 0),
                    PGW_ELOOP);
    fuse_ref_ctx_stop(c);
}

/* --- references (VFS-04) --------------------------------------------------- */

static void test_release_and_forget_drop_the_node(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_SMALL_FILE, &e, 0),
                    PGW_OK);
    uint64_t nodeid = e.nodeid;
    PGW_ASSERT_EQ_U(pgw_fuse_ref_get(c->session, nodeid), 1);

    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(c->session, nodeid, PGW_O_RDONLY, false, &fh,
                                  0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, nodeid, fh, false, 0), PGW_OK);

    /* FORGET gives the reference back and the server forgets the node. */
    PGW_ASSERT_EQ_I(pgw_fuse_forget_node(c->session, nodeid), PGW_OK);
    PGW_ASSERT_EQ_U(pgw_fuse_ref_get(c->session, nodeid), 0);
    pgw_fuse_attr a;
    PGW_ASSERT_EQ_I(pgw_fuse_getattr(c->session, nodeid, &a, 0), PGW_ENOENT);

    /* Looking the name up again hands out a different node id. */
    pgw_fuse_entry again;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_SMALL_FILE, &again, 0),
                    PGW_OK);
    PGW_ASSERT(again.nodeid != nodeid);
    fuse_ref_ctx_stop(c);
}

/* --- requests with no reply, and synchronisation --------------------------- */

static void test_interrupt_and_fsync_keep_the_session_alive(void)
{
    fuse_ref_ctx *c = fuse_ref_ctx_start(NULL, NULL);
    PGW_ASSERT(c != NULL);
    /* INTERRUPT travels on the priority lane and is never answered: the
     * transport must not wait for a reply, and the session must stay usable. */
    PGW_ASSERT_EQ_I(pgw_fuse_interrupt(c->session, 12345), PGW_OK);

    pgw_fuse_entry e;
    PGW_ASSERT_EQ_I(pgw_fuse_lookup(c->session, PGW_FUSE_ROOT_ID,
                                    FUSE_REF_SMALL_FILE, &e, 0),
                    PGW_OK);
    uint64_t fh = 0;
    PGW_ASSERT_EQ_I(pgw_fuse_open(c->session, e.nodeid, PGW_O_RDONLY, false,
                                  &fh, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_fsync(c->session, e.nodeid, fh, false, 0), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_release(c->session, e.nodeid, fh, false, 0),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_fuse_interrupt(c->session, 999), PGW_OK);
    /* Still exactly what the disk says, after all of that. */
    fuse_ref_assert_attr_matches(c, e.nodeid, FUSE_REF_SMALL_FILE);
    fuse_ref_ctx_stop(c);
}

PGW_TEST(test_mkdir_and_rmdir_reach_the_disk);
PGW_TEST(test_rename_and_unlink_reach_the_disk);
PGW_TEST(test_symlink_and_readlink_match_the_disk);
PGW_TEST(test_link_raises_the_real_link_count);
PGW_TEST(test_missing_names_report_enoent);
PGW_TEST(test_conflicts_and_type_errors);
PGW_TEST(test_symlink_loop_reports_eloop);
PGW_TEST(test_release_and_forget_drop_the_node);
PGW_TEST(test_interrupt_and_fsync_keep_the_session_alive);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
