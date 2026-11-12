/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuzz_p9.c - randomized input campaign against the 9P2000.L decoders.
 *
 * Requirement ROB-06: the protocol library must not crash on *any* response,
 * and P9-09: every response is validated. Two families of inputs are used:
 *
 *   1. pure noise - random bytes at random lengths, which mostly exercise the
 *      "too short" and "bad type" paths;
 *   2. mutated well-formed messages - a message built with the protocol
 *      encoders (so its framing, tag and length fields are self-consistent)
 *      then damaged: truncated length, flipped byte, bogus size[4] field, wrong
 *      tag or wrong type. These reach deep into the decoders, where the
 *      interesting bugs live (length fields that disagree with the buffer,
 *      readdir records that claim more bytes than remain). Note that the
 *      encoders produce the request shapes; the response shapes are exercised
 *      exhaustively by proto9p's own fake-server tests, and this campaign is
 *      about *hostile* input rather than about happy-path coverage.
 *
 * The run is deterministic: the seed comes from the environment so a failure is
 * reproducible, and the same seed replays the same sequence. Built with
 * -DPGW_SANITIZERS=ON in CI, any out-of-bounds read becomes a hard failure.
 *
 * Optional libFuzzer entry point: compile with -DPGW_LIBFUZZER=ON and
 * -fsanitize=fuzzer to drive the same code from a coverage-guided fuzzer.
 */
#define _POSIX_C_SOURCE 200809L
#include "pgw/p9.h"

#include "fuzz_corpus.h"
#include "fuzz_p9_decode.h"

#include "pgw/limits.h"
#include "pgw/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MSG 2048
#define DEFAULT_ITERS 20000

static uint32_t g_state = 0x12345678u;

/* The kept corpus (gap 48): filled when PGW_FUZZ_CORPUS_DIR points at the
 * directory that must receive it, empty otherwise. */
static fuzz_corpus g_corpus;
static bool g_corpus_on;

static void corpus_from_env(void)
{
    const char *dir = getenv("PGW_FUZZ_CORPUS_DIR");
    if (dir == NULL || dir[0] == '\0')
        return;
    fuzz_corpus_init(&g_corpus, dir, "p9");
    g_corpus_on = true;
}

static uint32_t rnd(void)
{
    uint32_t x = g_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_state = x;
    return x;
}

/* Build one well-formed message with the protocol encoders. Returns its
 * length, or 0 when the encoder refused (nothing to mutate then). The family
 * is the kind of message: the kept corpus keeps one file per family. */
static size_t build_message(uint8_t *buf, size_t cap, uint16_t tag, const char **family)
{
    int32_t n = 0;
    switch (rnd() % 5u) {
    case 0:
        *family = "version";
        n = pgw_p9_enc_tversion(buf, cap, tag, PGW_MSIZE_DEFAULT, "9P2000.L");
        break;
    case 1: {
        const char *names[2] = {"alpha", "beta"};
        *family = "walk";
        n = pgw_p9_enc_twalk(buf, cap, tag, 1u, 2u, 2u, names);
        break;
    }
    case 2:
        *family = "read";
        n = pgw_p9_enc_tread(buf, cap, tag, 3u, 4096u, 128u);
        break;
    case 3:
        *family = "getattr";
        n = pgw_p9_enc_tgetattr(buf, cap, tag, 4u, PGW_P9_GETATTR_BASIC);
        break;
    default:
        *family = "statfs";
        n = pgw_p9_enc_tstatfs(buf, cap, tag, 5u);
        break;
    }
    return (n > 0) ? (size_t)n : 0;
}

/* Damage a well-formed message in place, one of five ways. */
static void damage(uint8_t *buf, size_t *len)
{
    switch (rnd() % 5u) {
    case 0: /* truncate anywhere, including to zero */
        *len = (size_t)(rnd() % (uint32_t)(*len + 1u));
        break;
    case 1: /* corrupt the size[4] field */
        if (*len >= 4)
            buf[rnd() % 4u] = (uint8_t)rnd();
        break;
    case 2: /* flip a byte in the body */
        if (*len > PGW_HEADER_SIZE)
            buf[PGW_HEADER_SIZE + rnd() % (uint32_t)(*len - PGW_HEADER_SIZE)] ^=
                    (uint8_t)(1u << (rnd() % 8u));
        break;
    case 3: { /* wrong tag */
        uint16_t bad = (uint16_t)rnd();
        if (*len >= 7) {
            buf[5] = (uint8_t)(bad & 0xFFu);
            buf[6] = (uint8_t)(bad >> 8);
        }
        break;
    }
    default: /* wrong type */
        if (*len >= 5)
            buf[4] = (uint8_t)rnd();
        break;
    }
}

static size_t make_mutated(uint8_t *buf, size_t cap, uint16_t tag, const char **family)
{
    size_t len = build_message(buf, cap, tag, family);
    if (len > 0)
        damage(buf, &len);
    return len;
}

static void run_campaign(size_t iters, uint32_t seed)
{
    g_state = seed | 1u;
    static uint8_t buf[MAX_MSG];
    for (size_t i = 0; i < iters; i++) {
        uint16_t tag = (uint16_t)rnd();
        const char *family = "noise";
        size_t len;
        if ((rnd() & 1u) == 0u) {
            len = (size_t)(rnd() % MAX_MSG);
            for (size_t j = 0; j < len; j++)
                buf[j] = (uint8_t)rnd();
        } else {
            len = make_mutated(buf, sizeof(buf), tag, &family);
        }
        int depth = fuzz_decode_p9(buf, len, tag);
        if (g_corpus_on)
            fuzz_corpus_offer(&g_corpus, family, buf, len, depth);
        if (!fuzz_p9_header_ok(buf, len)) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "seed %u iter %zu: header accepted with size out of range "
                     "(len %zu)",
                     seed, i, len);
            pgw_fail(__FILE__, __LINE__, "%s", msg);
        }
    }
}

static size_t iters_from_env(void)
{
    const char *v = getenv("PGW_FUZZ_ITERS");
    if (v == NULL || *v == '\0')
        return DEFAULT_ITERS;
    long n = strtol(v, NULL, 10);
    if (n < 1 || n > 100000000L)
        return DEFAULT_ITERS;
    return (size_t)n;
}

static void test_p9_noise_campaign(void)
{
    run_campaign(iters_from_env(), 0xC0FFEEu);
}
PGW_TEST(test_p9_noise_campaign);

static void test_p9_seeded_campaigns(void)
{
    size_t iters = iters_from_env() / 4u;
    if (iters == 0)
        iters = 1;
    for (uint32_t s = 1u; s <= 4u; s++)
        run_campaign(iters, s * 7919u);
}
PGW_TEST(test_p9_seeded_campaigns);

#ifdef PGW_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    (void)fuzz_decode_p9(data, size, 0u);
    (void)fuzz_p9_header_ok(data, size);
    return 0;
}
#else
int main(int argc, char **argv)
{
    corpus_from_env();
    int rc = pgw_test_main(argc, argv);
    if (g_corpus_on) {
        int written = fuzz_corpus_flush(&g_corpus);
        fprintf(stdout, "# corpus: %d file(s) written in %s/p9\n", written, g_corpus.dir);
    }
    return rc;
}
#endif
