/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_ref_harness.h - start the reference FUSE server and talk to it.
 *
 * Implements requirements:
 *   - VFS-03: the Passerelle FUSE client is exercised against a second,
 *     independent implementation of the protocol (tests/fuse_ref/fuse_ref_server.py)
 *     serving a real directory, so the client is no longer checked only against
 *     the in-memory fake server
 *   - VFS-05: the transport callback (pgw_fuse_rpc_fn) used here is the real
 *     one: a UNIX stream socket with a 4-byte big-endian length per message
 *
 * The harness owns the process and the socket, so a test that fails half way
 * still leaves no server behind (the exit handler kills it).
 */
#ifndef PGW_TESTS_FUSE_REF_HARNESS_H
#define PGW_TESTS_FUSE_REF_HARNESS_H

#include "pgw/err.h"
#include "pgw/fusep.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FUSE_REF_PATH_MAX 512u
#define FUSE_REF_NAME_MAX 256u
#define FUSE_REF_MAX_WRITE 65536u

/* Names seeded in the served directory: the "truth" the client is compared to. */
#define FUSE_REF_BIG_FILE "big.bin"
#define FUSE_REF_SMALL_FILE "small.txt"
#define FUSE_REF_DIR "sub"
#define FUSE_REF_INNER_FILE "inner.txt"
#define FUSE_REF_EMPTY_DIR "emptydir"
#define FUSE_REF_LINK "link"
#define FUSE_REF_LOOP "loop"

/* Size of FUSE_REF_BIG_FILE: larger than one max_write, so a read is split. */
#define FUSE_REF_BIG_SIZE 100000u

typedef struct fuse_ref_ctx {
    pgw_fuse_session *session; /* the client session, already negotiated */
    int fd;                     /* connected UNIX socket */
    pid_t pid;                  /* server process */
    char root[FUSE_REF_PATH_MAX]; /* served directory (the truth) */
} fuse_ref_ctx;

/* Start the server on a fresh temporary directory, connect and open a
 * session. `fault` is the --fault list of the server (NULL: none) and
 * `minor` the protocol minor it announces (NULL: 7.36). NULL on failure.
 * Both strings are borrowed, not copied: they must outlive the call. */
fuse_ref_ctx *fuse_ref_ctx_start(char *fault, char *minor);

/* Close the session, stop the server and free the context. */
void fuse_ref_ctx_stop(fuse_ref_ctx *c);

/* The pgw_fuse_rpc_fn the session is opened with. */
pgw_err fuse_ref_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                     size_t *rx_len, bool hiprio, uint32_t prio,
                     uint32_t timeout_ms, void *user);

/* --- truth-side oracle (fuse_ref_truth.c) ---------------------------------- */

/* Absolute path of `name` inside the served directory. */
void fuse_ref_disk_path(const fuse_ref_ctx *c, const char *name, char *out,
                        size_t cap);

/* Join `root` and `name` into `out` as "root/name". Unlike snprintf this never
 * warns about a possible truncation: it copies what fits and stops, and an
 * empty result means "did not fit". */
void fuse_ref_join(const char *root, const char *name, char *out, size_t cap);

/* Copy `src` into `dst`, always NUL-terminated, never longer than `cap`. */
void fuse_ref_copy_str(char *dst, size_t cap, const char *src);

/* lstat() of `name` in the served directory; false when it does not exist. */
bool fuse_ref_disk_lstat(const fuse_ref_ctx *c, const char *name,
                         struct stat *st);

/* Read the whole of `name` from the served directory into `buf`. */
bool fuse_ref_disk_read(const fuse_ref_ctx *c, const char *name, void *buf,
                        size_t cap, size_t *out_len);

/* Sorted names of the entries of `dir` in the served directory, "." and ".."
 * excluded. Returns the number of names written. */
size_t fuse_ref_disk_names(const fuse_ref_ctx *c, const char *dir,
                           char names[][FUSE_REF_NAME_MAX], size_t max);

/* Every entry of `dir` seen through the client (OPENDIR + READDIR + RELEASEDIR),
 * "." and ".." excluded, sorted by name. Returns the number of entries. */
size_t fuse_ref_client_names(fuse_ref_ctx *c, uint64_t nodeid,
                             char names[][FUSE_REF_NAME_MAX], size_t max);

/* Assert that GETATTR of `nodeid` reports exactly what the disk says. */
void fuse_ref_assert_attr_matches(const fuse_ref_ctx *c, uint64_t nodeid,
                                  const char *name);

#endif /* PGW_TESTS_FUSE_REF_HARNESS_H */
