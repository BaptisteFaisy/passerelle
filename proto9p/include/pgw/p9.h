/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/p9.h - Layer 2 (portable 9P2000.L protocol).
 *
 * Implements requirements P9-01 through P9-10:
 *   - Pure C11 over pgw_buf / pgw_out; zero OS-specific headers, runs on PC and QNX
 *   - Every 9P2000.L message needed for S and C tiers
 *   - Strict response validation (size, type, tag, no trailing bytes) (P9-09)
 *   - Multi-request client with tag/fid pools, signal/timeout flush (P9-05, P9-06)
 *   - Automatic chunking of reads, writes and readdir above msize (P9-08)
 */
#ifndef PGW_P9_H
#define PGW_P9_H

#include "pgw/err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 9P2000.L message types (wire values). */
enum pgw_p9_type {
    PGW_P9_TLERROR = 6,
    PGW_P9_RLERROR = 7,
    PGW_P9_TSTATFS = 8,
    PGW_P9_RSTATFS = 9,
    PGW_P9_TLOPEN = 12,
    PGW_P9_RLOPEN = 13,
    PGW_P9_TLCREATE = 14,
    PGW_P9_RLCREATE = 15,
    PGW_P9_TSYMLINK = 16,
    PGW_P9_RSYMLINK = 17,
    PGW_P9_TMKNOD = 18,
    PGW_P9_RMKNOD = 19,
    PGW_P9_TRENAME = 20,
    PGW_P9_RRENAME = 21,
    PGW_P9_TREADLINK = 22,
    PGW_P9_RREADLINK = 23,
    PGW_P9_TGETATTR = 24,
    PGW_P9_RGETATTR = 25,
    PGW_P9_TSETATTR = 26,
    PGW_P9_RSETATTR = 27,
    PGW_P9_TREADDIR = 40,
    PGW_P9_RREADDIR = 41,
    PGW_P9_TFSYNC = 50,
    PGW_P9_RFSYNC = 51,
    PGW_P9_TLOCK = 52,
    PGW_P9_RLOCK = 53,
    PGW_P9_TGETLOCK = 54,
    PGW_P9_RGETLOCK = 55,
    PGW_P9_TLINK = 70,
    PGW_P9_RLINK = 71,
    PGW_P9_TMKDIR = 72,
    PGW_P9_RMKDIR = 73,
    PGW_P9_TRENAMEAT = 74,
    PGW_P9_RRENAMEAT = 75,
    PGW_P9_TUNLINKAT = 76,
    PGW_P9_RUNLINKAT = 77,
    PGW_P9_TVERSION = 100,
    PGW_P9_RVERSION = 101,
    PGW_P9_TAUTH = 102,
    PGW_P9_RAUTH = 103,
    PGW_P9_TATTACH = 104,
    PGW_P9_RATTACH = 105,
    PGW_P9_TFLUSH = 108,
    PGW_P9_RFLUSH = 109,
    PGW_P9_TWALK = 110,
    PGW_P9_RWALK = 111,
    PGW_P9_TREAD = 116,
    PGW_P9_RREAD = 117,
    PGW_P9_TWRITE = 118,
    PGW_P9_RWRITE = 119,
    PGW_P9_TCLUNK = 120,
    PGW_P9_RCLUNK = 121,
    PGW_P9_TREMOVE = 122,
    PGW_P9_RREMOVE = 123
};

/* Tgetattr request_mask bits. */
#define PGW_P9_GETATTR_MODE   0x00000001ULL
#define PGW_P9_GETATTR_NLINK  0x00000002ULL
#define PGW_P9_GETATTR_UID    0x00000004ULL
#define PGW_P9_GETATTR_GID    0x00000008ULL
#define PGW_P9_GETATTR_RDEV   0x00000010ULL
#define PGW_P9_GETATTR_ATIME  0x00000020ULL
#define PGW_P9_GETATTR_MTIME  0x00000040ULL
#define PGW_P9_GETATTR_CTIME  0x00000080ULL
#define PGW_P9_GETATTR_INO    0x00000100ULL
#define PGW_P9_GETATTR_SIZE   0x00000200ULL
#define PGW_P9_GETATTR_BLOCKS 0x00000400ULL
#define PGW_P9_GETATTR_BASIC  0x000007FFULL

