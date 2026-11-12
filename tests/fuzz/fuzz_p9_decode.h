/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fuzz/fuzz_p9_decode.h - the 9P decoders the campaigns and the corpus
 * replay push their inputs through.
 *
 * The campaigns of this directory (ROB-06, P9-09) and the replay of the kept
 * corpus must exercise exactly the same code, so the decoders live here
 * instead of being written twice.
 */
#ifndef FUZZ_P9_DECODE_H
#define FUZZ_P9_DECODE_H

#include "fuzz_decode.h"

#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/p9.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- the 9P decoders -------------------------------------------------------- */

/* True when the type of the header is one the decoders know. */
static inline bool fuzz_p9_known_type(uint8_t type)
{
    switch (type) {
    case PGW_P9_RLERROR:
    case PGW_P9_RVERSION:
    case PGW_P9_RATTACH:
    case PGW_P9_RWALK:
    case PGW_P9_RLOPEN:
    case PGW_P9_RREAD:
    case PGW_P9_RWRITE:
    case PGW_P9_RGETATTR:
    case PGW_P9_RREADLINK:
    case PGW_P9_RMKDIR:
    case PGW_P9_RSTATFS:
        return true;
    default:
        return false;
    }
}

/* Decode the message with the decoder of its own type. */
static inline bool fuzz_p9_decode_typed(const uint8_t *buf, size_t len, uint16_t tag)
{
    switch (buf[4]) {
    case PGW_P9_RLERROR: {
        uint32_t err = 0;
        return pgw_p9_dec_rlerror(buf, len, tag, &err) == PGW_OK;
    }
    case PGW_P9_RVERSION: {
        uint32_t msize = 0;
        char ver[32];
        return pgw_p9_dec_rversion(buf, len, tag, &msize, ver, sizeof ver) == PGW_OK;
    }
    case PGW_P9_RATTACH: {
        pgw_p9_qid qid;
        return pgw_p9_dec_rattach(buf, len, tag, &qid) == PGW_OK;
    }
    case PGW_P9_RWALK: {
        uint16_t nwqid = 0;
        pgw_p9_qid qids[PGW_MAXWELEM];
        return pgw_p9_dec_rwalk(buf, len, tag, &nwqid, qids, PGW_MAXWELEM) == PGW_OK;
    }
    case PGW_P9_RLOPEN: {
        pgw_p9_qid qid;
        uint32_t iounit = 0;
        return pgw_p9_dec_rlopen(buf, len, tag, &qid, &iounit) == PGW_OK;
    }
    case PGW_P9_RREAD: {
        uint32_t count = 0;
        const uint8_t *data = NULL;
        return pgw_p9_dec_rread(buf, len, tag, &count, &data) == PGW_OK;
    }
    case PGW_P9_RWRITE: {
        uint32_t count = 0;
        return pgw_p9_dec_rwrite(buf, len, tag, &count) == PGW_OK;
    }
    case PGW_P9_RGETATTR: {
        pgw_p9_attr attr;
        return pgw_p9_dec_rgetattr(buf, len, tag, &attr) == PGW_OK;
    }
    case PGW_P9_RREADLINK: {
        char target[256];
        return pgw_p9_dec_rreadlink(buf, len, tag, target, sizeof target) == PGW_OK;
    }
    case PGW_P9_RMKDIR: {
        pgw_p9_qid qid;
        return pgw_p9_dec_rmkdir(buf, len, tag, &qid) == PGW_OK;
    }
    case PGW_P9_RSTATFS: {
        pgw_p9_statfs_t st;
        return pgw_p9_dec_rstatfs(buf, len, tag, &st) == PGW_OK;
    }
    default:
        return false;
    }
}

/* Run every 9P decoder over the buffer. The return values are irrelevant
 * except for the depth they add up to; what matters is that nothing reads
 * out of bounds and nothing loops. */
static inline int fuzz_decode_p9(const uint8_t *buf, size_t len, uint16_t tag)
{
    pgw_p9_hdr hdr;
    uint32_t ecode = 0;
    uint32_t msize = 0;
    uint32_t iounit = 0;
    uint32_t count = 0;
    char str[256];
    char ver[32];
    pgw_p9_qid qid;
    pgw_p9_qid wqids[PGW_MAXWELEM];
    pgw_p9_attr attr;
    pgw_p9_statfs_t sfs;
    pgw_p9_dirent ents[8];
    const uint8_t *data = NULL;
    uint16_t nwqid = 0;
    size_t ndirents = 0;

    (void)pgw_p9_dec_hdr(buf, len, &hdr);
    (void)pgw_p9_dec_rlerror(buf, len, tag, &ecode);
    (void)pgw_p9_dec_rversion(buf, len, tag, &msize, ver, sizeof(ver));
    (void)pgw_p9_dec_rattach(buf, len, tag, &qid);
    (void)pgw_p9_dec_rwalk(buf, len, tag, &nwqid, wqids, PGW_MAXWELEM);
    (void)pgw_p9_dec_rlopen(buf, len, tag, &qid, &iounit);
    (void)pgw_p9_dec_rread(buf, len, tag, &count, &data);
    (void)pgw_p9_dec_rwrite(buf, len, tag, &count);
    (void)pgw_p9_dec_rgetattr(buf, len, tag, &attr);
    (void)pgw_p9_dec_rreaddir_iter(buf, len, tag, ents, 8, &ndirents);
    (void)pgw_p9_dec_rreadlink(buf, len, tag, str, sizeof(str));
    (void)pgw_p9_dec_rmkdir(buf, len, tag, &qid);
    (void)pgw_p9_dec_rstatfs(buf, len, tag, &sfs);
    (void)pgw_p9_dec_empty(buf, len, PGW_P9_RCLUNK, tag);

    if (pgw_p9_dec_hdr(buf, len, &hdr) != PGW_OK)
        return FUZZ_DEPTH_NONE;
    if (!fuzz_p9_known_type(hdr.type))
        return FUZZ_DEPTH_HDR;
    return fuzz_p9_decode_typed(buf, len, tag) ? FUZZ_DEPTH_FULL : FUZZ_DEPTH_BODY;
}

/* A decoder that accepts a header must report a size inside the buffer. */
static inline bool fuzz_p9_header_ok(const uint8_t *buf, size_t len)
{
    pgw_p9_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (pgw_p9_dec_hdr(buf, len, &hdr) != PGW_OK)
        return true; /* rejected: nothing to check */
    if (hdr.size < PGW_HEADER_SIZE)
        return false;
    return (size_t)hdr.size <= len;
}

#endif /* FUZZ_P9_DECODE_H */
