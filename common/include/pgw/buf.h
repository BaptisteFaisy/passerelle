/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pgw/buf.h - byte cursors for protocol messages.
 *
 * Little-endian, byte at a time: no struct is ever cast onto a buffer, so the
 * same object code is correct on any alignment and any endianness (x86_64 and
 * ARM64 guests, PC test builds).
 *
 * Both cursors carry a sticky failure flag. After an access that would cross
 * the end, nothing else is read or written and the caller checks the flag once,
 * at the end. That single rule is what makes the decoders safe against a
 * truncated or hostile server (P9-09, ROB-06, ROB-07).
 *
 * pgw_take() hands out a pointer into the caller's buffer instead of copying,
 * which is what lets the transport keep request buffers zero-copy (VIO-07).
 */
#ifndef PGW_BUF_H
#define PGW_BUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- reader --------------------------------------------------------------- */

typedef struct pgw_buf {
    const uint8_t *base; /* first byte of the message */
    size_t end;          /* one past the last readable byte */
    size_t pos;          /* next byte to read */
    bool fail;           /* sticky */
} pgw_buf;

/* Bind a reader to `len` bytes starting at `data`. */
void pgw_buf_init(pgw_buf *r, const void *data, size_t len);

/* Bind a reader to the body of a message: bytes [from, len) of `data`.
 * Used after the 9P header or the FUSE header has been parsed. */
void pgw_buf_body(pgw_buf *r, const void *data, size_t len, size_t from);

uint8_t pgw_get_u8(pgw_buf *r);
uint16_t pgw_get_u16(pgw_buf *r);
uint32_t pgw_get_u32(pgw_buf *r);
uint64_t pgw_get_u64(pgw_buf *r);

/* Copy up to `n` bytes into `dst`; returns the number copied (0 on failure). */
size_t pgw_get_data(pgw_buf *r, void *dst, size_t n);

/* Zero-copy view of the next `n` bytes, or NULL if unavailable. The pointer
 * stays valid as long as the caller's buffer does. */
const uint8_t *pgw_take(pgw_buf *r, size_t n);

/* Read a length-prefixed string: u16 length, then that many bytes.
 * The result is copied into `dst` and always NUL-terminated; `*out_len`
 * receives the wire length (which may exceed `cap - 1`, in which case the
 * copy is truncated and PGW_STR_TRUNCATED is returned). */
#define PGW_STR_OK        0
#define PGW_STR_TRUNCATED 1
#define PGW_STR_INVALID (-1) /* wire length ran past the message */
int pgw_get_str(pgw_buf *r, char *dst, size_t cap, size_t *out_len);

/* Read a u16 length without consuming the bytes that follow (peek). */
int pgw_peek_str_len(pgw_buf *r, size_t *out_len);

void pgw_skip(pgw_buf *r, size_t n);
size_t pgw_left(const pgw_buf *r);
bool pgw_buf_ok(const pgw_buf *r); /* true when nothing failed */
void pgw_buf_fail(pgw_buf *r);     /* force the sticky flag */

/* Shrink the readable range to `end`. A virtio used-ring length may be larger
 * than the size[4] field of a 9P message and never smaller; clamping to the
 * declared size is what makes "no trailing garbage" a checkable property. */
void pgw_buf_clamp(pgw_buf *r, size_t end);

/* --- writer --------------------------------------------------------------- */

typedef struct pgw_out {
    uint8_t *base;
    size_t cap;
    size_t pos;
    bool fail; /* sticky */
} pgw_out;

/* Bind a writer to `cap` writable bytes at `buf`. */
void pgw_out_init(pgw_out *w, void *buf, size_t cap);

void pgw_put_u8(pgw_out *w, uint8_t v);
void pgw_put_u16(pgw_out *w, uint16_t v);
void pgw_put_u32(pgw_out *w, uint32_t v);
void pgw_put_u64(pgw_out *w, uint64_t v);

/* Write `n` bytes from `src`. Returns the number written (0 on failure). */
size_t pgw_put_data(pgw_out *w, const void *src, size_t n);

/* Write a length-prefixed string: u16 length then the bytes, no NUL.
 * Returns false when `len` does not fit in u16 or the buffer is full. */
bool pgw_put_str(pgw_out *w, const char *s, size_t len);

/* Write `n` zero bytes (protocol padding). */
void pgw_put_pad(pgw_out *w, size_t n);

/* Reserve `n` bytes and return a pointer to them, or NULL on failure. Used to
 * leave room for a length that is only known once the body is encoded. */
uint8_t *pgw_reserve(pgw_out *w, size_t n);

/* Absolute access for patching a reserved field. Returns NULL out of range. */
uint8_t *pgw_out_at(pgw_out *w, size_t off);

/* Patch a little-endian u32 at `off` with the current written length.
 * Returns false when `off + 4` is out of range or the writer failed. */
bool pgw_patch_len_u32(pgw_out *w, size_t off);

size_t pgw_written(const pgw_out *w);
size_t pgw_room(const pgw_out *w);
bool pgw_out_ok(const pgw_out *w);
void pgw_out_fail(pgw_out *w);
void pgw_out_reset(pgw_out *w); /* keep the buffer, rewind to zero bytes */

#ifdef __cplusplus
}
#endif

#endif /* PGW_BUF_H */