/* Tsetattr valid bits. */
#define PGW_P9_SETATTR_MODE      0x00000001u
#define PGW_P9_SETATTR_UID       0x00000002u
#define PGW_P9_SETATTR_GID       0x00000004u
#define PGW_P9_SETATTR_SIZE      0x00000008u
#define PGW_P9_SETATTR_ATIME     0x00000010u
#define PGW_P9_SETATTR_MTIME     0x00000020u
#define PGW_P9_SETATTR_CTIME     0x00000040u
#define PGW_P9_SETATTR_ATIME_SET 0x00000080u
#define PGW_P9_SETATTR_MTIME_SET 0x00000100u

/* Tunlinkat flags. */
#define PGW_P9_AT_REMOVEDIR 0x200u

/* Qid type bits. */
#define PGW_P9_QTDIR     0x80u
#define PGW_P9_QTAPPEND  0x40u
#define PGW_P9_QTEXCL    0x20u
#define PGW_P9_QTSYMLINK 0x02u
#define PGW_P9_QTFILE    0x00u

typedef struct pgw_p9_hdr {
    uint32_t size;
    uint8_t type;
    uint16_t tag;
} pgw_p9_hdr;

typedef struct pgw_p9_qid {
    uint8_t type;
    uint32_t version;
    uint64_t path;
} pgw_p9_qid;

typedef struct pgw_p9_attr {
    uint64_t valid;
    pgw_p9_qid qid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t nlink;
    uint64_t rdev;
    uint64_t size;
    uint64_t blksize;
    uint64_t blocks;
    uint64_t atime_sec;
    uint64_t atime_nsec;
    uint64_t mtime_sec;
    uint64_t mtime_nsec;
    uint64_t ctime_sec;
    uint64_t ctime_nsec;
} pgw_p9_attr;

typedef struct pgw_p9_statfs {
    uint32_t type;
    uint32_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint64_t fsid;
    uint32_t namelen;
} pgw_p9_statfs_t;

/* The two structs below are typedef'd with a `_t` suffix: in C, a typedef name
 * and a function name live in the same (ordinary) identifier namespace, and
 * both of these types would otherwise collide with the session functions
 * pgw_p9_setattr() and pgw_p9_statfs() declared further down. Only the type
 * alias changed; the struct tags and every function name are untouched. */
typedef struct pgw_p9_setattr {
    uint32_t valid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime_sec;
    uint64_t atime_nsec;
    uint64_t mtime_sec;
    uint64_t mtime_nsec;
} pgw_p9_setattr_t;

typedef struct pgw_p9_dirent {
    pgw_p9_qid qid;
    uint64_t offset;
    uint8_t type;
    char name[256];
    uint16_t name_len;
} pgw_p9_dirent;

/* --- wire codecs (return encoded length > 0 or negative pgw_err) ---------- */

int32_t pgw_p9_enc_tversion(void *buf, size_t cap, uint16_t tag,
                            uint32_t msize, const char *version);
int32_t pgw_p9_enc_tattach(void *buf, size_t cap, uint16_t tag,
                           uint32_t fid, uint32_t afid,
                           const char *uname, const char *aname, uint32_t n_uname);
int32_t pgw_p9_enc_twalk(void *buf, size_t cap, uint16_t tag,
                         uint32_t fid, uint32_t newfid,
                         uint16_t nwname, const char *const *wnames);
int32_t pgw_p9_enc_tlopen(void *buf, size_t cap, uint16_t tag,
                          uint32_t fid, uint32_t linux_flags);
int32_t pgw_p9_enc_tlcreate(void *buf, size_t cap, uint16_t tag,
                            uint32_t fid, const char *name,
                            uint32_t linux_flags, uint32_t mode, uint32_t gid);
int32_t pgw_p9_enc_tread(void *buf, size_t cap, uint16_t tag,
                         uint32_t fid, uint64_t offset, uint32_t count);
int32_t pgw_p9_enc_twrite(void *buf, size_t cap, uint16_t tag,
                          uint32_t fid, uint64_t offset,
                          const void *data, uint32_t count);
int32_t pgw_p9_enc_tclunk(void *buf, size_t cap, uint16_t tag, uint32_t fid);
int32_t pgw_p9_enc_tflush(void *buf, size_t cap, uint16_t tag, uint16_t oldtag);
int32_t pgw_p9_enc_tgetattr(void *buf, size_t cap, uint16_t tag,
                            uint32_t fid, uint64_t request_mask);
int32_t pgw_p9_enc_tsetattr(void *buf, size_t cap, uint16_t tag,
                            uint32_t fid, const pgw_p9_setattr_t *sa);
