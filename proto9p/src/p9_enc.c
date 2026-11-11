/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/src/p9_enc.c - encoders for every T-message of 9P2000.L.
 *
 * Covers P9-02 (attach, walk, open, create, read, write, clunk, getattr,
 * readdir, mkdir, unlinkat, renameat, fsync, statfs), P9-03 (setattr,
 * symlink, readlink, link) and P9-01 (version).
 *
 * Field order is the normative one from the 9P2000.L description: every
 * message is size[4] type[1] tag[2] followed by the documented fields, all
 * little-endian. Nothing is cast onto the caller's buffer: the bytes are
 * written one field at a time through pgw_out, whose sticky failure flag is
 * checked exactly once, in finish(). A message that does not fit returns a
 * negative pgw_err instead of a truncated message (P9-09, ROB-06).
 */
#include "p9_priv.h"

#include "pgw/buf.h"
#include "pgw/limits.h"

#include <string.h>

/* Reserve size[4] then write type[1] tag[2]. */
static pgw_err begin(pgw_out *w, void *buf, size_t cap, uint8_t type,
                     uint16_t tag)
{
    if (buf == NULL || cap < PGW_P9_HDR_LEN)
        return PGW_EINVAL;
    pgw_out_init(w, buf, cap);
    pgw_put_u32(w, 0); /* patched by finish() */
    pgw_put_u8(w, type);
    pgw_put_u16(w, tag);
    return pgw_out_ok(w) ? PGW_OK : PGW_EINVAL;
}

/* Patch size[4] and return the message length, or a negative pgw_err. */
static int32_t finish(pgw_out *w)
{
    size_t n = pgw_written(w);
    if (!pgw_out_ok(w) || n > (size_t)INT32_MAX || !pgw_patch_len_u32(w, 0))
        return -(int32_t)PGW_EMSGSIZE;
    return (int32_t)n;
}

/* Length-prefixed string with an upper bound on its length. */
static bool put_str_lim(pgw_out *w, const char *s, size_t max)
{
    if (s == NULL)
        return false;
    size_t len = strlen(s);
    if (len > max)
        return false;
    return pgw_put_str(w, s, len);
}

int32_t pgw_p9_enc_tversion(void *buf, size_t cap, uint16_t tag,
                            uint32_t msize, const char *version)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TVERSION, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, msize);
    if (!put_str_lim(&w, version, PGW_NAME_MAX))
        return -(int32_t)PGW_EINVAL;
    return finish(&w);
}

int32_t pgw_p9_enc_tattach(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                           uint32_t afid, const char *uname, const char *aname,
                           uint32_t n_uname)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TATTACH, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u32(&w, afid);
    if (!put_str_lim(&w, uname, PGW_NAME_MAX) ||
        !put_str_lim(&w, aname, PGW_PATH_MAX))
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, n_uname);
    return finish(&w);
}

int32_t pgw_p9_enc_twalk(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                         uint32_t newfid, uint16_t nwname,
                         const char *const *wnames)
{
    if (nwname > (uint16_t)PGW_MAXWELEM)
        return -(int32_t)PGW_EINVAL;
    if (nwname > 0 && wnames == NULL)
        return -(int32_t)PGW_EINVAL;
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TWALK, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u32(&w, newfid);
    pgw_put_u16(&w, nwname);
    for (uint16_t i = 0; i < nwname; i++) {
        if (!put_str_lim(&w, wnames[i], PGW_NAME_MAX))
            return -(int32_t)PGW_EINVAL;
    }
    return finish(&w);
}

int32_t pgw_p9_enc_tlopen(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                          uint32_t linux_flags)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TLOPEN, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u32(&w, linux_flags);
    return finish(&w);
}

int32_t pgw_p9_enc_tlcreate(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                            const char *name, uint32_t linux_flags,
                            uint32_t mode, uint32_t gid)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TLCREATE, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    if (!put_str_lim(&w, name, PGW_NAME_MAX))
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, linux_flags);
    pgw_put_u32(&w, mode);
    pgw_put_u32(&w, gid);
    return finish(&w);
}

