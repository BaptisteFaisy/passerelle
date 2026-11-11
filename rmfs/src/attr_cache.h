/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rmfs/src/attr_cache.h - Inode attribute cache definitions.
 *
 * Implements requirements D4, FS-12, FS-13:
 *   - Optional attribute cache with configurable, bounded TTL
 *   - No data cache
 *   - Accounting for PGW_STAT_INODE_HIT and PGW_STAT_INODE_MISS
 */
#ifndef RMFS_ATTR_CACHE_H
#define RMFS_ATTR_CACHE_H

#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include <stdbool.h>

typedef struct rmfs_attr_cache rmfs_attr_cache;

rmfs_attr_cache *rmfs_attr_cache_create(uint64_t ttl_ns);
void rmfs_attr_cache_destroy(rmfs_attr_cache *c);

bool rmfs_attr_cache_get(rmfs_attr_cache *c, const char *relpath,
                         pgw_fs_attr *out);
void rmfs_attr_cache_put(rmfs_attr_cache *c, const char *relpath,
                         const pgw_fs_attr *attr);
void rmfs_attr_cache_invalidate(rmfs_attr_cache *c, const char *relpath);
void rmfs_attr_cache_clear(rmfs_attr_cache *c);

#endif /* RMFS_ATTR_CACHE_H */
