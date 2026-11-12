/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/corpus/test_corpus.c - replay every stored divergence (gap 62).
 *
 * Each file of tests/corpus/ is a divergence the differential bench of
 * tests/e2e found and reduced to its shortest prefix (diff_reduce). This bench
 * regenerates the operations from the stored seed, checks that they are still
 * the ones that were stored (ops_digest), and replays them against the truth
 * side (tests/posix_backend). A stored case that diverges again fails here, so
 * a bug that was fixed once cannot come back silently.
 *
 * The bench is strictly read-only: it opens corpus files for reading and the
 * replay itself runs in a temporary directory created by the diff runner, so
 * nothing is ever written to the repository tree.
 *
 * An empty corpus is a valid state - it means no divergence has been captured
 * yet - and is reported as such rather than as a failure.
 */
#define _POSIX_C_SOURCE 200809L
#include "corpus.h"
#include "diff.h"

#include "pgw/test.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PGW_CORPUS_DIR
#define PGW_CORPUS_DIR "."
#endif

#define CORPUS_MAX_FILES 256u

static char *g_names[CORPUS_MAX_FILES];
static size_t g_count;

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Collect the *.pgwcase files of the corpus, sorted so that a run is
 * reproducible whatever order the filesystem hands them back in. */
static size_t corpus_list(void)
{
    DIR *d = opendir(PGW_CORPUS_DIR);
    if (d == NULL)
        return 0;
    struct dirent *e;
    size_t ext_len = strlen(PGW_CORPUS_EXT);
    while ((e = readdir(d)) != NULL && g_count < CORPUS_MAX_FILES) {
        size_t n = strlen(e->d_name);
        if (n <= ext_len || strcmp(e->d_name + n - ext_len, PGW_CORPUS_EXT) != 0)
            continue;
        char path[PGW_CORPUS_PATH];
        snprintf(path, sizeof(path), "%s/%s", PGW_CORPUS_DIR, e->d_name);
        g_names[g_count] = strdup(path);
        if (g_names[g_count] != NULL)
            g_count++;
    }
    closedir(d);
    qsort(g_names, g_count, sizeof(g_names[0]), cmp_str);
    return g_count;
}

/* Replay one stored case. Returns 0 when the divergence has not come back. */
static int replay_one(const char *path)
{
    pgw_corpus_case c;
    if (pgw_corpus_load(path, &c) != 0) {
        pgw_fail(__FILE__, __LINE__, "%s: not a readable corpus case", path);
        return -1;
    }
    if (c.n_ops > (size_t)PGW_DIFF_MAX_OPS) {
        pgw_fail(__FILE__, __LINE__, "%s: %zu operations is not plausible",
                 path, c.n_ops);
        return -1;
    }
    diff_op *ops = calloc(c.n_ops, sizeof(*ops));
    if (ops == NULL) {
        pgw_fail(__FILE__, __LINE__, "%s: cannot allocate %zu operations", path,
                 c.n_ops);
        return -1;
    }
    diff_gen(ops, c.n_ops, c.suite_seed);
    uint64_t digest = pgw_corpus_digest(ops, c.n_ops);
    if (digest != c.ops_digest) {
        free(ops);
        pgw_fail(__FILE__, __LINE__,
                 "%s: the stored operations no longer match the generator "
                 "(digest %016llX, stored %016llX) - regenerate the case",
                 path, (unsigned long long)digest,
                 (unsigned long long)c.ops_digest);
        return -1;
    }
    char err[PGW_DIFF_MSG];
    long bad = diff_run(ops, c.n_ops, c.suite_seed, err, sizeof(err));
    free(ops);
    if (bad >= 0) {
        pgw_fail(__FILE__, __LINE__,
                 "%s: the stored divergence is back at op %ld (%s)", path, bad,
                 err);
        return -1;
    }
    fprintf(stdout, "# corpus: %s replayed clean (%zu ops, seed %u)\n", path,
            c.n_ops, c.suite_seed);
    return 0;
}

static void test_corpus_replays_every_stored_divergence(void)
{
    corpus_list();
    if (g_count == 0) {
        fprintf(stdout,
                "# corpus: %s holds no case yet - a reduced divergence is\n"
                "# stored by tools/corpus-save.sh (PGW_CORPUS_SAVE=<dir>)\n",
                PGW_CORPUS_DIR);
        PGW_ASSERT(1);
        return;
    }
    for (size_t i = 0; i < g_count; i++)
        replay_one(g_names[i]);
    for (size_t i = 0; i < g_count; i++)
        free(g_names[i]);
    g_count = 0;
}
PGW_TEST(test_corpus_replays_every_stored_divergence);

/* The corpus directory must exist and be readable, or a saved case would be
 * written somewhere the replay never looks. */
static void test_corpus_directory_is_readable(void)
{
    DIR *d = opendir(PGW_CORPUS_DIR);
    PGW_ASSERT(d != NULL);
    if (d != NULL)
        closedir(d);
}
PGW_TEST(test_corpus_directory_is_readable);

/* The writer/reader pair itself, exercised in a temporary directory: a case
 * that is stored must load back identical, down to the digest of the
 * operations. This is what makes an empty tests/corpus/ trustworthy - the
 * machinery is proven even while no divergence has been captured. */
static void test_corpus_save_and_load_round_trips(void)
{
    char *dir = pgw_test_tmpdir("pgwcorpus");
    PGW_ASSERT(dir != NULL);
    if (dir == NULL)
        return;
    enum { N = 7 };
    diff_op *ops = calloc(N, sizeof(*ops));
    PGW_ASSERT(ops != NULL);
    diff_gen(ops, N, 0x9E3779B9u);

    pgw_corpus_case c;
    memset(&c, 0, sizeof(c));
    c.suite_seed = 0x9E3779B9u;
    c.n_ops = N;
    c.ops_digest = pgw_corpus_digest(ops, N);
    c.diverged_at = N;
    snprintf(c.detail, sizeof(c.detail), "d2/f7: mode differs from the truth");

    char path[PGW_CORPUS_PATH];
    PGW_ASSERT_EQ_I(pgw_corpus_save(dir, &c, path, sizeof(path)), 0);
    pgw_corpus_case back;
    PGW_ASSERT_EQ_I(pgw_corpus_load(path, &back), 0);
    PGW_ASSERT_EQ_U(back.suite_seed, c.suite_seed);
    PGW_ASSERT_EQ_U(back.n_ops, c.n_ops);
    PGW_ASSERT_EQ_U(back.ops_digest, c.ops_digest);
    PGW_ASSERT_EQ_I(back.diverged_at, c.diverged_at);
    PGW_ASSERT_STR_EQ(back.detail, c.detail);
    free(ops);
}
PGW_TEST(test_corpus_save_and_load_round_trips);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
