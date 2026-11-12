/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_corpus.h - the kept corpus of the fuzzing campaigns.
 *
 * The campaigns of this directory regenerate their input from a seed, so
 * nothing of them survives a run: a decoder that used to misbehave on some
 * input would simply misbehave again, with nothing to replay. This header
 * keeps what is worth keeping as *data* under tests/corpus/fuzz/, so the
 * normal CTest suite can replay it on every pull request.
 *
 * The selection rule (also written in tests/corpus/README.md):
 *   - one file per (family, depth): the first input of a campaign that
 *     reached that depth wins, which makes the selection reproducible from
 *     the seed;
 *   - the depth is how far the input went into the decoders (see
 *     fuzz_decode.h): 0 the header was refused, 1 the type was unknown,
 *     2 the body was refused, 3 the message decoded entirely;
 *   - the corpus is capped: at most FUZZ_CORPUS_MAX_PER_FAMILY files per
 *     family, at most FUZZ_CORPUS_MAX_FAMILIES families, and never more than
 *     FUZZ_CORPUS_MAX_BYTES per file.
 *
 * An input that made a decoder misbehave - the header invariant broken - is
 * a failure of the campaign, not a corpus entry: it is reported by the
 * campaign itself, and the fix that follows is what the corpus then keeps.
 */
#ifndef FUZZ_CORPUS_H
#define FUZZ_CORPUS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* The cap of the kept corpus. */
#define FUZZ_CORPUS_MAX_PER_FAMILY 4
#define FUZZ_CORPUS_MAX_FAMILIES 16
#define FUZZ_CORPUS_MAX_BYTES 512
#define FUZZ_CORPUS_NAME_MAX 32
#define FUZZ_CORPUS_PATH_MAX 256
/* A path is a corpus directory plus a family and a file name: the buffer is
 * bigger so that the compiler can prove the concatenation never truncates. */
#define FUZZ_CORPUS_FULL_MAX (FUZZ_CORPUS_PATH_MAX + 128)

typedef struct {
    bool used;
    char family[FUZZ_CORPUS_NAME_MAX];
    int depth;
    uint8_t data[FUZZ_CORPUS_MAX_BYTES];
    size_t len;
} fuzz_corpus_entry;

typedef struct {
    fuzz_corpus_entry entries[FUZZ_CORPUS_MAX_FAMILIES * FUZZ_CORPUS_MAX_PER_FAMILY];
    size_t count;
    char dir[FUZZ_CORPUS_PATH_MAX];
    char sub[FUZZ_CORPUS_NAME_MAX];
} fuzz_corpus;

/* Prepare the corpus: `dir` is the root of the corpus, `sub` the family
 * directory inside it ("p9" or "fuse"). */
static inline void fuzz_corpus_init(fuzz_corpus *c, const char *dir, const char *sub)
{
    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    snprintf(c->sub, sizeof c->sub, "%s", sub);
}

/* Offer one input. The first input of a (family, depth) pair wins. */
static inline void fuzz_corpus_offer(fuzz_corpus *c, const char *family,
                                     const uint8_t *data, size_t len, int depth)
{
    if (c == NULL || data == NULL || len == 0 || len > FUZZ_CORPUS_MAX_BYTES)
        return;
    size_t limit = FUZZ_CORPUS_MAX_FAMILIES * FUZZ_CORPUS_MAX_PER_FAMILY;
    for (size_t i = 0; i < c->count; i++) {
        if (c->entries[i].used && c->entries[i].depth == depth &&
            strcmp(c->entries[i].family, family) == 0)
            return;
    }
    if (c->count >= limit)
        return;
    fuzz_corpus_entry *e = &c->entries[c->count];
    e->used = true;
    e->depth = depth;
    e->len = len;
    snprintf(e->family, sizeof e->family, "%s", family);
    memcpy(e->data, data, len);
    c->count++;
}

/* Write the corpus to disk. Returns the number of files written. */
static inline int fuzz_corpus_flush(fuzz_corpus *c)
{
    if (c == NULL || c->count == 0)
        return 0;
    char path[FUZZ_CORPUS_FULL_MAX];
    (void)mkdir(c->dir, 0755);
    snprintf(path, sizeof path, "%s/%s", c->dir, c->sub);
    (void)mkdir(path, 0755);
    int written = 0;
    for (size_t i = 0; i < c->count; i++) {
        const fuzz_corpus_entry *e = &c->entries[i];
        if (!e->used)
            continue;
        snprintf(path, sizeof path, "%s/%s/%s-%d.bin", c->dir, c->sub, e->family,
                 e->depth);
        FILE *f = fopen(path, "wb");
        if (f == NULL)
            continue;
        size_t n = fwrite(e->data, 1u, e->len, f);
        fclose(f);
        if (n == e->len)
            written++;
    }
    return written;
}

#endif /* FUZZ_CORPUS_H */
