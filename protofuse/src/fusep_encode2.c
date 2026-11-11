/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/src/fusep_encode2.c - Request encoders, part 2: data path and
 * namespace mutations (RELEASE, READ, WRITE, MKDIR, UNLINK, RMDIR, RENAME,
 * SYMLINK, LINK, READLINK, FSYNC, STATFS, INTERRUPT).
 *
 * Implements requirements:
 *   - VFS-02: INTERRUPT is encoded here so that the session can put it on the
 *     priority lane
 *   - VFS-03: file I/O, directory, link and synchronisation requests
 *   - ROB-06 / ROB-07: serialisation through pgw_out only
 */
#include "fusep_internal.h"

#include "pgw/buf.h"
#include "pgw/fusep.h"

int32_t pgw_fuse_enc_release(void *buf, size_t cap, uint64_t unique,
                             uint64_t nodeid, uint64_t fh, bool is_dir)
{
    uint32_t op =
        is_dir ? (uint32_t)PGW_FUSE_RELEASEDIR : (uint32_t)PGW_FUSE_RELEASE;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, op, unique, nodeid);
    pgw_put_u64(&w, fh);
    pgw_put_u32(&w, 0); /* flags of the original open */
    pgw_put_u32(&w, 0); /* release_flags */
    pgw_put_u64(&w, 0); /* lock_owner */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_read(void *buf, size_t cap, uint64_t unique,
                          uint64_t nodeid, uint64_t fh, uint64_t off,
                          uint32_t size, bool is_dir)
{
    uint32_t op = is_dir ? (uint32_t)PGW_FUSE_READDIR : (uint32_t)PGW_FUSE_READ;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, op, unique, nodeid);
    pgw_put_u64(&w, fh);
    pgw_put_u64(&w, off);
    pgw_put_u32(&w, size);
    pgw_put_u32(&w, 0); /* read_flags */
    pgw_put_u64(&w, 0); /* lock_owner */
    pgw_put_u32(&w, 0); /* flags of the original open */
    pgw_put_u32(&w, 0); /* padding */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_write(void *buf, size_t cap, uint64_t unique,
                           uint64_t nodeid, uint64_t fh, uint64_t off,
                           const void *data, uint32_t size)
{
    if (size > 0 && data == NULL)
        return -1;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_WRITE, unique, nodeid);
    pgw_put_u64(&w, fh);
    pgw_put_u64(&w, off);
    pgw_put_u32(&w, size);
    pgw_put_u32(&w, 0); /* write_flags */
    pgw_put_u64(&w, 0); /* lock_owner */
    pgw_put_u32(&w, 0); /* flags of the original open */
    pgw_put_u32(&w, 0); /* padding */
    if (size > 0)
        pgw_put_data(&w, data, size); /* payload follows the header directly */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_mkdir(void *buf, size_t cap, uint64_t unique,
                           uint64_t parent, const char *name,
                           uint32_t mode, uint32_t umask)
{
    size_t nlen = pgw_fusep_name_wire_len(name);
    if (nlen == 0)
        return -1;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_MKDIR, unique, parent);
    pgw_put_u32(&w, mode);
    pgw_put_u32(&w, umask);
    pgw_put_data(&w, name, nlen);
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_name_op(void *buf, size_t cap, uint32_t opcode,
                             uint64_t unique, uint64_t parent, const char *name)
{
    size_t nlen = pgw_fusep_name_wire_len(name);
    if (nlen == 0)
        return -1;
    if (opcode != (uint32_t)PGW_FUSE_UNLINK && opcode != (uint32_t)PGW_FUSE_RMDIR)
        return -1; /* this encoder only covers the name-only mutations */
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, opcode, unique, parent);
    pgw_put_data(&w, name, nlen);
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_rename(void *buf, size_t cap, uint64_t unique,
                            uint64_t olddir, const char *oldname,
                            uint64_t newdir, const char *newname, uint32_t flags)
{
    size_t olen = pgw_fusep_name_wire_len(oldname);
    size_t nlen = pgw_fusep_name_wire_len(newname);
    if (olen == 0 || nlen == 0)
        return -1;
    pgw_out w;
    if (flags != 0) {
        /* RENAME2 carries the flags; it only exists from protocol 7.23 on, so
         * the session refuses it when the server announced a lower minor. */
        pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_RENAME2, unique, olddir);
        pgw_put_u64(&w, newdir);
        pgw_put_u32(&w, flags);
        pgw_put_u32(&w, 0); /* padding */
    } else {
        pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_RENAME, unique, olddir);
        pgw_put_u64(&w, newdir);
    }
    pgw_put_data(&w, oldname, olen);
    pgw_put_data(&w, newname, nlen);
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_symlink(void *buf, size_t cap, uint64_t unique,
                             uint64_t parent, const char *name,
                             const char *target)
{
    size_t nlen = pgw_fusep_name_wire_len(name);
    size_t tlen = pgw_fusep_name_wire_len(target);
    if (nlen == 0 || tlen == 0)
        return -1;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_SYMLINK, unique, parent);
    pgw_put_data(&w, name, nlen);
    pgw_put_data(&w, target, tlen); /* target follows the name, also NUL-ended */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_link(void *buf, size_t cap, uint64_t unique,
                          uint64_t parent, uint64_t oldnodeid, const char *name)
{
    size_t nlen = pgw_fusep_name_wire_len(name);
    if (nlen == 0)
        return -1;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_LINK, unique, parent);
    pgw_put_u64(&w, oldnodeid);
    pgw_put_data(&w, name, nlen);
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_readlink(void *buf, size_t cap, uint64_t unique,
                              uint64_t nodeid)
{
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_READLINK, unique, nodeid);
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_fsync(void *buf, size_t cap, uint64_t unique,
                           uint64_t nodeid, uint64_t fh, bool datasync,
                           bool is_dir)
{
    uint32_t op =
        is_dir ? (uint32_t)PGW_FUSE_FSYNCDIR : (uint32_t)PGW_FUSE_FSYNC;
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, op, unique, nodeid);
    pgw_put_u64(&w, fh);
    pgw_put_u32(&w, datasync ? PGW_FUSE_FSYNC_FDATASYNC_BIT : 0u);
    pgw_put_u32(&w, 0); /* padding */
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_statfs(void *buf, size_t cap, uint64_t unique,
                            uint64_t nodeid)
{
    pgw_out w;
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_STATFS, unique, nodeid);
    return pgw_fusep_enc_end(&w);
}

int32_t pgw_fuse_enc_interrupt(void *buf, size_t cap, uint64_t unique,
                               uint64_t target_unique)
{
    pgw_out w;
    /* nodeid is unused for INTERRUPT: the target is carried in the body. */
    pgw_fusep_enc_begin(&w, buf, cap, PGW_FUSE_INTERRUPT, unique, 0);
    pgw_put_u64(&w, target_unique);
    return pgw_fusep_enc_end(&w);
}