/* Tread and Treaddir share fid[4] offset[8] count[4]. */
static int32_t enc_fid_off_count(void *buf, size_t cap, uint8_t type,
                                 uint16_t tag, uint32_t fid, uint64_t offset,
                                 uint32_t count)
{
    pgw_out w;
    if (begin(&w, buf, cap, type, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u64(&w, offset);
    pgw_put_u32(&w, count);
    return finish(&w);
}

int32_t pgw_p9_enc_tread(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                         uint64_t offset, uint32_t count)
{
    return enc_fid_off_count(buf, cap, PGW_P9_TREAD, tag, fid, offset, count);
}

int32_t pgw_p9_enc_treaddir(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                            uint64_t offset, uint32_t count)
{
    return enc_fid_off_count(buf, cap, PGW_P9_TREADDIR, tag, fid, offset,
                             count);
}

int32_t pgw_p9_enc_twrite(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                          uint64_t offset, const void *data, uint32_t count)
{
    if (count > 0 && data == NULL)
        return -(int32_t)PGW_EINVAL;
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TWRITE, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u64(&w, offset);
    pgw_put_u32(&w, count);
    if (count > 0 && pgw_put_data(&w, data, count) != (size_t)count)
        return -(int32_t)PGW_EMSGSIZE;
    return finish(&w);
}

/* Tclunk, Treadlink and Tstatfs carry only fid[4]. */
static int32_t enc_fid_only(void *buf, size_t cap, uint8_t type, uint16_t tag,
                            uint32_t fid)
{
    pgw_out w;
    if (begin(&w, buf, cap, type, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    return finish(&w);
}

int32_t pgw_p9_enc_tclunk(void *buf, size_t cap, uint16_t tag, uint32_t fid)
{
    return enc_fid_only(buf, cap, PGW_P9_TCLUNK, tag, fid);
}

int32_t pgw_p9_enc_treadlink(void *buf, size_t cap, uint16_t tag, uint32_t fid)
{
    return enc_fid_only(buf, cap, PGW_P9_TREADLINK, tag, fid);
}

int32_t pgw_p9_enc_tstatfs(void *buf, size_t cap, uint16_t tag, uint32_t fid)
{
    return enc_fid_only(buf, cap, PGW_P9_TSTATFS, tag, fid);
}

int32_t pgw_p9_enc_tflush(void *buf, size_t cap, uint16_t tag, uint16_t oldtag)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TFLUSH, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u16(&w, oldtag);
    return finish(&w);
}

int32_t pgw_p9_enc_tgetattr(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                            uint64_t request_mask)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TGETATTR, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u64(&w, request_mask);
    return finish(&w);
}

int32_t pgw_p9_enc_tsetattr(void *buf, size_t cap, uint16_t tag, uint32_t fid,
                            const pgw_p9_setattr_t *sa)
{
    if (sa == NULL)
        return -(int32_t)PGW_EINVAL;
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TSETATTR, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u32(&w, sa->valid);
    pgw_put_u32(&w, sa->mode);
    pgw_put_u32(&w, sa->uid);
    pgw_put_u32(&w, sa->gid);
    pgw_put_u64(&w, sa->size);
    pgw_put_u64(&w, sa->atime_sec);
    pgw_put_u64(&w, sa->atime_nsec);
    pgw_put_u64(&w, sa->mtime_sec);
    pgw_put_u64(&w, sa->mtime_nsec);
    return finish(&w);
}

int32_t pgw_p9_enc_tmkdir(void *buf, size_t cap, uint16_t tag, uint32_t dfid,
                          const char *name, uint32_t mode, uint32_t gid)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TMKDIR, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, dfid);
    if (!put_str_lim(&w, name, PGW_NAME_MAX))
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, mode);
    pgw_put_u32(&w, gid);
    return finish(&w);
}

int32_t pgw_p9_enc_tunlinkat(void *buf, size_t cap, uint16_t tag, uint32_t dfid,
                             const char *name, uint32_t flags)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TUNLINKAT, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, dfid);
    if (!put_str_lim(&w, name, PGW_NAME_MAX))
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, flags);
    return finish(&w);
}

int32_t pgw_p9_enc_trenameat(void *buf, size_t cap, uint16_t tag,
                             uint32_t olddfid, const char *oldname,
                             uint32_t newdfid, const char *newname)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TRENAMEAT, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, olddfid);
    if (!put_str_lim(&w, oldname, PGW_NAME_MAX))
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, newdfid);
    if (!put_str_lim(&w, newname, PGW_NAME_MAX))
        return -(int32_t)PGW_EINVAL;
    return finish(&w);
}

int32_t pgw_p9_enc_tsymlink(void *buf, size_t cap, uint16_t tag, uint32_t dfid,
                            const char *name, const char *symtgt, uint32_t gid)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TSYMLINK, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, dfid);
    if (!put_str_lim(&w, name, PGW_NAME_MAX) ||
        !put_str_lim(&w, symtgt, PGW_PATH_MAX))
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, gid);
    return finish(&w);
}

int32_t pgw_p9_enc_tlink(void *buf, size_t cap, uint16_t tag, uint32_t dfid,
                         uint32_t fid, const char *name)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TLINK, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, dfid);
    pgw_put_u32(&w, fid);
    if (!put_str_lim(&w, name, PGW_NAME_MAX))
        return -(int32_t)PGW_EINVAL;
    return finish(&w);
}

int32_t pgw_p9_enc_tfsync(void *buf, size_t cap, uint16_t tag, uint32_t fid)
{
    pgw_out w;
    if (begin(&w, buf, cap, PGW_P9_TFSYNC, tag) != PGW_OK)
        return -(int32_t)PGW_EINVAL;
    pgw_put_u32(&w, fid);
    pgw_put_u32(&w, 0); /* datasync: we never ask for a partial flush */
    return finish(&w);
}
