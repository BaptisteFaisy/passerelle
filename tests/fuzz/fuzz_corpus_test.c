/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_corpus_test.c - replay the kept corpus (gap 48).
 *
 * The campaigns of this directory regenerate their input from a seed, so
 * nothing of them survives a run: an input that made a decoder misbehave
 * would simply be found again, with nothing to replay. The corpus of
 * tests/corpus/fuzz/ keeps the interesting inputs as data, and this bench
 * replays them in the normal CTest suite, on every pull request.
 *
 * The replay is read-only and fast: a few dozen small files, each pushed
 * through the decoders of its family in a few microseconds. The bench fails
 * when a stored input breaks the header invariant - that is, when a decoder
 * accepts a header that is out of contract.
 *
 * The cap is documented in tests/corpus/README.md and enforced here: the
 * corpus stays small enough to live in the repository.
 */
#define _POSIX_C_SOURCE 200809L
#include "fuzz_fuse_decode.h"
#include "fuzz_p9_decode.h"

#include "pgw/test.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PGW_FUZZ_CORPUS_DIR
#define PGW_FUZZ_CORPUS_DIR "."
#endif

/* The cap the corpus must stay under, and the size of one stored file. */
#define CORPUS_MAX_FILES 256u
#define CORPUS_MAX_BYTES 4096u
#define CORPUS_MAX_TOTAL_BYTES (256u * 1024u)

static size_t g_files;
static size_t g_bytes;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8u; i++)
        v |= (uint64_t)p[i] << (8u * i);
    return v;
}

/* Replay one stored input through the decoders of its family. */
static void replay_one(const char *path, bool fuse)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        pgw_fail(__FILE__, __LINE__, "%s: cannot be read", path);
        return;
    }
    uint8_t buf[CORPUS_MAX_BYTES];
    size_t len = fread(buf, 1u, sizeof buf, f);
    fclose(f);
    if (len == 0u) {
        pgw_fail(__FILE__, __LINE__, "%s: the stored input is empty", path);
        return;
    }
    if (fuse) {
        uint64_t unique = (len >= 16u) ? le64(buf + 8u) : 0u;
        (void)fuzz_decode_fuse(buf, len, unique);
        if (!fuzz_fuse_header_ok(buf, len)) {
            pgw_fail(__FILE__, __LINE__,
                     "%s: the FUSE header was accepted out of contract", path);
            return;
        }
    } else {
        uint16_t tag = (len >= 7u) ? le16(buf + 5u) : 0u;
        (void)fuzz_decode_p9(buf, len, tag);
        if (!fuzz_p9_header_ok(buf, len)) {
            pgw_fail(__FILE__, __LINE__, "%s: the 9P header was accepted out of contract",
                     path);
            return;
        }
    }
    g_files++;
    g_bytes += len;
}

/* Replay every file of one family directory. */
static void replay_dir(const char *dir, bool fuse)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 5u || strcmp(e->d_name + n - 4u, ".bin") != 0)
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        replay_one(path, fuse);
    }
    closedir(d);
}

static void test_fuzz_corpus_replays_every_stored_input(void)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/p9", PGW_FUZZ_CORPUS_DIR);
    replay_dir(dir, false);
    snprintf(dir, sizeof dir, "%s/fuse", PGW_FUZZ_CORPUS_DIR);
    replay_dir(dir, true);

    PGW_ASSERT(g_files > 0u);
    PGW_ASSERT(g_files <= (size_t)CORPUS_MAX_FILES);
    PGW_ASSERT(g_bytes <= (size_t)CORPUS_MAX_TOTAL_BYTES);
    fprintf(stdout, "# fuzz corpus: %zu input(s), %zu byte(s) replayed from %s\n",
            g_files, g_bytes, PGW_FUZZ_CORPUS_DIR);
}
PGW_TEST(test_fuzz_corpus_replays_every_stored_input);

/* The corpus must exist: a regression corpus that lost its files would let a
 * bug come back silently. */
static void test_fuzz_corpus_directory_is_readable(void)
{
    DIR *d = opendir(PGW_FUZZ_CORPUS_DIR);
    PGW_ASSERT(d != NULL);
    if (d != NULL)
        closedir(d);
}
PGW_TEST(test_fuzz_corpus_directory_is_readable);

int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
