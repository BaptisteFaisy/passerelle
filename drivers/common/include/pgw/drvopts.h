/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pgw/drvopts.h - mount options shared by the two QNX drivers.
 *
 * The option list is identical for both drivers (`-o ro,noexec,nosuid,...`) and
 * only a handful of keys are protocol specific, so the parser lives here once
 * and each driver registers a hook for its own keys. This is the portable part
 * of the driver: it has no QNX dependency and is compiled by the PC build.
 *
 * Covered requirements: FS-09 / SEC-01 (ro, noexec, nosuid with nosuid on by
 * default), FS-13 (bounded attribute cache via attrttl=), P9-01 (msize=),
 * ROB-05 (timeout=), VIO-05 / VFS-01 (tag=), VIO-04 (irq= for the INTx
 * fallback), QNX-08 (debug=).
 */
#ifndef PGW_DRVOPTS_H
#define PGW_DRVOPTS_H

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every option the drivers accept, already validated. */
typedef struct pgw_mount_opts {
    pgw_fs_opts fs;         /* ro / noexec / nosuid / attr ttl / inflight */
    uint32_t timeout_ms;    /* per-request deadline, 0 = wait (ROB-05) */
    uint32_t msize;         /* 9P message size / FUSE max_write */
    int32_t irq;            /* >= 0: attach this INTx line (VIO-04) */
    bool debug;             /* stay in the foreground, verbose logging */
    char tag[64];           /* share tag (VIO-05, VFS-01) */
    const char *mount_point; /* absolute, no trailing slash */
} pgw_mount_opts;

/* Protocol-specific keys. Returns true when the key was recognised (and its
 * value validated, or PGW_EINVAL stored in *rc). */
typedef bool (*pgw_opt_hook)(pgw_mount_opts *opts, const char *key, size_t key_len,
                             const char *val, size_t val_len, pgw_err *rc);

/* Defaults: rw, exec allowed, nosuid, no attribute cache, msize and timeout
 * from pgw/limits.h. */
void pgw_opts_default(pgw_mount_opts *opts);

/* Parse a comma-separated `-o` list. `hook` may be NULL. Unknown keys are an
 * error so a typo cannot silently change behaviour. */
pgw_err pgw_opts_parse(const char *list, pgw_mount_opts *opts, pgw_opt_hook hook);

/* Bounded unsigned parse of a non-NUL-terminated slice: PGW_EINVAL when the
 * text is not a decimal or 0x-prefixed number, or falls outside [min,max]. */
pgw_err pgw_opts_u32(const char *text, size_t len, uint32_t min, uint32_t max,
                     uint32_t *out);

/* True when `key` (a slice) is exactly `name`. */
bool pgw_opts_key_is(const char *key, size_t key_len, const char *name);

/* Whether the mount lets layer 1 negotiate MSI-X (VIO-04). `-o irq=N` is the
 * documented escape hatch to force the classic line, and it has to decide at the
 * transport: enabling MSI-X and then waiting on line N would mean the device
 * signals through a vector nobody listens to, and no interrupt would arrive.
 * One rule for both drivers, so they cannot drift apart. */
bool pgw_opts_allow_msix(const pgw_mount_opts *opts);

#endif /* PGW_DRVOPTS_H */
