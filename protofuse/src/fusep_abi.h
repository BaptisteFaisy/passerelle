/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_abi.h - Wire constants and internal layout facts.
 *
 * Implements requirements:
 *   - VFS-03: FUSE 7.x wire layout. The layout is re-stated here in our own
 *     words (field order and sizes only) from the Linux kernel ABI; no line of
 *     the kernel header is copied. See NOTICE section 4 for attribution.
 *
 * Every size below is the exact number of bytes the server puts on the wire,
 * including the padding the ABI requires so that a 32-bit server can talk to a
 * 64-bit client.
 */
#ifndef PGW_FUSEP_ABI_H
#define PGW_FUSEP_ABI_H

#include <stddef.h>
#include <stdint.h>

/* --- request bodies (after the 40-byte input header) ---------------------- */
#define PGW_FUSE_INIT_IN_SIZE       64u /* major, minor, max_readahead, flags,
                                         * flags2, unused[11] */
#define PGW_FUSE_FORGET_IN_SIZE     8u  /* nlookup */
#define PGW_FUSE_BATCH_FORGET_IN    8u  /* count, dummy */
#define PGW_FUSE_BATCH_FORGET_ONE   16u /* nodeid, nlookup */
#define PGW_FUSE_GETATTR_IN_SIZE    16u /* getattr_flags, dummy, fh */
#define PGW_FUSE_SETATTR_IN_SIZE    88u
#define PGW_FUSE_OPEN_IN_SIZE       8u  /* flags, open_flags */
#define PGW_FUSE_CREATE_IN_SIZE     16u /* flags, mode, umask, open_flags */
#define PGW_FUSE_READ_IN_SIZE       40u
#define PGW_FUSE_WRITE_IN_SIZE      40u
#define PGW_FUSE_RELEASE_IN_SIZE    24u /* fh, flags, release_flags,
                                         * lock_owner */
#define PGW_FUSE_MKDIR_IN_SIZE      8u  /* mode, umask */
#define PGW_FUSE_RENAME_IN_SIZE     8u  /* newdir */
#define PGW_FUSE_RENAME2_IN_SIZE    16u /* newdir, flags, padding */
#define PGW_FUSE_LINK_IN_SIZE       8u  /* oldnodeid */
#define PGW_FUSE_FSYNC_IN_SIZE      16u /* fh, fsync_flags, padding */
#define PGW_FUSE_INTERRUPT_IN_SIZE  8u  /* unique of the interrupted request */

/* --- reply bodies (after the 16-byte output header) ----------------------- */
#define PGW_FUSE_INIT_OUT_SIZE      64u /* full 7.36 reply */
#define PGW_FUSE_INIT_OUT_7_22      24u /* up to max_write */
#define PGW_FUSE_INIT_OUT_COMPAT    8u  /* major + minor only */
#define PGW_FUSE_ENTRY_HDR_SIZE     40u  /* entry fields, before fuse_attr */
#define PGW_FUSE_OPEN_OUT_SIZE      16u /* fh, open_flags, padding */
#define PGW_FUSE_WRITE_OUT_SIZE     8u  /* size, padding */
#define PGW_FUSE_KSTATFS_SIZE       80u
#define PGW_FUSE_KSTATFS_COMPAT     48u
#define PGW_FUSE_DIRENT_HDR_SIZE    24u /* ino, off, namelen, type */

/* fuse_attr itself: 6 x u64 then 9 x u32 (blksize last), then a u32 `flags`
 * that only exists from 7.32 on. We decode the 84 bytes we model and accept
 * the extra 4-byte flags word without interpreting it. */
#define PGW_FUSE_ATTR_SIZE          84u
#define PGW_FUSE_ATTR_SIZE_FLAGS    88u
#define PGW_FUSE_ATTR_SIZE_COMPAT   80u

/* --- per-request flag bits used by the encoders --------------------------- */
#define PGW_FUSE_GETATTR_FH_BIT      1u /* fuse_getattr_in.getattr_flags */
#define PGW_FUSE_FSYNC_FDATASYNC_BIT 1u /* fuse_fsync_in.fsync_flags */

/* --- protocol minors that gate a feature ---------------------------------- */
#define PGW_FUSE_MINOR_BATCH_FORGET  16u /* BATCH_FORGET exists from 7.16 */
#define PGW_FUSE_MINOR_RENAME2       23u /* RENAME2 exists from 7.23 */
#define PGW_FUSE_MINOR_MAX_PAGES     28u /* max_pages exists from 7.28 */
#define PGW_FUSE_MINOR_FLOOR         9u  /* blksize in fuse_attr, from 7.9 */

/* --- codec limits --------------------------------------------------------- */
#define PGW_FUSE_NAME_WIRE_MAX      255u  /* a name plus its NUL */
#define PGW_FUSE_READDIR_MAX        64u   /* entries decoded per reply chunk */
#define PGW_FUSE_READDIR_CHUNK      1024u /* bytes requested per READDIR */
#define PGW_FUSE_BATCH_MAX          32u   /* nodes per BATCH_FORGET message */
#define PGW_FUSE_MAX_READAHEAD      131072u /* what we announce in INIT */
#define PGW_FUSE_PAGE_SIZE          4096u
#define PGW_FUSE_MAX_PAGES_CAP      256u
#define PGW_FUSE_TX_SMALL           640u  /* stack request buffer (2 names) */

#endif /* PGW_FUSEP_ABI_H */
