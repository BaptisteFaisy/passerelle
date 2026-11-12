/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_cas_exotic.c - the exotic names directed case of spec 7.4 (gap 67).
 *
 * A name is a byte sequence, not text: the layer must carry it from the share
 * to the client and back without normalising, re-encoding, quoting or
 * "cleaning" it. Every case here creates a name through the layer under test
 * and then asks the truth (plain POSIX calls on the shared directory) whether
 * exactly those bytes exist - and the listing must report exactly those bytes
 * too, which is where a normalising layer is caught.
 *
 * The table covers UTF-8 two, three and four byte sequences, a combining mark
 * (NFC and NFD are different byte sequences and must stay different files),
 * spaces, a tab, a newline, quotes, a backslash, a leading dash, a trailing
 * dot, two dots inside a name, a 255-byte component, and the two names that
 * are not names: "." and "..".
 *
 * Covers FS-15, FS-02, FS-03, P9-02 (gap 67 of docs/gap-analysis.md).
 */
#define _POSIX_C_SOURCE 200809L
#include "cas.h"
#include "cas_9p.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The names are written as byte escapes on purpose: two entries that look
 * alike on a screen (NFC and NFD) must not be confusable in the source. */
typedef struct exotic {
    const char *name;
    const char *what;
} exotic;

static const exotic g_exotic[] = {
    { "plain.txt", "an ordinary name" },
    { "with spaces.txt", "spaces" },
    { "tab\tinside", "a tab" },
    { "newline\ninside", "a newline" },
    { "single'quote", "a single quote" },
    { "double\"quote", "a double quote" },
    { "back\\slash", "a backslash" },
    { "-leading-dash", "a leading dash" },
    { "trailing-dot.", "a trailing dot" },
    { "dots..inside", "two dots inside the name" },
    { "\xc3\xa9\xc3\xa0\xc3\xbc-two-bytes", "UTF-8 two-byte sequences" },
    { "\xe4\xb8\xad\xe6\x96\x87-three-bytes", "UTF-8 three-byte sequences" },
    { "\xf0\x9f\xa6\x80-four-bytes", "a UTF-8 four-byte emoji" },
    { "\xc3\xa9-composed", "NFC: one precomposed code point" },
    { "e\xcc\x81-decomposed", "NFD: a base letter and a combining mark" },
    { "mix \xf0\x9f\xa6\x80 \xc3\xa9 back\\slash", "all of it at once" },
};
#define N_EXOTIC (sizeof(g_exotic) / sizeof(g_exotic[0]))

/* Create `name` through the layer with a known pattern inside, then require
 * that the truth holds exactly those bytes under exactly that name. */
static void create_and_verify(pgw_rmfs *fs, const pgw_fs_cred *cred,
                              const char *root, const exotic *e, uint32_t seed)
{
    pgw_rmfs_fd *fd = NULL;
    pgw_err err = pgw_rmfs_open(fs, cred, e->name,
                                PGW_O_RDWR | PGW_O_CREAT | PGW_O_TRUNC, 0644,
                                &fd);
    CAS_ASSERT_MSG(err == PGW_OK, "create of %s failed: %s", e->what,
                   pgw_err_name(err));
    uint8_t buf[64];
    pgw_test_pattern(buf, sizeof(buf), seed, 0);
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_write(fs, fd, 0, buf, sizeof(buf), &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);

    /* The truth holds the name, byte for byte, with the same content. */
    CAS_ASSERT_MSG(cas_truth_exists(root, e->name),
                   "the truth does not hold a file named %s", e->what);
    uint8_t back[64];
    size_t len = 0;
    CAS_ASSERT_MSG(cas_truth_read(root, e->name, back, sizeof(back), &len),
                   "the truth cannot read back %s", e->what);
    CAS_ASSERT_MSG(len == sizeof(buf) && memcmp(back, buf, len) == 0,
                   "the content of %s did not survive", e->what);

    /* And the layer reads back what it wrote, through its own path. */
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, cred, e->name, PGW_O_RDONLY, 0, &fd),
                    PGW_OK);
    memset(back, 0, sizeof(back));
    PGW_ASSERT_EQ_I(pgw_rmfs_read(fs, fd, 0, back, sizeof(back), &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, fd), PGW_OK);
    PGW_ASSERT_MEM_EQ(back, buf, sizeof(buf));
}

