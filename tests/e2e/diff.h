/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * diff.h - the differential conformance bench, PC edition (spec 7.2).
 *
 * The host folder is the truth. A seeded sequence of filesystem operations is
 * replayed through the layer under test (pgw_rmfs over a backend), then the
 * tree the layer reports is compared field by field with the tree the PC really
 * holds: names, types, sizes, modes, link targets and a content digest. Any
 * divergence is reported with the seed and reduced to the shortest prefix that
 * reproduces it, which is what turns a random failure into a regression test.
 *
 * The very same suite runs inside a QNX guest against a real share (host/fsproof
 * drives it there), which is what makes the comparison between clients
 * objective (PERF-07).
 */
#ifndef PGW_TESTS_DIFF_H
#define PGW_TESTS_DIFF_H

#include "posix_backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGW_DIFF_MAX_OPS 4096
#define PGW_DIFF_PATHS 20
#define PGW_DIFF_MSG 512

typedef enum {
    OP_CREATE = 0,
    OP_WRITE,
    OP_READ,
    OP_TRUNC,
    OP_MKDIR,
    OP_RMDIR,
    OP_SYMLINK,
    OP_LINK,
    OP_RENAME,
    OP_UNLINK,
    OP_CHMOD,
    OP_READDIR,
    OP_STAT,
    OP_KIND_MAX
} diff_op_kind;

/* One generated operation. `a` and `b` index the path pool; the pool itself is
 * derived from the seed so a suite is fully reproducible. */
typedef struct diff_op {
    diff_op_kind kind;
    uint8_t a;
    uint8_t b;
    uint32_t off;
    uint32_t len;
    uint32_t mode;
    uint32_t seed;
} diff_op;

/* Name of pool entry `i` for a given suite seed (e.g. "d2/f7"). */
void diff_path(uint32_t suite_seed, unsigned i, char *out, size_t cap);

/* Generate `n` operations deterministically from `seed`. */
void diff_gen(diff_op *ops, size_t n, uint32_t seed);

/* Human-readable name of an operation kind, for failure reports. */
const char *diff_kind_name(diff_op_kind k);

/* Replay `n` operations in a fresh temporary tree and compare with the truth.
 * Returns the index of the first divergence, or -1 when the run is clean.
 * `err` receives a one-line explanation. */
long diff_run(const diff_op *ops, size_t n, uint32_t seed, char *err,
              size_t err_cap);

/* Binary-search the shortest prefix of `ops` that still diverges. Returns the
 * prefix length (>= 1), or -1 when the divergence does not reproduce. */
long diff_reduce(const diff_op *ops, size_t n, uint32_t seed, char *err,
                 size_t err_cap);

/* --- verification primitives (diff_verify.c) ------------------------------ */

/* FNV-1a 64 over a buffer; used as the content digest of a file. */
uint64_t diff_hash(const void *data, size_t len);
uint64_t diff_hash_mix(uint64_t h, uint64_t v);

/* Compare what `fs` reports for `relpath` with the real file at `root/relpath`.
 * Returns PGW_OK or the first mismatch described in `err`. */
typedef struct pgw_rmfs pgw_rmfs;
int diff_verify_path(pgw_rmfs *fs, const char *root, const char *relpath,
                     char *err, size_t err_cap);

/* Compare the whole tree: every entry of the real tree must be reported
 * identically by `fs`, and every directory listing of `fs` must contain no
 * entry that the real tree lacks. */
int diff_verify_tree(pgw_rmfs *fs, const char *root, char *err, size_t err_cap);

/* Write `len` bytes of a deterministic pattern seeded by `seed` at `off`. */
void diff_fill(void *buf, size_t len, uint32_t seed, uint64_t off);

#ifdef __cplusplus
}
#endif

#endif /* PGW_TESTS_DIFF_H */
