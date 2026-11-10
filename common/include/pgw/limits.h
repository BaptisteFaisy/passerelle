/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/limits.h - project-wide constants for Passerelle.
 *
 * Every tunable named in the specification lives here so that a driver, a test
 * and the documentation quote the same number. Values marked "measured
 * default" are the result of host/bench and may be overridden at mount time.
 */
#ifndef PGW_LIMITS_H
#define PGW_LIMITS_H

#include <stddef.h>
#include <stdint.h>

/* --- names and paths (FS-15) ---------------------------------------------- */
#define PGW_NAME_MAX        255u    /* UTF-8 bytes in one path element */
#define PGW_PATH_MAX        4096u   /* full path, host side */
#define PGW_SYMLINK_MAX     40u     /* resolution depth before ELOOP (FS-08) */

/* --- 9P message size (P9-01, spec 5.3 "taille des messages") --------------- */
#define PGW_MSIZE_MIN       4096u
#define PGW_MSIZE_DEFAULT   131072u /* 128 KiB, measured default, tunable */
#define PGW_MSIZE_MAX       (4u * 1024u * 1024u)
#define PGW_HEADER_SIZE     7u      /* size[4] type[1] tag[2] */
#define PGW_MAXWELEM        16u     /* max names in one Twalk */
#define PGW_TAG_NONE        0xFFFFu /* NOTAG, used by Tversion */
#define PGW_FID_NONE        0xFFFFFFFFu /* NOFID, used by Tattach afid */

/* --- virtio transport (VIO-*) --------------------------------------------- */
#define PGW_QUEUE_SIZE_DEFAULT 128u /* descriptors per split virtqueue */
#define PGW_QUEUE_SIZE_MIN     2u
#define PGW_QUEUE_SIZE_MAX     32768u
#define PGW_SG_MAX             32u  /* scatter-gather entries per request */
#define PGW_INDIRECT_THRESHOLD 4u   /* use indirect descriptors from N sg (VIO-08) */

/* virtio device identities (VIO-01, VFS-01) */
#define PGW_VIRTIO_PCI_VENDOR   0x1AF4u
#define PGW_VIRTIO_PCI_LEGACY_BASE 0x1000u /* transitional: 0x1000 + type */
#define PGW_VIRTIO_PCI_MODERN_BASE 0x1040u /* modern only:  0x1040 + type */
#define PGW_VIRTIO_ID_9P        9u
#define PGW_VIRTIO_ID_FS        26u
#define PGW_VIRTIO_PCI_DID_9P_TRANSITIONAL 0x1009u /* 0x1000 + 9 */
#define PGW_VIRTIO_PCI_DID_9P_MODERN       0x1049u /* 0x1040 + 9 */
#define PGW_VIRTIO_PCI_DID_FS_MODERN       0x105Au /* 0x1040 + 26 */

/* virtio-mmio magic and versions (VIO-03) */
#define PGW_MMIO_MAGIC      0x74726976u /* "virt" little-endian */
#define PGW_MMIO_VERSION_1  1u          /* legacy */
#define PGW_MMIO_VERSION_2  2u          /* modern */

/* --- request pipeline (P9-05, QNX-04, QNX-05, ROB-05) --------------------- */
#define PGW_INFLIGHT_DEFAULT   32u  /* requests in flight at once */
#define PGW_INFLIGHT_MAX       256u
#define PGW_PENDING_DEFAULT    64u  /* waiting requests, priority ordered */
#define PGW_PENDING_MAX        1024u
#define PGW_TIMEOUT_MS_DEFAULT 10000u /* give up on a silent server */
#define PGW_TIMEOUT_MS_MAX     3600000u

/* --- caching (decision D4, FS-13) ----------------------------------------- */
#define PGW_DATA_CACHE_DEFAULT   0  /* never cache data: the PC folder is truth */
#define PGW_ATTR_CACHE_NS_DEFAULT 0 /* attribute cache off unless asked for */
#define PGW_ATTR_CACHE_NS_MAX    10000000000ULL /* 10 s hard ceiling */

/* --- identifiers ---------------------------------------------------------- */
#define PGW_INO_ROOT        1ull    /* FUSE_ROOT_ID equivalent */
#define PGW_INO_NONE        0ull
#define PGW_FH_NONE         0ull

#endif /* PGW_LIMITS_H */
