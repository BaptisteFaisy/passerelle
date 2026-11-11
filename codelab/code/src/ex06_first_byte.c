/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ex06_first_byte.c - chapter 7's exercise: the first 9P exchange, and the
 * first byte read, with no device at all.
 *
 * The 9P session of the tutorial is: Tversion/Rversion, Tattach/Rattach,
 * Twalk/Rwalk, Tlopen/Rlopen, Tread/Rread. This program encodes each request,
 * checks its bytes against the wire layout, and decodes a reply built by hand.
 * It is the part of a driver you can write and debug before you have an
 * interrupt that works, and the part that tells you whether a failure later is
 * in the protocol or in the transport.
 *
 * The reply bytes are built here rather than received, which is deliberate:
 * the decoder must be exercised against bytes it did not produce.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clab_check.h"
#include "clab_p9.h"

/* Build an Rversion by hand: size, type, tag, msize, version string. */
static size_t build_rversion(uint8_t *buf, uint16_t tag, uint32_t msize,
                             const char *version)
{
    const size_t vlen = strlen(version);
    const size_t total = CLAB_P9_HDR + 4u + 2u + vlen;
    size_t at = 0;

    for (unsigned i = 0; i < 4u; i++)
        buf[at++] = (uint8_t)((uint32_t)total >> (8u * i));
    buf[at++] = (uint8_t)CLAB_P9_RVERSION;
    buf[at++] = (uint8_t)(tag & 0xFFu);
    buf[at++] = (uint8_t)(tag >> 8u);
    for (unsigned i = 0; i < 4u; i++)
        buf[at++] = (uint8_t)(msize >> (8u * i));
    buf[at++] = (uint8_t)(vlen & 0xFFu);
    buf[at++] = (uint8_t)(vlen >> 8u);
    memcpy(buf + at, version, vlen);
    return total;
}

/* Build an Rread: size, type, tag, count, data. */
static size_t build_rread(uint8_t *buf, uint16_t tag, const char *data)
{
    const size_t dlen = strlen(data);
    const size_t total = CLAB_P9_HDR + 4u + dlen;
    size_t at = 0;

    for (unsigned i = 0; i < 4u; i++)
        buf[at++] = (uint8_t)((uint32_t)total >> (8u * i));
    buf[at++] = (uint8_t)CLAB_P9_RREAD;
    buf[at++] = (uint8_t)(tag & 0xFFu);
    buf[at++] = (uint8_t)(tag >> 8u);
    for (unsigned i = 0; i < 4u; i++)
        buf[at++] = (uint8_t)(dlen >> (8u * i));
    memcpy(buf + at, data, dlen);
    return total;
}

static void step_tversion(void)
{
    uint8_t req[64];
    uint8_t rpl[64];
    clab_p9_msg msg;
    char version[16];
    uint32_t msize = 0;
    const int n = clab_p9_enc_tversion(req, sizeof req, CLAB_P9_NOTAG, 8192u, "9P2000.L");

    printf("\nTversion (msize 8192, \"9P2000.L\"): %d bytes\n", n);
    CLAB_CHECK_EQ(n, (int)(CLAB_P9_HDR + 4u + 2u + 8u));
    /* The header, byte by byte: size, type 100, tag 0xFFFF. */
    CLAB_CHECK_EQ(req[4], CLAB_P9_TVERSION);
    CLAB_CHECK_EQ(req[5], 0xFF);
    CLAB_CHECK_EQ(req[6], 0xFF);

    const size_t total = build_rversion(rpl, CLAB_P9_NOTAG, 8192u, "9P2000.L");
    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RVERSION, CLAB_P9_NOTAG, &msg),
                  CLAB_OK);
    CLAB_CHECK_EQ(clab_p9_dec_rversion(&msg, &msize, version, sizeof version),
                  CLAB_OK);
    printf("Rversion: msize %u, version \"%s\"\n", msize, version);
    CLAB_CHECK_EQ(msize, 8192);
    CLAB_CHECK_STR(version, "9P2000.L");
}

