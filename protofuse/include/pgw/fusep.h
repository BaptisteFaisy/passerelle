/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/fusep.h - Layer 2 (portable FUSE wire protocol for virtiofs).
 *
 * Implements requirements VFS-01 through VFS-05:
 *   - Protocol version negotiation (7.x)
 *   - Node reference counts (lookup / forget / batch_forget) (VFS-04)
 *   - File handles, getattr/setattr, lookup, open/create/release, read/write,
 *     readdir, mkdir/unlink/rmdir/rename, symlink/readlink/link, fsync, statfs,
 *     flush/interrupt
 *   - Strict decoder validation (no struct casting onto untrusted buffers)
 */
#ifndef PGW_FUSEP_H
#define PGW_FUSEP_H

#include "pgw/err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PGW_FUSE_MAJOR      7u
#define PGW_FUSE_MINOR      36u
#define PGW_FUSE_IN_HDR_SZ  40u
#define PGW_FUSE_OUT_HDR_SZ 16u
#define PGW_FUSE_ROOT_ID    1ULL

/* FUSE opcodes (wire numbers). */
enum pgw_fuse_op {
    PGW_FUSE_LOOKUP = 1,
    PGW_FUSE_FORGET = 2,
    PGW_FUSE_GETATTR = 3,
    PGW_FUSE_SETATTR = 4,
    PGW_FUSE_READLINK = 5,
    PGW_FUSE_SYMLINK = 6,
    PGW_FUSE_MKDIR = 9,
    PGW_FUSE_UNLINK = 10,
    PGW_FUSE_RMDIR = 11,
    PGW_FUSE_RENAME = 12,
    PGW_FUSE_LINK = 13,
    PGW_FUSE_OPEN = 14,
    PGW_FUSE_READ = 15,
    PGW_FUSE_WRITE = 16,
    PGW_FUSE_STATFS = 17,
    PGW_FUSE_RELEASE = 18,
    PGW_FUSE_FSYNC = 20,
    PGW_FUSE_FLUSH = 25,
    PGW_FUSE_INIT = 26,
    PGW_FUSE_OPENDIR = 27,
    PGW_FUSE_READDIR = 28,
    PGW_FUSE_RELEASEDIR = 29,
    PGW_FUSE_FSYNCDIR = 30,
    PGW_FUSE_DESTROY = 38,
    PGW_FUSE_CREATE = 35,
    PGW_FUSE_INTERRUPT = 36,
    PGW_FUSE_BATCH_FORGET = 42,
    PGW_FUSE_RENAME2 = 45
};

/* Init feature flags (wire bits). */
#define PGW_FUSE_F_ASYNC_READ       (1ULL << 0)
#define PGW_FUSE_F_ATOMIC_O_TRUNC   (1ULL << 3)
#define PGW_FUSE_F_BIG_WRITES       (1ULL << 5)
#define PGW_FUSE_F_DONT_MASK        (1ULL << 6)
#define PGW_FUSE_F_PARALLEL_DIROPS  (1ULL << 18)
#define PGW_FUSE_F_MAX_PAGES        (1ULL << 22)

/* Setattr valid bits (wire bits). */
#define PGW_FUSE_FATTR_MODE   (1u << 0)
#define PGW_FUSE_FATTR_UID    (1u << 1)
#define PGW_FUSE_FATTR_GID    (1u << 2)
#define PGW_FUSE_FATTR_SIZE   (1u << 3)
#define PGW_FUSE_FATTR_ATIME  (1u << 4)
#define PGW_FUSE_FATTR_MTIME  (1u << 5)
#define PGW_FUSE_FATTR_CTIME  (1u << 10)

typedef struct pgw_fuse_in_hdr {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
} pgw_fuse_in_hdr;

typedef struct pgw_fuse_out_hdr {
    uint32_t len;
    int32_t error; /* negative Linux errno, or 0 */
    uint64_t unique;
} pgw_fuse_out_hdr;

typedef struct pgw_fuse_attr {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
    uint32_t blksize;
} pgw_fuse_attr;

typedef struct pgw_fuse_entry {
    uint64_t nodeid;
    uint64_t generation;
    uint64_t entry_valid;
    uint64_t attr_valid;
    uint32_t entry_valid_nsec;
    uint32_t attr_valid_nsec;
    pgw_fuse_attr attr;
} pgw_fuse_entry;

typedef struct pgw_fuse_init_info {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint64_t flags;
    uint32_t max_write;
    uint32_t max_pages;
} pgw_fuse_init_info;

typedef struct pgw_fuse_kstatfs {
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint32_t bsize;
    uint32_t namelen;
    uint32_t frsize;
} pgw_fuse_kstatfs;

typedef struct pgw_fuse_dirent {
    uint64_t ino;
    uint64_t off;
    uint32_t type;
    char name[256];
    uint16_t namelen;
} pgw_fuse_dirent;