/* The listing of "." must contain `name` with exactly those bytes. */
static void expect_listed(pgw_rmfs *fs, const pgw_fs_cred *cred,
                          const char *name, const char *what)
{
    pgw_rmfs_fd *dir = NULL;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(fs, cred, ".",
                                  PGW_O_RDONLY | PGW_O_DIRECTORY, 0, &dir),
                    PGW_OK);
    pgw_fs_dirent ents[64];
    size_t got = 0;
    uint64_t off = 0;
    uint64_t next = 0;
    bool found = false;
    const size_t want_len = strlen(name) + 1;
    while (!found) {
        PGW_ASSERT_EQ_I(pgw_rmfs_readdir(fs, dir, off, ents, 64, &got, &next),
                        PGW_OK);
        if (got == 0)
            break;
        for (size_t i = 0; i < got && !found; i++)
            found = memcmp(ents[i].name, name, want_len) == 0;
        if (next == 0)
            break;
        off = next;
    }
    PGW_ASSERT_EQ_I(pgw_rmfs_close(fs, dir), PGW_OK);
    CAS_ASSERT_MSG(found, "the listing does not report %s byte for byte", what);
}

/* FS-15 (gap 67): every name of the table survives the whole chain and is
 * reported back with the same bytes. */
static void test_exotic_names_survive_the_layer(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-exotic", NULL, NULL));
    for (size_t i = 0; i < N_EXOTIC; i++)
        create_and_verify(m.fs, &m.cred, m.root, &g_exotic[i],
                          (uint32_t)(i + 1));
    for (size_t i = 0; i < N_EXOTIC; i++)
        expect_listed(m.fs, &m.cred, g_exotic[i].name, g_exotic[i].what);
    PGW_ASSERT_EQ_U(cas_truth_count(m.root, "."), N_EXOTIC);
    cas_mount_close(&m);
}
PGW_TEST(test_exotic_names_survive_the_layer);

/* FS-15 (gap 67): NFC and NFD are different byte sequences and must stay two
 * different files - a layer that normalises silently merges them. */
