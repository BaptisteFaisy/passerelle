/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/ref9p_truth.h - the "truth" side of the reference benches.
 *
 * The reference server serves a real directory and the benches read that same
 * directory with plain POSIX calls: what the client obtained over 9P is only
 * accepted when the real file system agrees. These helpers are the whole of
 * that second opinion, so no bench re-implements a path join or a slurp.
 *
 * They are deliberately host-only (POSIX file system calls): the point of the
 * bench is that the truth does not come from the code under test.
 */
#ifndef REF9P_TRUTH_H
#define REF9P_TRUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "<root>/<rel>", or "<root>" when rel is empty. Always NUL-terminated. */
void ref9p_truth_path(const char *root, const char *rel, char *out, size_t cap);

/* Write/create, read back, stat (lstat: symlinks are not followed). */
bool ref9p_truth_write(const char *root, const char *rel, const void *data,
                       size_t len);
bool ref9p_truth_read(const char *root, const char *rel, void *buf, size_t cap,
                      size_t *out_len);
bool ref9p_truth_stat(const char *root, const char *rel, struct stat *out);
bool ref9p_truth_exists(const char *root, const char *rel);

/* Directory and link creation, so a bench can build the tree it serves. */
bool ref9p_truth_mkdir(const char *root, const char *rel, mode_t mode);
bool ref9p_truth_symlink(const char *root, const char *target,
                         const char *rel);

/* The names a real readdir() reports in "<root>/<rel>", sorted, without the
 * "." and ".." pseudo entries. Returns the count, or (size_t)-1 on error. */
size_t ref9p_truth_names(const char *root, const char *rel, char names[][256],
                         size_t max_names);

#ifdef __cplusplus
}
#endif

#endif /* REF9P_TRUTH_H */
