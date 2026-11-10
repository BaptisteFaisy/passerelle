/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/src/buf_reader.c - reader cursor implementation.
 */
#include "pgw/buf.h"

#include <string.h>

void pgw_buf_init(pgw_buf *r, const void *data, size_t len)
{
    if (r == NULL)
        return;
    r->base = (const uint8_t *)data;
    r->end = (data != NULL) ? len : 0;
    r->pos = 0;
    r->fail = (data == NULL && len > 0);
}

void pgw_buf_body(pgw_buf *r, const void *data, size_t len, size_t from)
{
    pgw_buf_init(r, data, len);
    if (r == NULL)
        return;
    if (from > len) {
        r->fail = true;
        r->pos = r->end;
    } else {
        r->pos = from;
    }
}

static const uint8_t *need(pgw_buf *r, size_t n)
{
    if (r == NULL || r->fail || r->end - r->pos < n) {
        if (r != NULL)
            r->fail = true;
        return NULL;
    }
    const uint8_t *p = r->base + r->pos;
    r->pos += n;
    return p;
}

uint8_t pgw_get_u8(pgw_buf *r)
{
    const uint8_t *p = need(r, 1);
    return p ? p[0] : 0;
}

uint16_t pgw_get_u16(pgw_buf *r)
{
    const uint8_t *p = need(r, 2);
    if (!p)
        return 0;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t pgw_get_u32(pgw_buf *r)
{
    const uint8_t *p = need(r, 4);
    if (!p)
        return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t pgw_get_u64(pgw_buf *r)
{
    const uint8_t *p = need(r, 8);
    if (!p)
        return 0;
    uint64_t lo = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint64_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                  ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    return lo | (hi << 32);
}

size_t pgw_get_data(pgw_buf *r, void *dst, size_t n)
{
    if (n == 0)
        return 0;
    const uint8_t *p = need(r, n);
    if (!p)
        return 0;
    if (dst != NULL)
        memcpy(dst, p, n);
    return n;
}

const uint8_t *pgw_take(pgw_buf *r, size_t n)
{
    return need(r, n);
}

int pgw_get_str(pgw_buf *r, char *dst, size_t cap, size_t *out_len)
{
    if (out_len != NULL)
        *out_len = 0;
    if (r == NULL || r->fail)
        return PGW_STR_INVALID;
    uint16_t len = pgw_get_u16(r);
    if (r->fail)
        return PGW_STR_INVALID;
    if (out_len != NULL)
        *out_len = len;
    const uint8_t *p = need(r, len);
    if (!p)
        return PGW_STR_INVALID;
    if (dst == NULL || cap == 0)
        return PGW_STR_OK;
    size_t copy_len = len < cap - 1 ? len : cap - 1;
    if (copy_len > 0)
        memcpy(dst, p, copy_len);
    dst[copy_len] = '\0';
    return (len >= cap) ? PGW_STR_TRUNCATED : PGW_STR_OK;
}

int pgw_peek_str_len(pgw_buf *r, size_t *out_len)
{
    if (out_len != NULL)
        *out_len = 0;
    if (r == NULL || r->fail || r->end - r->pos < 2)
        return -1;
    const uint8_t *p = r->base + r->pos;
    size_t len = (size_t)p[0] | ((size_t)p[1] << 8);
    if (out_len != NULL)
        *out_len = len;
    return (r->end - r->pos - 2 >= len) ? 0 : -1;
}

void pgw_skip(pgw_buf *r, size_t n)
{
    (void)need(r, n);
}

size_t pgw_left(const pgw_buf *r)
{
    if (r == NULL || r->fail || r->pos >= r->end)
        return 0;
    return r->end - r->pos;
}

bool pgw_buf_ok(const pgw_buf *r)
{
    return r != NULL && !r->fail;
}

void pgw_buf_fail(pgw_buf *r)
{
    if (r != NULL)
        r->fail = true;
}

void pgw_buf_clamp(pgw_buf *r, size_t end)
{
    if (r == NULL)
        return;
    if (end < r->pos || end > r->end) {
        r->fail = true;
        return;
    }
    r->end = end;
}