static void test_exotic_names_nfc_and_nfd_stay_distinct(void)
{
    const char *nfc = "\xc3\xa9-composed";
    const char *nfd = "e\xcc\x81-decomposed";
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-nfc", NULL, NULL));

    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(m.fs, &m.cred, nfc,
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(m.fs, fd, 0, "NFC", 3, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_open(m.fs, &m.cred, nfd,
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(m.fs, fd, 0, "NFD-longer", 10, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);

    /* Two entries on the truth side, two distinct inodes, each with its own
     * content: nothing was normalised on the way. */
    PGW_ASSERT_EQ_U(cas_truth_count(m.root, "."), 2);
    pgw_fs_attr a;
    pgw_fs_attr b;
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, nfc, &a), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_stat(m.fs, &m.cred, nfd, &b), PGW_OK);
    CAS_ASSERT_MSG(a.ino != b.ino, "NFC and NFD are the same file (ino %llu)",
                   (unsigned long long)a.ino);
    PGW_ASSERT_EQ_U(a.size, 3);
    PGW_ASSERT_EQ_U(b.size, 10);
    expect_listed(m.fs, &m.cred, nfc, "the NFC name");
    expect_listed(m.fs, &m.cred, nfd, "the NFD name");
    cas_mount_close(&m);
}
PGW_TEST(test_exotic_names_nfc_and_nfd_stay_distinct);

/* FS-15 (gap 67): a 255-byte single component is a name like any other. */
static void test_exotic_name_of_255_bytes(void)
{
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-255", NULL, NULL));
    char name[PGW_NAME_MAX + 1];
    memset(name, 'x', PGW_NAME_MAX);
    name[PGW_NAME_MAX] = '\0';

    pgw_rmfs_fd *fd = NULL;
    size_t n = 0;
    PGW_ASSERT_EQ_I(pgw_rmfs_open(m.fs, &m.cred, name,
                                  PGW_O_RDWR | PGW_O_CREAT, 0644, &fd),
                    PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_write(m.fs, fd, 0, "long name", 9, &n), PGW_OK);
    PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    PGW_ASSERT(cas_truth_exists(m.root, name));
    PGW_ASSERT_EQ_U(cas_truth_size(m.root, name), 9);
    expect_listed(m.fs, &m.cred, name, "a 255-byte name");
    cas_mount_close(&m);
}
PGW_TEST(test_exotic_name_of_255_bytes);

/* FS-15, FS-02 (gap 67): "." and ".." are not names: the layer must refuse
 * them exactly as the kernel does, and must not create anything. */
static void test_exotic_names_dot_and_dotdot_refused(void)
{
    static const char *const bad[] = { ".", ".." };
    cas_mount m;
    PGW_ASSERT(cas_mount_open(&m, "pgwcas-dots", NULL, NULL));
    const size_t before = cas_truth_count(m.root, ".");

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* mkdir: the kernel answers EEXIST for both, since they exist. */
        const pgw_err got_mkdir = pgw_rmfs_mkdir(m.fs, &m.cred, bad[i], 0755);
        errno = 0;
        char abs[CAS_PATH_MAX];
        cas_mount_abs(&m, bad[i], abs, sizeof(abs));
        const int truth_mkdir = (mkdir(abs, 0755) == 0) ? 0 : errno;
        CAS_ASSERT_MSG(got_mkdir == pgw_code_from_host(truth_mkdir),
                       "mkdir(\"%s\"): the layer answered %s, the kernel errno "
                       "%d",
                       bad[i], pgw_err_name(got_mkdir), truth_mkdir);

        /* create: the kernel answers EISDIR, and so must the layer. */
        pgw_rmfs_fd *fd = NULL;
        const pgw_err got_open = pgw_rmfs_open(m.fs, &m.cred, bad[i],
                                               PGW_O_RDWR | PGW_O_CREAT, 0644,
                                               &fd);
        errno = 0;
        const int truth_open = (open(abs, O_WRONLY | O_CREAT, 0644) < 0) ? errno
                                                                        : 0;
        CAS_ASSERT_MSG(got_open == pgw_code_from_host(truth_open),
                       "create(\"%s\"): the layer answered %s, the kernel errno "
                       "%d",
                       bad[i], pgw_err_name(got_open), truth_open);
        if (fd != NULL)
            PGW_ASSERT_EQ_I(pgw_rmfs_close(m.fs, fd), PGW_OK);
    }
    PGW_ASSERT_EQ_U(cas_truth_count(m.root, "."), before);

    /* The backend's own guard still refuses a ".." *component* - refusing
     * every name that merely contains two dots would be the bug this case
     * exists for, and accepting a ".." component would escape the share. */
    char joined[CAS_PATH_MAX];
    PGW_ASSERT_FALSE(pgw_posix_join(m.be, "a/../b", joined, sizeof(joined)));
    PGW_ASSERT_FALSE(pgw_posix_join(m.be, "..", joined, sizeof(joined)));
    PGW_ASSERT_FALSE(pgw_posix_join(m.be, "../escape", joined, sizeof(joined)));
    PGW_ASSERT(pgw_posix_join(m.be, "dots..inside", joined, sizeof(joined)));
    cas_mount_close(&m);
}
PGW_TEST(test_exotic_names_dot_and_dotdot_refused);

/* FS-15, P9-02 (gap 67): the same names across the wire - the client writes
 * them, the served directory holds exactly those bytes. */
static void test_exotic_names_through_9p(void)
{
    cas_9p c;
    PGW_ASSERT(cas_9p_open(&c, "pgwcas-exotic9p", NULL));
    for (size_t i = 0; i < N_EXOTIC; i++)
        create_and_verify(c.fs, &c.cred, c.root, &g_exotic[i],
                          (uint32_t)(i + 100));
    for (size_t i = 0; i < N_EXOTIC; i++)
        expect_listed(c.fs, &c.cred, g_exotic[i].name, g_exotic[i].what);
    PGW_ASSERT_EQ_U(cas_truth_count(c.root, "."), N_EXOTIC);
    cas_9p_close(&c);
}
PGW_TEST(test_exotic_names_through_9p);