/* The wire structure is named *_in, like the kernel's fuse_setattr_in: the
 * bare name `pgw_fuse_setattr` is the operation, and C has a single namespace
 * for typedefs and functions, so the two cannot share a spelling. */
typedef struct pgw_fuse_setattr {
    uint32_t valid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime;
    uint32_t atime_nsec;
    uint64_t mtime;
    uint32_t mtime_nsec;
} pgw_fuse_setattr_in;

/* --- codecs --------------------------------------------------------------- */

int32_t pgw_fuse_enc_init(void *buf, size_t cap, uint64_t unique,
                          uint32_t max_readahead, uint64_t flags);
int32_t pgw_fuse_enc_lookup(void *buf, size_t cap, uint64_t unique,
                            uint64_t parent, const char *name);
int32_t pgw_fuse_enc_forget(void *buf, size_t cap, uint64_t unique,
                            uint64_t nodeid, uint64_t nlookup);
int32_t pgw_fuse_enc_getattr(void *buf, size_t cap, uint64_t unique,
                             uint64_t nodeid, uint64_t fh, bool has_fh);
int32_t pgw_fuse_enc_setattr(void *buf, size_t cap, uint64_t unique,
                             uint64_t nodeid, const pgw_fuse_setattr_in *sa);
int32_t pgw_fuse_enc_open(void *buf, size_t cap, uint64_t unique,
                          uint64_t nodeid, uint32_t linux_flags, bool is_dir);
int32_t pgw_fuse_enc_create(void *buf, size_t cap, uint64_t unique,
                            uint64_t parent, const char *name,
                            uint32_t linux_flags, uint32_t mode, uint32_t umask);
int32_t pgw_fuse_enc_release(void *buf, size_t cap, uint64_t unique,
                             uint64_t nodeid, uint64_t fh, bool is_dir);
int32_t pgw_fuse_enc_read(void *buf, size_t cap, uint64_t unique,
                          uint64_t nodeid, uint64_t fh, uint64_t off,
                          uint32_t size, bool is_dir);
int32_t pgw_fuse_enc_write(void *buf, size_t cap, uint64_t unique,
                           uint64_t nodeid, uint64_t fh, uint64_t off,
                           const void *data, uint32_t size);
int32_t pgw_fuse_enc_mkdir(void *buf, size_t cap, uint64_t unique,
                           uint64_t parent, const char *name,
                           uint32_t mode, uint32_t umask);
int32_t pgw_fuse_enc_name_op(void *buf, size_t cap, uint32_t opcode,
                             uint64_t unique, uint64_t parent, const char *name);
int32_t pgw_fuse_enc_rename(void *buf, size_t cap, uint64_t unique,
                            uint64_t olddir, const char *oldname,
                            uint64_t newdir, const char *newname, uint32_t flags);
int32_t pgw_fuse_enc_symlink(void *buf, size_t cap, uint64_t unique,
                             uint64_t parent, const char *name, const char *target);
int32_t pgw_fuse_enc_link(void *buf, size_t cap, uint64_t unique,
                          uint64_t parent, uint64_t oldnodeid, const char *name);
int32_t pgw_fuse_enc_readlink(void *buf, size_t cap, uint64_t unique,
                              uint64_t nodeid);
int32_t pgw_fuse_enc_fsync(void *buf, size_t cap, uint64_t unique,
                           uint64_t nodeid, uint64_t fh, bool datasync, bool is_dir);
int32_t pgw_fuse_enc_statfs(void *buf, size_t cap, uint64_t unique,
                            uint64_t nodeid);
int32_t pgw_fuse_enc_interrupt(void *buf, size_t cap, uint64_t unique,
                               uint64_t target_unique);

/* --- decoders ------------------------------------------------------------- */

pgw_err pgw_fuse_dec_hdr(const void *msg, size_t len, pgw_fuse_out_hdr *out);
/* The header of a request this client wrote (a driver reads it back to label
 * the kernel trace of QNX-09). */
pgw_err pgw_fuse_dec_in_hdr(const void *msg, size_t len, pgw_fuse_in_hdr *out);
pgw_err pgw_fuse_dec_init(const void *msg, size_t len, uint64_t unique,
                          pgw_fuse_init_info *out);
pgw_err pgw_fuse_dec_entry(const void *msg, size_t len, uint64_t unique,
                           pgw_fuse_entry *out);
pgw_err pgw_fuse_dec_attr(const void *msg, size_t len, uint64_t unique,
                          pgw_fuse_attr *out);
pgw_err pgw_fuse_dec_open(const void *msg, size_t len, uint64_t unique,
                          uint64_t *fh, uint32_t *open_flags);
pgw_err pgw_fuse_dec_create(const void *msg, size_t len, uint64_t unique,
                            pgw_fuse_entry *entry, uint64_t *fh);
pgw_err pgw_fuse_dec_read(const void *msg, size_t len, uint64_t unique,
                          const uint8_t **data, size_t *data_len);
