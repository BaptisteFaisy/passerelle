/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/lock.h - Guest-local byte-range lock definitions.
 *
 * Implements requirement FS-14:
 *   - Local byte-range locks between guest processes
 *   - Conflicting RD / WR locks
 *   - Automatic release of all locks held by a deceased process (ROB-02)
 */
#ifndef RMFS_LOCK_H
#define RMFS_LOCK_H

#include "pgw/limits.h"
#include "pgw/rmfs.h"

typedef struct rmfs_lock_table rmfs_lock_table;

rmfs_lock_table *rmfs_lock_table_create(void);
void rmfs_lock_table_destroy(rmfs_lock_table *t);

pgw_err rmfs_lock_apply(rmfs_lock_table *t, const char *path, uint32_t owner_pid,
                        pgw_lock_type type, uint64_t start, uint64_t len);
pgw_err rmfs_lock_test(rmfs_lock_table *t, const char *path, uint32_t owner_pid,
                       pgw_lock_type *type, uint64_t *start, uint64_t *len,
                       uint32_t *holder_pid);
void rmfs_lock_release_pid(rmfs_lock_table *t, uint32_t owner_pid);

#endif /* RMFS_LOCK_H */