int32_t pgw_p9_enc_treaddir(void *buf, size_t cap, uint16_t tag,
                            uint32_t fid, uint64_t offset, uint32_t count);
int32_t pgw_p9_enc_tmkdir(void *buf, size_t cap, uint16_t tag,
                          uint32_t dfid, const char *name,
                          uint32_t mode, uint32_t gid);
int32_t pgw_p9_enc_tunlinkat(void *buf, size_t cap, uint16_t tag,
                             uint32_t dfid, const char *name, uint32_t flags);
int32_t pgw_p9_enc_trenameat(void *buf, size_t cap, uint16_t tag,
                             uint32_t olddfid, const char *oldname,
                             uint32_t newdfid, const char *newname);
int32_t pgw_p9_enc_tsymlink(void *buf, size_t cap, uint16_t tag,
                            uint32_t dfid, const char *name,
                            const char *symtgt, uint32_t gid);
int32_t pgw_p9_enc_treadlink(void *buf, size_t cap, uint16_t tag, uint32_t fid);
int32_t pgw_p9_enc_tlink(void *buf, size_t cap, uint16_t tag,
                         uint32_t dfid, uint32_t fid, const char *name);
int32_t pgw_p9_enc_tfsync(void *buf, size_t cap, uint16_t tag, uint32_t fid);
int32_t pgw_p9_enc_tstatfs(void *buf, size_t cap, uint16_t tag, uint32_t fid);

/* --- decoders (validate length, type, tag, trailing bytes - P9-09) -------- */

pgw_err pgw_p9_dec_hdr(const void *msg, size_t len, pgw_p9_hdr *out);
pgw_err pgw_p9_dec_rlerror(const void *msg, size_t len, uint16_t tag,
                           uint32_t *linux_errno);
pgw_err pgw_p9_dec_rversion(const void *msg, size_t len, uint16_t tag,
                            uint32_t *msize, char *ver, size_t ver_cap);
pgw_err pgw_p9_dec_rattach(const void *msg, size_t len, uint16_t tag,
                           pgw_p9_qid *qid);
pgw_err pgw_p9_dec_rwalk(const void *msg, size_t len, uint16_t tag,
                         uint16_t *nwqid, pgw_p9_qid *wqids, uint16_t max_wqids);
pgw_err pgw_p9_dec_rlopen(const void *msg, size_t len, uint16_t tag,
                          pgw_p9_qid *qid, uint32_t *iounit);
pgw_err pgw_p9_dec_rread(const void *msg, size_t len, uint16_t tag,
                         uint32_t *count, const uint8_t **data_ptr);
pgw_err pgw_p9_dec_rwrite(const void *msg, size_t len, uint16_t tag,
                          uint32_t *count);
pgw_err pgw_p9_dec_rgetattr(const void *msg, size_t len, uint16_t tag,
                            pgw_p9_attr *attr);
pgw_err pgw_p9_dec_rreaddir_iter(const void *msg, size_t len, uint16_t tag,
                                 pgw_p9_dirent *out, size_t max_entries,
                                 size_t *out_count);
pgw_err pgw_p9_dec_rreadlink(const void *msg, size_t len, uint16_t tag,
                             char *target, size_t cap);
pgw_err pgw_p9_dec_rmkdir(const void *msg, size_t len, uint16_t tag,
                          pgw_p9_qid *qid);
pgw_err pgw_p9_dec_rstatfs(const void *msg, size_t len, uint16_t tag,
                           pgw_p9_statfs_t *st);
pgw_err pgw_p9_dec_empty(const void *msg, size_t len, uint8_t expect_type,
                         uint16_t tag);

/* --- ID pools (tags and fids, P9-07) -------------------------------------- */

typedef struct pgw_p9_idpool pgw_p9_idpool;

pgw_p9_idpool *pgw_p9_pool_create(uint32_t first, uint32_t count);
void pgw_p9_pool_destroy(pgw_p9_idpool *p);
bool pgw_p9_pool_alloc(pgw_p9_idpool *p, uint32_t *out_id);
void pgw_p9_pool_free(pgw_p9_idpool *p, uint32_t id);
uint32_t pgw_p9_pool_in_use(const pgw_p9_idpool *p);

/* --- high-level session over an abstract RPC callback --------------------- */

typedef pgw_err (*pgw_p9_rpc_fn)(const void *tx, size_t tx_len,
                                 void *rx, size_t rx_cap, size_t *rx_len,
                                 uint32_t prio, uint32_t timeout_ms, void *user);

