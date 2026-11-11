/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_encode.c - Request encoders, part 1: INIT and the
 * metadata operations (LOOKUP, FORGET, GETATTR, SETATTR, OPEN, CREATE).
 *
 * Implements requirements:
 *   - VFS-03: FUSE 7.x request encoding for negotiation and metadata
 *   - ROB-06 / ROB-07: every encoder writes through pgw_out (little-endian,
 *     sticky failure, no struct ever cast onto a buffer)
 *
 * Every encoder returns the message length in bytes (header included) or -1
 * when the destination is too small or an argument is invalid.
 */
#include "fusep_internal.h"

#include "pgw/buf.h"
#include "pgw/fusep.h"

int32_t pgw_fuse_enc_init(void *buf, size_t cap, uint64_t unique,
                          uint32_t max_readahead, uint64_t flags)
{
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_INIT, unique, 0);

    pgw_put_u32(&w, PGW_FUSE_MAJOR);
    pgw_put_u32(&w, PGW_FUSE_MINOR);
    pgw_put_u32(&w, max_readahead);
    pgw_put_u32(&w, (uint32_t)(flags & 0xFFFFFFFFu)); /* flags  */
    pgw_put_u32(&w, (uint32_t)(flags >> 32));         /* flags2 */
    pgw_put_pad(&w, 11u * sizeof(uint32_t));          /* unused[11] */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_lookup(void *buf, size_t cap, uint64_t unique,
                            uint64_t parent, const char *name)
{
    size_t nlen = pgw_fusep_name_wire_len(name);
    if (nlen == 0)
        return -1;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_LOOKUP, unique, parent);
    pgw_put_data(&w, name, nlen); /* name + NUL, no length prefix */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_forget(void *buf, size_t cap, uint64_t unique,
                            uint64_t nodeid, uint64_t nlookup)
{
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_FORGET, unique, nodeid);
    pgw_put_u64(&w, nlookup);
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_getattr(void *buf, size_t cap, uint64_t unique,
                             uint64_t nodeid, uint64_t fh, bool has_fh)
{
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_GETATTR, unique, nodeid);
    pgw_put_u32(&w, has_fh ? PGW_FUSE_GETATTR_FH_BIT : 0u);
    pgw_put_u32(&w, 0);                 /* dummy */
    pgw_put_u64(&w, has_fh ? fh : 0ull); /* fh, only meaningful with the bit */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_setattr(void *buf, size_t cap, uint64_t unique,
                             uint64_t nodeid, const pgw_fuse_setattr_in *sa)
{
    if (sa == NULL)
        return -1;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_SETATTR, unique, nodeid);
    pgw_put_u32(&w, sa->valid);
    pgw_put_u32(&w, 0);  /* padding */
    pgw_put_u64(&w, 0);  /* fh (FATTR_FH is never set by this layer) */
    pgw_put_u64(&w, sa->size);
    pgw_put_u64(&w, 0);  /* lock_owner */
    pgw_put_u64(&w, sa->atime);
    pgw_put_u64(&w, sa->mtime);
    pgw_put_u64(&w, 0);  /* ctime */
    pgw_put_u32(&w, sa->atime_nsec);
    pgw_put_u32(&w, sa->mtime_nsec);
    pgw_put_u32(&w, 0);  /* ctimensec */
    pgw_put_u32(&w, sa->mode);
    pgw_put_u32(&w, 0);  /* unused4 */
    pgw_put_u32(&w, sa->uid);
    pgw_put_u32(&w, sa->gid);
    pgw_put_u32(&w, 0);  /* unused5 */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_open(void *buf, size_t cap, uint64_t unique,
                          uint64_t nodeid, uint32_t linux_flags, bool is_dir)
{
    uint32_t op = is_dir ? (uint32_t)PGW_FUSE_OPENDIR : (uint32_t)PGW_FUSE_OPEN;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, op, unique, nodeid);
    pgw_put_u32(&w, linux_flags); /* already in Linux numbering (wire) */
    pgw_put_u32(&w, 0);           /* open_flags: no FUSE_OPEN_* requested */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_create(void *buf, size_t cap, uint64_t unique,
                            uint64_t parent, const char *name,
                            uint32_t linux_flags, uint32_t mode, uint32_t umask)
{
    size_t nlen = pgw_fusep_name_wire_len(name);
    if (nlen == 0)
        return -1;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_CREATE, unique, parent);
    pgw_put_u32(&w, linux_flags);
    pgw_put_u32(&w, mode);  /* Linux mode bits, type included */
    pgw_put_u32(&w, umask);
    pgw_put_u32(&w, 0);     /* open_flags */
    pgw_put_data(&w, name, nlen);
    return pgw_fusep_enc_end(&w);
}
