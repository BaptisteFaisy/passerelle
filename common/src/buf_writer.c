/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * common/src/buf_writer.c - writer cursor implementation.
 */
#include "pgw/buf.h"

#include <string.h>

void pgw_out_init(pgw_out *w, void *buf, size_t cap)
{
    if (w == NULL)
        return;
    w->base = (uint8_t *)buf;
    w->cap = (buf != NULL) ? cap : 0;
    w->pos = 0;
    w->fail = (buf == NULL && cap > 0);
}

static uint8_t *need(pgw_out *w, size_t n)
{
    if (w == NULL || w->fail || w->cap - w->pos < n) {
        if (w != NULL)
            w->fail = true;
        return NULL;
    }
    uint8_t *p = w->base + w->pos;
    w->pos += n;
    return p;
}

void pgw_put_u8(pgw_out *w, uint8_t v)
{
    uint8_t *p = need(w, 1);
    if (p)
        p[0] = v;
}

void pgw_put_u16(pgw_out *w, uint16_t v)
{
    uint8_t *p = need(w, 2);
    if (!p)
        return;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void pgw_put_u32(pgw_out *w, uint32_t v)
{
    uint8_t *p = need(w, 4);
    if (!p)
        return;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void pgw_put_u64(pgw_out *w, uint64_t v)
{
    uint8_t *p = need(w, 8);
    if (!p)
        return;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

size_t pgw_put_data(pgw_out *w, const void *src, size_t n)
{
    if (n == 0)
        return 0;
    uint8_t *p = need(w, n);
    if (!p)
        return 0;
    if (src != NULL)
        memcpy(p, src, n);
    else
        memset(p, 0, n);
    return n;
}

bool pgw_put_str(pgw_out *w, const char *s, size_t len)
{
    if (len > 0xFFFFu || (len > 0 && s == NULL)) {
        if (w)
            w->fail = true;
        return false;
    }
    pgw_put_u16(w, (uint16_t)len);
    if (len > 0)
        pgw_put_data(w, s, len);
    return pgw_out_ok(w);
}

void pgw_put_pad(pgw_out *w, size_t n)
{
    if (n == 0)
        return;
    uint8_t *p = need(w, n);
    if (p)
        memset(p, 0, n);
}

uint8_t *pgw_reserve(pgw_out *w, size_t n)
{
    return need(w, n);
}

uint8_t *pgw_out_at(pgw_out *w, size_t off)
{
    if (w == NULL || w->fail || off >= w->pos)
        return NULL;
    return w->base + off;
}

bool pgw_patch_len_u32(pgw_out *w, size_t off)
{
    if (w == NULL || w->fail || off + 4 > w->pos)
        return false;
    uint32_t val = (uint32_t)w->pos;
    uint8_t *p = w->base + off;
    p[0] = (uint8_t)val;
    p[1] = (uint8_t)(val >> 8);
    p[2] = (uint8_t)(val >> 16);
    p[3] = (uint8_t)(val >> 24);
    return true;
}

size_t pgw_written(const pgw_out *w)
{
    return (w != NULL) ? w->pos : 0;
}

size_t pgw_room(const pgw_out *w)
{
    if (w == NULL || w->fail || w->pos >= w->cap)
        return 0;
    return w->cap - w->pos;
}

bool pgw_out_ok(const pgw_out *w)
{
    return w != NULL && !w->fail;
}

void pgw_out_fail(pgw_out *w)
{
    if (w != NULL)
        w->fail = true;
}

void pgw_out_reset(pgw_out *w)
{
    if (w == NULL)
        return;
    w->pos = 0;
    w->fail = false;
}