pgw_err pgw_fuse_dec_write(const void *msg, size_t len, uint64_t unique,
                           uint32_t *size);
pgw_err pgw_fuse_dec_readdir(const void *msg, size_t len, uint64_t unique,
                             pgw_fuse_dirent *out, size_t max_entries,
                             size_t *out_count);
pgw_err pgw_fuse_dec_readlink(const void *msg, size_t len, uint64_t unique,
                              char *target, size_t cap);
pgw_err pgw_fuse_dec_statfs(const void *msg, size_t len, uint64_t unique,
                            pgw_fuse_kstatfs *out);
pgw_err pgw_fuse_dec_empty(const void *msg, size_t len, uint64_t unique);

/* --- inode refcount table + session (VFS-02, VFS-04) ---------------------- */

/* Priority-marked RPC callback: `hiprio` requests go on virtiofs queue 0
 * (FORGET, INTERRUPT) and normal requests go on request queues >= 1. */
typedef pgw_err (*pgw_fuse_rpc_fn)(const void *tx, size_t tx_len,
                                   void *rx, size_t rx_cap, size_t *rx_len,
                                   bool hiprio, uint32_t prio,
                                   uint32_t timeout_ms, void *user);

typedef struct pgw_fuse_session pgw_fuse_session;

pgw_fuse_session *pgw_fuse_session_open(pgw_fuse_rpc_fn rpc, void *user,
                                        uint32_t max_write);
void pgw_fuse_session_close(pgw_fuse_session *s); /* sends FORGET for all refs */

/* Refcount table helpers (VFS-04). */
uint64_t pgw_fuse_ref_get(const pgw_fuse_session *s, uint64_t nodeid);
size_t pgw_fuse_ref_count_nodes(const pgw_fuse_session *s);
pgw_err pgw_fuse_forget_node(pgw_fuse_session *s, uint64_t nodeid);
pgw_err pgw_fuse_interrupt(pgw_fuse_session *s, uint64_t target_unique);

pgw_err pgw_fuse_lookup(pgw_fuse_session *s, uint64_t parent, const char *name,
                        pgw_fuse_entry *out, uint32_t prio);
pgw_err pgw_fuse_getattr(pgw_fuse_session *s, uint64_t nodeid,
                         pgw_fuse_attr *out, uint32_t prio);
pgw_err pgw_fuse_setattr(pgw_fuse_session *s, uint64_t nodeid,
                         const pgw_fuse_setattr_in *sa, pgw_fuse_attr *out,
                         uint32_t prio);
pgw_err pgw_fuse_open(pgw_fuse_session *s, uint64_t nodeid, uint32_t pgw_oflags,
                      bool is_dir, uint64_t *out_fh, uint32_t prio);
pgw_err pgw_fuse_create(pgw_fuse_session *s, uint64_t parent, const char *name,
                        uint32_t pgw_oflags, uint32_t mode,
                        pgw_fuse_entry *out_e, uint64_t *out_fh, uint32_t prio);
pgw_err pgw_fuse_release(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                         bool is_dir, uint32_t prio);
pgw_err pgw_fuse_read(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                      uint64_t off, void *buf, size_t len, size_t *out_read,
                      uint32_t prio);
pgw_err pgw_fuse_write(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                       uint64_t off, const void *buf, size_t len,
                       size_t *out_written, uint32_t prio);
pgw_err pgw_fuse_readdir(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                         uint64_t off, pgw_fuse_dirent *out, size_t max_entries,
                         size_t *out_count, uint32_t prio);
pgw_err pgw_fuse_mkdir(pgw_fuse_session *s, uint64_t parent, const char *name,
                       uint32_t mode, pgw_fuse_entry *out, uint32_t prio);
pgw_err pgw_fuse_unlink(pgw_fuse_session *s, uint64_t parent, const char *name,
                        bool is_dir, uint32_t prio);
pgw_err pgw_fuse_rename(pgw_fuse_session *s, uint64_t olddir, const char *oldname,
                        uint64_t newdir, const char *newname, uint32_t prio);
pgw_err pgw_fuse_symlink(pgw_fuse_session *s, uint64_t parent, const char *name,
                         const char *target, pgw_fuse_entry *out, uint32_t prio);
pgw_err pgw_fuse_link(pgw_fuse_session *s, uint64_t parent, uint64_t oldnodeid,
                      const char *name, pgw_fuse_entry *out, uint32_t prio);
pgw_err pgw_fuse_readlink(pgw_fuse_session *s, uint64_t nodeid, char *target,
                          size_t cap, uint32_t prio);
pgw_err pgw_fuse_fsync(pgw_fuse_session *s, uint64_t nodeid, uint64_t fh,
                       bool is_dir, uint32_t prio);
pgw_err pgw_fuse_statfs(pgw_fuse_session *s, uint64_t nodeid,
                        pgw_fuse_kstatfs *out, uint32_t prio);

#ifdef __cplusplus
}
#endif

#endif /* PGW_FUSEP_H */