static void step_tattach(void)
{
    uint8_t req[128];
    const int n = clab_p9_enc_tattach(req, sizeof req, 1u, 0u, "nobody", "");

    printf("\nTattach (fid 0, uname \"nobody\", aname \"\"): %d bytes\n", n);
    CLAB_CHECK_EQ(n, (int)(CLAB_P9_HDR + 4u + 4u + 2u + 6u + 2u + 4u));
    CLAB_CHECK_EQ(req[4], CLAB_P9_TATTACH);
    /* afid is NOFID: no authentication. */
    CLAB_CHECK_EQ(req[CLAB_P9_HDR + 4u], 0xFF);
    CLAB_CHECK_EQ(req[CLAB_P9_HDR + 7u], 0xFF);
}

static void step_twalk_tlopen(void)
{
    uint8_t req[128];
    const char *names[] = {"hello.txt"};
    int n = clab_p9_enc_twalk(req, sizeof req, 2u, 0u, 1u, 1u, names);

    printf("\nTwalk (fid 0 -> newfid 1, \"hello.txt\"): %d bytes\n", n);
    CLAB_CHECK_EQ(n, (int)(CLAB_P9_HDR + 4u + 4u + 2u + 2u + 9u));

    /* Linux O_RDONLY is 0; the wire carries Linux values, not QNX ones. */
    n = clab_p9_enc_tlopen(req, sizeof req, 3u, 1u, 0u);
    printf("Tlopen (fid 1, O_RDONLY): %d bytes\n", n);
    CLAB_CHECK_EQ(n, (int)(CLAB_P9_HDR + 4u + 4u));
    CLAB_CHECK_EQ(req[4], CLAB_P9_TLOPEN);
}

static void step_tread_and_first_byte(void)
{
    uint8_t req[64];
    uint8_t rpl[64];
    clab_p9_msg msg;
    const uint8_t *data = NULL;
    uint32_t count = 0;
    int n = clab_p9_enc_tread(req, sizeof req, 4u, 1u, 0u, 4096u);

    printf("\nTread (fid 1, offset 0, count 4096): %d bytes\n", n);
    CLAB_CHECK_EQ(n, (int)(CLAB_P9_HDR + 4u + 8u + 4u));

    const size_t total = build_rread(rpl, 4u, "hello, QNX\n");
    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RREAD, 4u, &msg), CLAB_OK);
    CLAB_CHECK_EQ(clab_p9_dec_rread(&msg, &data, &count), CLAB_OK);
    CLAB_CHECK_EQ(count, 11);
    printf("Rread: %u bytes: \"%.*s\"\n", count, (int)count, (const char *)data);
    printf("first byte: 0x%02x '%c'\n", data[0], data[0]);
    CLAB_CHECK_EQ(data[0], 'h');
}

/* A reply that lies about its size must be refused, not read. */
static void step_reject_bad_reply(void)
{
    uint8_t rpl[64];
    clab_p9_msg msg;
    const size_t total = build_rread(rpl, 4u, "hello");

    printf("\nmalformed replies:\n");
    /* Truncated: the size field still says "total", the buffer is shorter, and
     * the decoder must notice the disagreement rather than read what is there. */
    CLAB_CHECK_EQ(clab_p9_check(rpl, total - 1u, CLAB_P9_RREAD, 4u, &msg),
                  CLAB_ERR_SIZE);
    rpl[4] = (uint8_t)CLAB_P9_RREAD; /* correct type, wrong tag */
    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RREAD, 9u, &msg), CLAB_ERR_TAG);
    /* A count larger than the bytes that arrived. */
    rpl[CLAB_P9_HDR + 0u] = 200u;
    CLAB_CHECK_EQ(clab_p9_check(rpl, total, CLAB_P9_RREAD, 4u, &msg), CLAB_OK);
    const uint8_t *data = NULL;
    uint32_t count = 0;
    CLAB_CHECK_EQ(clab_p9_dec_rread(&msg, &data, &count), CLAB_ERR_SIZE);
    printf("  short reply, wrong tag and inflated count all refused\n");
}

int main(void)
{
    printf("chapter 7 - the first 9P exchange, and the first byte");
    step_tversion();
    step_tattach();
    step_twalk_tlopen();
    step_tread_and_first_byte();
    step_reject_bad_reply();
    return clab_result();
}
