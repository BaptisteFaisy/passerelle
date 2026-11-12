/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * cas.h - shared fixtures of the directed cases (spec 7.4, gaps 63..69).
 *
 * The directed cases need the same three objects as the differential bench:
 * a real directory (the truth), a backend over it and the layer under test.
 * This header declares that fixture once, so no case re-implements the setup,
 * and adds the two helpers no other test provides:
 *
 *   - cas_probe: a counting decorator around the POSIX backend, used to
 *     prove that a call (fsync above all) really reaches the backend and is
 *     not swallowed by the attribute cache;
 *   - cas_env_u64: the scale of a case (the size of the big file, the number
 *     of entries of the big directory), so the same binary runs the short
 *     variant in a pull request and the long one in the nightly campaigns.
 *
 * Covers PERF-04, PERF-05, FS-10, FS-15, FS-12, ROB-03 (gaps 63, 64, 65,
 * 66, 67, 69 of docs/gap-analysis.md).
 */
#ifndef PGW_TESTS_CAS_H
#define PGW_TESTS_CAS_H

#include "posix_backend.h"

#include "pgw/rmfs.h"
#include "pgw/test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A failure that says *what* diverged: a directed case must not only report
 * that something is wrong, it must name the value and the layer. */
#define CAS_FAIL(...) pgw_fail(__FILE__, __LINE__, __VA_ARGS__)
#define CAS_ASSERT_MSG(cond, ...)                                              \
    do {                                                                       \
        if (!(cond))                                                           \
            CAS_FAIL(__VA_ARGS__);                                             \
    } while (0)

/* The injectable clock of pgw/time.h: a case that must see an attribute
 * expire installs it and advances it, instead of sleeping. */
uint64_t cas_fake_clock(void *user);

/* --- the counting backend (proof that a call reaches the backend) --------- */

typedef struct cas_probe {
    const pgw_fs_backend_ops *next; /* the wrapped vtable */
    void *next_impl;                /* the wrapped implementation */
    size_t lookup_calls;
    size_t getattr_calls;
    size_t open_calls;
    size_t close_calls;
    size_t read_calls;
    size_t write_calls;
    size_t readdir_calls;
    size_t fsync_calls;
} cas_probe;

/* The vtable to hand to pgw_rmfs_create() when the probe wraps a backend. */
extern const pgw_fs_backend_ops cas_probe_ops;

/* --- the fixture ---------------------------------------------------------- */

typedef struct cas_mount {
    char root[512];        /* the temporary directory: the truth */
    pgw_posix_backend *be; /* truth backend */
    pgw_rmfs *fs;          /* layer under test */
    pgw_fs_cred cred;      /* the owner of the files (never hardcoded) */
    cas_probe *probe;      /* the caller's counters, NULL when not probed */
} cas_mount;

/* Open the fixture over a fresh temporary directory. `opts` NULL means the
 * defaults. When `probe` is not NULL, the layer is built over the counting
 * decorator, so the case can see every call that reaches the backend. */
bool cas_mount_open(cas_mount *m, const char *tag, const pgw_fs_opts *opts,
                    cas_probe *probe);
/* A second mount over an *existing* directory: the durability case closes
 * everything and comes back, as a driver would after a crash. */
bool cas_mount_open_at(cas_mount *m, const char *root,
                       const pgw_fs_opts *opts, cas_probe *probe);
void cas_mount_close(cas_mount *m);

/* Absolute host path of a relative path inside the fixture. */
void cas_mount_abs(const cas_mount *m, const char *rel, char *out, size_t cap);

/* --- scale of a case ------------------------------------------------------ */

/* Read a decimal number from the environment, `def` when unset or invalid. */
uint64_t cas_env_u64(const char *name, uint64_t def);

/* --- truth side (plain POSIX, never the code under test) ------------------ */

/* A path buffer wide enough for the limit cases of spec 7.4 (PATH_MAX plus a
 * share root plus the separator). */
#define CAS_PATH_MAX 4200

/* Truth-side errno probes. The limit cases compare the layer under test with
 * what the kernel itself answers, so these return 0 on success and the raw
 * errno on failure - never a translated code, and never the layer's. */
int cas_truth_mkdir_errno(const char *root, const char *rel, uint32_t mode);
int cas_truth_stat_errno(const char *root, const char *rel);
int cas_truth_create_errno(const char *root, const char *rel);

/* Size of the real file, or 0 when it does not exist. */
uint64_t cas_truth_size(const char *root, const char *rel);
bool cas_truth_exists(const char *root, const char *rel);
/* Blocks really allocated on the disk (st_blocks * 512): the proof that the
 * big file stayed sparse. */
uint64_t cas_truth_blocks(const char *root, const char *rel);
/* Number of real entries of a directory, "." and ".." excluded. */
size_t cas_truth_count(const char *root, const char *rel);
/* Read a whole file of the truth into `buf`; false when it cannot be read. */
bool cas_truth_read(const char *root, const char *rel, void *buf, size_t cap,
                    size_t *out_len);

/* --- host helpers --------------------------------------------------------- */

/* Number of file descriptors the process holds, or 0 when the platform does
 * not tell (the load case then uses the backend's own counters). */
size_t cas_fd_count(void);
/* Ask the kernel to drop the page cache of a file: the closest thing to a
 * crash simulation that a test may do without privileges. */
bool cas_drop_cache(const char *path);
/* "<a>/<b>", always NUL terminated. */
void cas_path_join(char *out, size_t cap, const char *a, const char *b);

#endif /* PGW_TESTS_CAS_H */