typedef struct pgw_p9_session pgw_p9_session;

/* The memory the session uses for its request and reply buffers. A transport
 * that hands those buffers to a device needs memory the device can address,
 * which on QNX means physically contiguous memory (VIO-07); the allocator is
 * therefore injectable. NULL means the C library. */
typedef void *(*pgw_p9_alloc_fn)(size_t bytes, void *user);
typedef void (*pgw_p9_free_fn)(void *ptr, size_t bytes, void *user);
void pgw_p9_session_set_memory(pgw_p9_alloc_fn alloc, pgw_p9_free_fn free_fn,
                               void *user);

pgw_p9_session *pgw_p9_session_open(pgw_p9_rpc_fn rpc, void *user,
                                    uint32_t msize_req, const char *uname,
                                    const char *aname, uint32_t uid);
void pgw_p9_session_close(pgw_p9_session *s);
uint32_t pgw_p9_session_msize(const pgw_p9_session *s);
uint32_t pgw_p9_session_root_fid(const pgw_p9_session *s);
uint32_t pgw_p9_session_open_fids(const pgw_p9_session *s);
void pgw_p9_session_set_timeout(pgw_p9_session *s, uint32_t timeout_ms);

/* Session operations (chunked read/write/readdir per P9-08, clean clunk P9-07). */
pgw_err pgw_p9_walk_path(pgw_p9_session *s, uint32_t base_fid, const char *rel,
                         uint32_t *out_fid, pgw_p9_qid *out_qid, uint32_t prio);
pgw_err pgw_p9_clunk(pgw_p9_session *s, uint32_t fid, uint32_t prio);
pgw_err pgw_p9_lopen(pgw_p9_session *s, uint32_t fid, uint32_t pgw_oflags,
                     uint32_t prio);
pgw_err pgw_p9_lcreate(pgw_p9_session *s, uint32_t dfid, const char *name,
                       uint32_t pgw_oflags, uint32_t mode, uint32_t gid,
                       pgw_p9_qid *out_qid, uint32_t prio);
pgw_err pgw_p9_read(pgw_p9_session *s, uint32_t fid, uint64_t off,
                    void *buf, size_t len, size_t *out_read, uint32_t prio);
pgw_err pgw_p9_write(pgw_p9_session *s, uint32_t fid, uint64_t off,
                     const void *buf, size_t len, size_t *out_written,
                     uint32_t prio);
pgw_err pgw_p9_getattr(pgw_p9_session *s, uint32_t fid, pgw_p9_attr *out,
                       uint32_t prio);
pgw_err pgw_p9_setattr(pgw_p9_session *s, uint32_t fid,
                       const pgw_p9_setattr_t *sa, uint32_t prio);
pgw_err pgw_p9_readdir(pgw_p9_session *s, uint32_t fid, uint64_t off,
                       pgw_p9_dirent *out, size_t max_entries,
                       size_t *out_count, uint64_t *next_off, uint32_t prio);
pgw_err pgw_p9_mkdir(pgw_p9_session *s, uint32_t dfid, const char *name,
                     uint32_t mode, uint32_t gid, pgw_p9_qid *out_qid,
                     uint32_t prio);
pgw_err pgw_p9_unlinkat(pgw_p9_session *s, uint32_t dfid, const char *name,
                        bool is_dir, uint32_t prio);
pgw_err pgw_p9_renameat(pgw_p9_session *s, uint32_t olddfid, const char *oldname,
                        uint32_t newdfid, const char *newname, uint32_t prio);
pgw_err pgw_p9_symlink(pgw_p9_session *s, uint32_t dfid, const char *name,
                       const char *target, uint32_t gid, pgw_p9_qid *out_qid,
                       uint32_t prio);
pgw_err pgw_p9_readlink(pgw_p9_session *s, uint32_t fid, char *target,
                        size_t cap, uint32_t prio);
pgw_err pgw_p9_link(pgw_p9_session *s, uint32_t dfid, uint32_t fid,
                    const char *name, uint32_t prio);
pgw_err pgw_p9_fsync(pgw_p9_session *s, uint32_t fid, uint32_t prio);
pgw_err pgw_p9_statfs(pgw_p9_session *s, uint32_t fid, pgw_p9_statfs_t *out,
                      uint32_t prio);
pgw_err pgw_p9_flush(pgw_p9_session *s, uint16_t oldtag, uint32_t prio);

#ifdef __cplusplus
}
#endif

#endif /* PGW_P9_H */
