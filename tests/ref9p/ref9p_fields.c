/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/ref9p_fields.c - the oracle accessors and the raw conversation.
 *
 * The little-endian field readers and writers here are the independent second
 * opinion of the wire-format benches: they encode and decode the normative
 * 9P2000.L layout (see the diod protocol description) with plain byte access
 * and no shared code with pgw_buf/pgw_out, so a divergence between the code
 * under test and this oracle is exactly what the benches exist to catch.
 */
#define _POSIX_C_SOURCE 200809L

#include "ref9p_fields.h"
#include "ref9p_truth.h"

#include "pgw/limits.h"

#include "pgw/test.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

uint8_t ref9p_rx[REF9P_RX_CAP];
uint8_t ref9p_tx[REF9P_TX_CAP];

/* --- the oracle ------------------------------------------------------------ */

uint8_t ref9p_o_u8(const uint8_t *p, size_t off)
{
    return p[off];
}

uint16_t ref9p_o_u16(const uint8_t *p, size_t off)
{
    return (uint16_t)((uint16_t)p[off] | ((uint16_t)p[off + 1] << 8));
}

uint32_t ref9p_o_u32(const uint8_t *p, size_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

uint64_t ref9p_o_u64(const uint8_t *p, size_t off)
{
    return (uint64_t)ref9p_o_u32(p, off) | ((uint64_t)ref9p_o_u32(p, off + 4) << 32);
}

void ref9p_o_put_u8(uint8_t *p, size_t off, uint8_t v)
{
    p[off] = v;
}

void ref9p_o_put_u16(uint8_t *p, size_t off, uint16_t v)
{
    p[off] = (uint8_t)(v & 0xFFu);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

void ref9p_o_put_u32(uint8_t *p, size_t off, uint32_t v)
{
    ref9p_o_put_u16(p, off, (uint16_t)(v & 0xFFFFu));
    ref9p_o_put_u16(p, off + 2, (uint16_t)((v >> 16) & 0xFFFFu));
}

void ref9p_o_put_u64(uint8_t *p, size_t off, uint64_t v)
{
    ref9p_o_put_u32(p, off, (uint32_t)(v & 0xFFFFFFFFu));
    ref9p_o_put_u32(p, off + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

size_t ref9p_o_put_str(uint8_t *p, size_t off, const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    ref9p_o_put_u16(p, off, len);
    memcpy(p + off + 2, s, len);
    return off + 2 + (size_t)len;
}

size_t ref9p_o_seal(uint8_t *p, size_t len)
{
    ref9p_o_put_u32(p, 0, (uint32_t)len);
    return len;
}

void ref9p_expect_header(const uint8_t *msg, size_t len, uint8_t type,
                         uint16_t tag)
{
    PGW_ASSERT_EQ_U(ref9p_o_u32(msg, 0), len);
    PGW_ASSERT_EQ_U(ref9p_o_u8(msg, 4), type);
    PGW_ASSERT_EQ_U(ref9p_o_u16(msg, 5), tag);
}

size_t ref9p_call(struct ref9p_server *srv, const void *tx, size_t tx_len)
{
    size_t rx_len = 0;
    PGW_ASSERT_EQ_I(ref9p_frame(ref9p_fd(srv), tx, tx_len, ref9p_rx,
                                sizeof ref9p_rx, &rx_len, 5000u),
                    PGW_OK);
    PGW_ASSERT(rx_len >= PGW_HEADER_SIZE);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 0), rx_len);
    return rx_len;
}

struct ref9p_server *ref9p_fields_start(void)
{
    const char *root = pgw_test_tmpdir("ref9p-fields");
    PGW_ASSERT(root != NULL);
    PGW_ASSERT(ref9p_truth_write(root, "hello.txt", "hello world\n", 12));
    PGW_ASSERT(ref9p_truth_mkdir(root, "sub", 0755));
    PGW_ASSERT(ref9p_truth_write(root, "sub/nested.txt", "nested\n", 7));
    PGW_ASSERT(ref9p_truth_symlink(root, "hello.txt", "alias"));

    struct ref9p_server *srv = ref9p_start(root, "none");
    PGW_ASSERT(srv != NULL);

    size_t n = 7;
    ref9p_o_put_u8(ref9p_tx, 4, PGW_P9_TVERSION);
    ref9p_o_put_u16(ref9p_tx, 5, REF9P_NOTAG);
    ref9p_o_put_u32(ref9p_tx, n, 8192u);
    n = ref9p_o_put_str(ref9p_tx, n + 4, "9P2000.L");
    size_t len = ref9p_call(srv, ref9p_tx,
                            ref9p_o_seal(ref9p_tx, n));
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RVERSION, REF9P_NOTAG);
    PGW_ASSERT_EQ_U(ref9p_o_u32(ref9p_rx, 7), 8192u);

    n = 7;
    ref9p_o_put_u8(ref9p_tx, 4, PGW_P9_TATTACH);
    ref9p_o_put_u16(ref9p_tx, 5, 1);
    ref9p_o_put_u32(ref9p_tx, n, 0u);
    ref9p_o_put_u32(ref9p_tx, n + 4, PGW_FID_NONE);
    n = ref9p_o_put_str(ref9p_tx, n + 8, "baptiste");
    n = ref9p_o_put_str(ref9p_tx, n, "");
    ref9p_o_put_u32(ref9p_tx, n, PGW_FID_NONE);
    len = ref9p_call(srv, ref9p_tx, ref9p_o_seal(ref9p_tx, n + 4));
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RATTACH, 1);
    PGW_ASSERT_EQ_U(len, PGW_HEADER_SIZE + 13u);
    PGW_ASSERT_EQ_U(ref9p_o_u8(ref9p_rx, 7), PGW_P9_QTDIR);
    return srv;
}

size_t ref9p_walk_to(struct ref9p_server *srv, uint32_t newfid, const char *name)
{
    const char *names[1];
    names[0] = name;
    int32_t n = pgw_p9_enc_twalk(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG, 0u,
                                  newfid, 1u, names);
    PGW_ASSERT(n > 0);
    return ref9p_call(srv, ref9p_tx, (size_t)n);
}

void ref9p_lopen(struct ref9p_server *srv, uint32_t fid, uint32_t linux_flags)
{
    int32_t n = pgw_p9_enc_tlopen(ref9p_tx, sizeof ref9p_tx, REF9P_FIELD_TAG,
                                   fid, linux_flags);
    PGW_ASSERT(n > 0);
    size_t len = ref9p_call(srv, ref9p_tx, (size_t)n);
    ref9p_expect_header(ref9p_rx, len, PGW_P9_RLOPEN, REF9P_FIELD_TAG);
    PGW_ASSERT_EQ_U(len, PGW_HEADER_SIZE + 13u + 4u);
}