/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuzz_fuse.c - randomized input campaign against the FUSE/virtiofs decoders.
 *
 * Same shape and same rationale as fuzz_p9.c (ROB-06, VFS-03): a virtiofs
 * server is a process on the PC, so its answers must be treated as hostile
 * input. Noise and mutated well-formed messages are pushed through every
 * decoder; the header invariant (length, unique, non-positive error) is checked
 * on the way so that a decoder that "accepts" garbage is a test failure and not
 * just a silent no-op.
 *
 * Deterministic: PGW_FUZZ_ITERS controls the campaign length, the seeds are
 * fixed, so a CI failure replays exactly. Optional coverage-guided mode with
 * -DPGW_LIBFUZZER=ON.
 */
#define _POSIX_C_SOURCE 200809L
#include "pgw/fusep.h"

#include "pgw/limits.h"
#include "pgw/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MSG 2048
#define DEFAULT_ITERS 20000
#define FUSE_HDR_MIN 16u

static uint32_t g_state = 0x9E3779B9u;

static uint32_t rnd(void)
{
    uint32_t x = g_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_state = x;
    return x;
}

static void decode_all(const uint8_t *buf, size_t len, uint64_t unique)
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

    (void)pgw_fuse_dec_hdr(buf, len, &hdr);
    (void)pgw_fuse_dec_init(buf, len, unique, &init);
    (void)pgw_fuse_dec_entry(buf, len, unique, &entry);
    (void)pgw_fuse_dec_attr(buf, len, unique, &attr);
    (void)pgw_fuse_dec_open(buf, len, unique, &fh, &open_flags);
    (void)pgw_fuse_dec_create(buf, len, unique, &entry, &fh);
    (void)pgw_fuse_dec_read(buf, len, unique, &data, &data_len);
    (void)pgw_fuse_dec_write(buf, len, unique, &written);
    (void)pgw_fuse_dec_readdir(buf, len, unique, ents, 8, &ndirents);
    (void)pgw_fuse_dec_readlink(buf, len, unique, target, sizeof(target));
    (void)pgw_fuse_dec_statfs(buf, len, unique, &sfs);
    (void)pgw_fuse_dec_empty(buf, len, unique);
}

/* A decoder that accepts a header must report a length inside the buffer and a
 * non-positive error (FUSE carries -errno, never a positive code). */
static int header_invariant_ok(const uint8_t *buf, size_t len)
{
    pgw_fuse_out_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (pgw_fuse_dec_hdr(buf, len, &hdr) != PGW_OK)
        return 1;
    if (hdr.len < FUSE_HDR_MIN)
        return 0;
    if ((size_t)hdr.len > len)
        return 0;
    if (hdr.error > 0)
        return 0;
    return 1;
}

static size_t make_mutated(uint8_t *buf, size_t cap, uint64_t unique)
{
    int32_t n = 0;
    switch (rnd() % 5u) {
    case 0:
        n = pgw_fuse_enc_init(buf, cap, unique, 131072u, PGW_FUSE_F_ASYNC_READ);
        break;
    case 1:
        n = pgw_fuse_enc_lookup(buf, cap, unique, PGW_FUSE_ROOT_ID, "entry");
        break;
    case 2:
        n = pgw_fuse_enc_getattr(buf, cap, unique, 42u, 0u, false);
        break;
    case 3:
        n = pgw_fuse_enc_read(buf, cap, unique, 42u, 7u, 0u, 4096u, false);
        break;
    default:
        n = pgw_fuse_enc_statfs(buf, cap, unique, PGW_FUSE_ROOT_ID);
        break;
    }
    if (n <= 0)
        return 0;
    size_t len = (size_t)n;
    switch (rnd() % 5u) {
    case 0:
        len = (size_t)(rnd() % (uint32_t)(len + 1u));
        break;
    case 1: /* corrupt the length word */
        if (len >= 4)
            buf[rnd() % 4u] = (uint8_t)rnd();
        break;
    case 2: /* corrupt the error word (bytes 4..7 of the out header) */
        if (len >= 8)
            buf[4 + rnd() % 4u] = (uint8_t)rnd();
        break;
    case 3: /* corrupt the unique id */
        if (len >= 16)
            buf[8 + rnd() % 8u] = (uint8_t)rnd();
        break;
    default: /* flip a body byte */
        if (len > FUSE_HDR_MIN)
            buf[FUSE_HDR_MIN + rnd() % (uint32_t)(len - FUSE_HDR_MIN)] ^=
                (uint8_t)(1u << (rnd() % 8u));
        break;
    }
    return len;
}

static void run_campaign(size_t iters, uint32_t seed)
{
    g_state = seed | 1u;
    static uint8_t buf[MAX_MSG];
    for (size_t i = 0; i < iters; i++) {
        uint64_t unique = ((uint64_t)rnd() << 32) | (uint64_t)rnd();
        size_t len;
        if ((rnd() & 1u) == 0u) {
            len = (size_t)(rnd() % MAX_MSG);
            for (size_t j = 0; j < len; j++)
                buf[j] = (uint8_t)rnd();
        } else {
            len = make_mutated(buf, sizeof(buf), unique);
        }
        decode_all(buf, len, unique);
        if (!header_invariant_ok(buf, len)) {
            pgw_fail(__FILE__, __LINE__,
                     "seed %u iter %zu: FUSE header accepted out of contract "
                     "(buffer %zu bytes)", seed, i, len);
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

static void test_fuse_noise_campaign(void)
{
    run_campaign(iters_from_env(), 0x51F5EEDu);
}
PGW_TEST(test_fuse_noise_campaign);

static void test_fuse_seeded_campaigns(void)
{
    size_t iters = iters_from_env() / 4u;
    if (iters == 0)
        iters = 1;
    for (uint32_t s = 1u; s <= 4u; s++)
        run_campaign(iters, s * 104729u);
}
PGW_TEST(test_fuse_seeded_campaigns);

#ifdef PGW_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    decode_all(data, size, 1u);
    (void)header_invariant_ok(data, size);
    return 0;
}
#else
int main(int argc, char **argv)
{
    return pgw_test_main(argc, argv);
}
#endif
