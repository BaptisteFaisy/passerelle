/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/e2e/corpus.h - persist a reduced divergence as a regression case.
 *
 * Gap 62 of docs/gap-analysis.md. diff_reduce() already minimises a divergence
 * of the differential bench to its shortest prefix; this header makes that
 * prefix *durable*: tools/corpus-save.sh runs the bench with PGW_CORPUS_SAVE
 * pointing at tests/corpus/, and every reduced case is written there as a
 * documented, machine-readable file that tests/corpus replays on each CTest
 * run. A divergence that was found once can then never come back unnoticed.
 *
 * The file format is key: value lines ('#' comments ignored), so that the C
 * replay test and any host tool read the same bytes:
 *
 *   pgw-corpus: 1        format version, refused when unknown
 *   suite_seed: N        the seed diff_gen() was called with
 *   n_ops: N             the number of operations of the reduced prefix
 *   ops_digest: 16 hex   FNV-1a over the operations themselves
 *   diverged_at: N       index of the operation that diverged (report only)
 *   detail: text         the one-line explanation the bench produced
 *
 * The operations are *not* stored: diff_gen() is a sequential PRNG, so
 * (suite_seed, n_ops) reproduces the suite byte for byte. ops_digest pins that
 * promise down - the replay test recomputes it and refuses a file whose
 * operations no longer match, instead of silently replaying a different case.
 *
 * Writing happens only on an explicit request (the environment variable), and
 * the replay test only ever opens these files for reading: nothing in the
 * normal suite writes to the repository tree.
 */
#ifndef PGW_TESTS_CORPUS_H
#define PGW_TESTS_CORPUS_H

#include "diff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PGW_CORPUS_VERSION 1u
#define PGW_CORPUS_ENV     "PGW_CORPUS_SAVE"
#define PGW_CORPUS_EXT     ".pgwcase"
#define PGW_CORPUS_DETAIL  PGW_DIFF_MSG /* the bench message, whole */
#define PGW_CORPUS_PATH    512u

typedef struct pgw_corpus_case {
    uint32_t suite_seed;
    size_t n_ops;
    uint64_t ops_digest;
    long diverged_at;
    char detail[PGW_CORPUS_DETAIL];
} pgw_corpus_case;

/* FNV-1a over the *fields* of every operation, not over the struct: padding
 * bytes are not part of the case and must not change the digest. */
static inline uint64_t pgw_corpus_digest(const diff_op *ops, size_t n)
{
    uint64_t h = diff_hash("pgw-corpus", 10);
    for (size_t i = 0; i < n; i++) {
        h = diff_hash_mix(h, (uint64_t)ops[i].kind);
        h = diff_hash_mix(h, ops[i].a);
        h = diff_hash_mix(h, ops[i].b);
        h = diff_hash_mix(h, ops[i].off);
        h = diff_hash_mix(h, ops[i].len);
        h = diff_hash_mix(h, ops[i].mode);
        h = diff_hash_mix(h, ops[i].seed);
    }
    return h;
}

/* Write `c` into `dir`. `path` (optional) receives the file that was written.
 * Returns 0 on success. */
static inline int pgw_corpus_save(const char *dir, const pgw_corpus_case *c,
                                  char *path, size_t path_cap)
{
    char local[PGW_CORPUS_PATH];
    char *out = (path != NULL) ? path : local;
    size_t cap = (path != NULL) ? path_cap : sizeof(local);
    int n = snprintf(out, cap, "%s/diff-%08X-%zu%s", dir, c->suite_seed,
                     c->n_ops, PGW_CORPUS_EXT);
    if (n <= 0 || (size_t)n >= cap)
        return -1;
    FILE *f = fopen(out, "w");
    if (f == NULL)
        return -1;
    fprintf(f,
            "# SPDX-License-Identifier: Apache-2.0\n"
            "#\n"
            "# A reduced divergence of the differential bench (tests/e2e),\n"
            "# saved by tools/corpus-save.sh with PGW_CORPUS_SAVE. Replayed by\n"
            "# tests/corpus on every CTest run: if it diverges again, the\n"
            "# replay test fails. See tests/e2e/corpus.h for the format.\n"
            "pgw-corpus: %u\nsuite_seed: %u\nn_ops: %zu\nops_digest: %016llX\n"
            "diverged_at: %ld\ndetail: %s\n",
            PGW_CORPUS_VERSION, c->suite_seed, c->n_ops,
            (unsigned long long)c->ops_digest, c->diverged_at, c->detail);
    return (fclose(f) == 0) ? 0 : -1;
}

static inline bool pgw_corpus_field(const char *line, const char *key,
                                    const char **val)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0 || line[n] != ':')
        return false;
    *val = line + n + 1;
    while (**val == ' ')
        (*val)++;
    return true;
}

/* Read one corpus file. Returns 0 on success, -1 on a malformed or unknown
 * file (which the replay test reports rather than skips). */
static inline int pgw_corpus_load(const char *path, pgw_corpus_case *out)
{
    FILE *f = fopen(path, "r"); /* read-only: the replay never writes */
    if (f == NULL)
        return -1;
    memset(out, 0, sizeof(*out));
    char line[PGW_CORPUS_DETAIL]; /* a longer line is not ours to store */
    unsigned version = 0;
    const char *v = NULL;
    int rc = -1;
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (pgw_corpus_field(line, "pgw-corpus", &v))
            version = (unsigned)strtoul(v, NULL, 10);
        else if (pgw_corpus_field(line, "suite_seed", &v))
            out->suite_seed = (uint32_t)strtoul(v, NULL, 10);
        else if (pgw_corpus_field(line, "n_ops", &v))
            out->n_ops = (size_t)strtoull(v, NULL, 10);
        else if (pgw_corpus_field(line, "ops_digest", &v))
            out->ops_digest = strtoull(v, NULL, 16);
        else if (pgw_corpus_field(line, "diverged_at", &v))
            out->diverged_at = strtol(v, NULL, 10);
        else if (pgw_corpus_field(line, "detail", &v))
            snprintf(out->detail, sizeof(out->detail), "%s", v);
    }
    if (version == PGW_CORPUS_VERSION && out->n_ops > 0)
        rc = 0;
    fclose(f);
    return rc;
}

#endif /* PGW_TESTS_CORPUS_H */
