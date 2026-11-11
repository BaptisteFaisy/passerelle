/* SPDX-License-Identifier: Apache-2.0 */
/*
 * clab_p9.h - chapter 7's exercise: enough 9P2000.L to read the first byte.
 *
 * Every 9P message is `size[4] type[1] tag[2]` followed by its body, all
 * little-endian, strings as `len[2]` plus bytes with no NUL, a qid as 13 bytes
 * (`type[1] version[4] path[8]`). An R-message's type is its T-message's + 1.
 *
 * The three messages this exercise needs:
 *
 *   Tversion  msize[4] version[s]        -> Rversion  msize[4] version[s]
 *   Tattach   fid[4] afid[4] uname[s] aname[s] n_uname[4]  -> Rattach qid[13]
 *   Twalk     fid[4] newfid[4] nwname[2] wname[n]          -> Rwalk  nwqid[2] wqid[n]
 *   Tlopen    fid[4] flags[4]            -> Rlopen  qid[13] iounit[4]
 *   Tread     fid[4] offset[8] count[4]  -> Rread   count[4] data[count]
 *
 * The decoder here is deliberately strict, because a reply comes from outside
 * the guest: the declared size must match the bytes received, the type must be
 * the one expected, the tag must be the one sent, and no byte may be left over.
 * That is requirement P9-09 of the specification, and it is the habit worth
 * taking away from this chapter even if you never write another 9P client.
 *
 * The real implementation (proto9p/include/pgw/p9.h) covers all of 9P2000.L
 * with the same rules and is tested against random input; this is the small
 * version that fits in a tutorial.
 */
#ifndef CLAB_P9_H
#define CLAB_P9_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Message types (wire values, 9P2000.L). */
#define CLAB_P9_TVERSION 100u
#define CLAB_P9_RVERSION 101u
#define CLAB_P9_TATTACH  104u
#define CLAB_P9_RATTACH  105u
#define CLAB_P9_TWALK    110u
#define CLAB_P9_RWALK    111u
#define CLAB_P9_TLOPEN   12u
#define CLAB_P9_RLOPEN   13u
#define CLAB_P9_TREAD    116u
#define CLAB_P9_RREAD    117u
#define CLAB_P9_RLERROR  7u

#define CLAB_P9_HDR 7u
#define CLAB_P9_NOTAG 0xFFFFu
#define CLAB_P9_NOFID 0xFFFFFFFFu

/* Our own error codes: 0 is success, so `if (rc)` works. */
typedef enum {
    CLAB_OK = 0,
    CLAB_ERR_SHORT = -1,  /* fewer bytes than the header claims */
    CLAB_ERR_SIZE = -2,   /* the size field and the buffer disagree */
    CLAB_ERR_TYPE = -3,   /* not the reply we asked for */
    CLAB_ERR_TAG = -4,    /* not the request we sent */
    CLAB_ERR_TRAILING = -5, /* bytes left over after the last field */
    CLAB_ERR_INVAL = -6,  /* a field does not fit the buffer */
    CLAB_ERR_REMOTE = -7  /* the server said Rlerror */
} clab_p9_err;

/* A reply, already validated: a view into the caller's buffer. */
typedef struct {
    const uint8_t *body; /* first byte after the 7-byte header */
    size_t len;          /* body length */
    uint8_t type;
    uint16_t tag;
} clab_p9_msg;

/* --- encoders: return the message length, or a negative clab_p9_err -------- */

int clab_p9_enc_tversion(uint8_t *buf, size_t cap, uint16_t tag, uint32_t msize,
                         const char *version);
int clab_p9_enc_tattach(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                        const char *uname, const char *aname);
int clab_p9_enc_twalk(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                      uint32_t newfid, uint16_t nwname, const char *const *names);
int clab_p9_enc_tlopen(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                       uint32_t flags);
int clab_p9_enc_tread(uint8_t *buf, size_t cap, uint16_t tag, uint32_t fid,
                      uint64_t off, uint32_t count);

/* --- validation and decoding ---------------------------------------------- */

/* Check size, type and tag; on success `out` points into `buf`. */
clab_p9_err clab_p9_check(const uint8_t *buf, size_t len, uint8_t want_type,
                          uint16_t want_tag, clab_p9_msg *out);

/* Rversion: msize then the version string. The string is copied, always
 * NUL-terminated; a wire string that does not fit `ver_cap` is CLAB_ERR_INVAL. */
clab_p9_err clab_p9_dec_rversion(const clab_p9_msg *m, uint32_t *msize, char *ver,
                                 size_t ver_cap);

/* Rread: count then the data. `data` is a zero-copy view into the message. */
clab_p9_err clab_p9_dec_rread(const clab_p9_msg *m, const uint8_t **data,
                              uint32_t *count);

/* Rlerror: one u32, a Linux errno. */
clab_p9_err clab_p9_dec_rlerror(const clab_p9_msg *m, uint32_t *linux_errno);

/* --- a tiny little-endian reader over a validated body --------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    bool fail; /* sticky: once a read crosses the end, nothing else is read */
} clab_p9_rd;

void clab_p9_rd_init(clab_p9_rd *r, const clab_p9_msg *m);
uint8_t clab_p9_get8(clab_p9_rd *r);
uint16_t clab_p9_get16(clab_p9_rd *r);
uint32_t clab_p9_get32(clab_p9_rd *r);
uint64_t clab_p9_get64(clab_p9_rd *r);
/* Length-prefixed string; returns false when it does not fit. */
bool clab_p9_get_str(clab_p9_rd *r, char *dst, size_t cap, uint16_t *out_len);
/* True when the whole body was consumed and nothing failed. */
bool clab_p9_rd_done(const clab_p9_rd *r);

#endif /* CLAB_P9_H */
