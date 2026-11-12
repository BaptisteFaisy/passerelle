/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_fuse_decode.h - the FUSE decoders the campaigns and the
 * corpus replay push their inputs through.
 *
 * Same rationale as fuzz_p9_decode.h: the code is written once and used by
 * the campaigns and by the replay of the kept corpus.
 */
#ifndef FUZZ_FUSE_DECODE_H
#define FUZZ_FUSE_DECODE_H

#include "fuzz_decode.h"

#include "pgw/err.h"
#include "pgw/fusep.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FUZZ_FUSE_HDR_MIN 16u

/* --- the FUSE decoders ------------------------------------------------------ */

/* Run every FUSE decoder over the buffer and return the depth it reached. */
static inline int fuzz_decode_fuse(const uint8_t *buf, size_t len, uint64_t unique)
{
    pgw_fuse_out_hdr hdr;
    pgw_fuse_init_info init;
    pgw_fuse_entry entry;
    pgw_fuse_attr attr;
    pgw_fuse_kstatfs sfs;
    pgw_fuse_dirent ents[8];
    uint64_t fh = 0;
    uint32_t open_flags = 0;
    uint32_t written = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    size_t ndirents = 0;
    char target[256];
    bool full = false;

    (void)pgw_fuse_dec_hdr(buf, len, &hdr);
    full |= pgw_fuse_dec_init(buf, len, unique, &init) == PGW_OK;
    full |= pgw_fuse_dec_entry(buf, len, unique, &entry) == PGW_OK;
    full |= pgw_fuse_dec_attr(buf, len, unique, &attr) == PGW_OK;
    full |= pgw_fuse_dec_open(buf, len, unique, &fh, &open_flags) == PGW_OK;
    full |= pgw_fuse_dec_create(buf, len, unique, &entry, &fh) == PGW_OK;
    full |= pgw_fuse_dec_read(buf, len, unique, &data, &data_len) == PGW_OK;
    full |= pgw_fuse_dec_write(buf, len, unique, &written) == PGW_OK;
    full |= pgw_fuse_dec_readdir(buf, len, unique, ents, 8, &ndirents) == PGW_OK;
    full |= pgw_fuse_dec_readlink(buf, len, unique, target, sizeof(target)) == PGW_OK;
    full |= pgw_fuse_dec_statfs(buf, len, unique, &sfs) == PGW_OK;
    full |= pgw_fuse_dec_empty(buf, len, unique) == PGW_OK;

    if (pgw_fuse_dec_hdr(buf, len, &hdr) != PGW_OK)
        return FUZZ_DEPTH_NONE;
    return full ? FUZZ_DEPTH_FULL : FUZZ_DEPTH_BODY;
}

/* A decoder that accepts a header must report a length inside the buffer and
 * a non-positive error (FUSE carries -errno, never a positive code). */
static inline bool fuzz_fuse_header_ok(const uint8_t *buf, size_t len)
{
    pgw_fuse_out_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (pgw_fuse_dec_hdr(buf, len, &hdr) != PGW_OK)
        return true;
    if (hdr.len < FUZZ_FUSE_HDR_MIN)
        return false;
    if ((size_t)hdr.len > len)
        return false;
    return hdr.error <= 0;
}

#endif /* FUZZ_FUSE_DECODE_H */
