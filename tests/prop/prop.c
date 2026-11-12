/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/prop/prop.c - the deterministic property harness (gap 61).
 *
 * Implements what prop.h documents: a seeded generator, per-iteration
 * reproducibility, shrinking of a failing case to a minimal one, and a bounded
 * iteration count. Used by the codec property benches of prop_p9*.c and
 * prop_fuse*.c (P9-09 and VFS-03: nothing the peer sends is trusted; ROB-06:
 * no out-of-bounds access, which the sanitizer build turns into a hard
 * failure).
 *
 * The generator is splitmix64: 64 bits of state, no table, and each output
 * depends on the state alone. Iteration i of a campaign is seeded from
 * (seed, i), so a failing case is reproducible from two numbers without
 * replaying the campaign that found it - which is what makes a property bench
 * usable in CI and in a bug report.
 */
#include "prop.h"

#include "pgw/test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROP_MIX     0x9E3779B97F4A7C15ULL
#define PROP_MIX_A   0xBF58476D1CE4E5B9ULL
#define PROP_MIX_B   0x94D049BB133111EBULL
#define PROP_DUMP    24u   /* bytes shown in a failure report */
#define PROP_MSG     768u  /* report buffer */
#define PROP_NAME_CH 0x61u /* 'a': names stay printable in a report */

/* --- the generator -------------------------------------------------------- */

void prop_seed(prop_rng *r, uint64_t seed)
{
    r->s = seed + PROP_MIX;
    if (r->s == 0)
        r->s = PROP_MIX; /* a zero state would only ever produce zeros */
}

uint64_t prop_u64(prop_rng *r)
{
    uint64_t z = (r->s += PROP_MIX);
    z = (z ^ (z >> 30)) * PROP_MIX_A;
    z = (z ^ (z >> 27)) * PROP_MIX_B;
    return z ^ (z >> 31);
}

uint32_t prop_u32(prop_rng *r)
{
    return (uint32_t)(prop_u64(r) >> 32);
}

uint32_t prop_below(prop_rng *r, uint32_t bound)
{
    return (bound == 0u) ? 0u : (prop_u32(r) % bound);
}

void prop_fill(prop_rng *r, prop_case *c, size_t off, size_t n)
{
    for (size_t i = 0; i < n && off + i < PROP_MAX_BYTES; i++)
        c->bytes[off + i] = (uint8_t)prop_u32(r);
}

/* A NUL-terminated name of exactly `len` characters ('a', 'b', ... cycling),
 * truncated to what `cap` holds. Callers pass a length above the protocol
 * limit on purpose: the refusal is the property being checked. */
void prop_name(char *out, size_t cap, size_t len)
{
    if (cap == 0)
        return;
    size_t n = (len < cap - 1u) ? len : cap - 1u;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)(PROP_NAME_CH + (uint8_t)(i % 26u));
    out[n] = '\0';
}

/* --- the buffer guard ----------------------------------------------------- */

void prop_guard_arm(uint8_t *buf, size_t cap)
{
    memset(buf + cap, (int)PROP_GUARD_BYTE, PROP_GUARD);
}

bool prop_guard_ok(const uint8_t *buf, size_t cap)
{
    for (size_t i = 0; i < PROP_GUARD; i++) {
        if (buf[cap + i] != PROP_GUARD_BYTE)
            return false;
    }
    return true;
}

/* --- shrinking ------------------------------------------------------------ */

/* Shortest prefix of c->bytes that still fails. A linear scan, not a binary
 * search: nothing guarantees that a longer prefix fails too (a decoder may
 * reject a message precisely because it grew a trailing byte), and a wrong
 * monotonicity assumption would report a case that is not minimal. */
static void shrink_bytes(prop_pred pred, prop_case *c)
{
    size_t full = c->len;
    for (size_t n = 0; n < full; n++) {
        c->len = n;
        if (!pred(c))
            return; /* c->len keeps the shortest failing prefix */
    }
    c->len = full;
}

/* Clear the high bits of one knob: the smallest value that still fails wins,
 * so a report shows 0x00000001 rather than 0x9E3779B9 when both fail. */
static bool knob_still_fails(prop_pred pred, prop_case *c)
{
    return !pred(c);
}

static void shrink_one_u64(prop_pred pred, prop_case *c, uint64_t *knob)
{
    static const uint64_t masks[] = { 0x0ULL,
                                      0xFULL,
                                      0xFFULL,
                                      0xFFFFULL,
                                      0xFFFFFFULL,
                                      0xFFFFFFFFULL,
                                      0xFFFFFFFFFFULL,
                                      0xFFFFFFFFFFFFULL };
    uint64_t orig = *knob;
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        *knob = orig & masks[i];
        if (knob_still_fails(pred, c))
            return;
    }
    *knob = orig;
}

static void shrink_knobs(prop_pred pred, prop_case *c)
{
    uint64_t value = c->value;
    uint64_t a = c->a;
    uint64_t b = c->b;
    shrink_one_u64(pred, c, &c->value);
    shrink_one_u64(pred, c, &a);
    c->a = (uint32_t)a;
    shrink_one_u64(pred, c, &b);
    c->b = (uint32_t)b;
    (void)value;
}

/* --- the report ----------------------------------------------------------- */

static void dump_hex(char *out, size_t cap, const uint8_t *b, size_t n)
{
    size_t off = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && off + 3 < cap; i++)
        off += (size_t)snprintf(out + off, cap - off, "%02x", b[i]);
}

static void report(const char *name, uint64_t seed, uint32_t iter,
                   const prop_case *c)
{
    char hex[PROP_DUMP * 2u + 8u];
    char msg[PROP_MSG];
    dump_hex(hex, sizeof(hex), c->bytes,
             (c->len < PROP_DUMP) ? c->len : PROP_DUMP);
    snprintf(msg, sizeof(msg),
             "property \"%s\" failed: seed %llu iteration %u, shrunk to len "
             "%zu value 0x%llx a %u b %u tag \"%s\" bytes %s%s",
             name, (unsigned long long)seed, iter, c->len,
             (unsigned long long)c->value, c->a, c->b, c->tag, hex,
             (c->len > PROP_DUMP) ? "..." : "");
    pgw_fail(__FILE__, __LINE__, "%s", msg);
}

/* --- the runner ----------------------------------------------------------- */

void prop_forall(const char *name, prop_gen gen, prop_pred pred, uint32_t iters,
                 uint64_t seed)
{
    for (uint32_t i = 0; i < iters; i++) {
        prop_rng r;
        prop_case c;
        memset(&c, 0, sizeof(c));
        /* Iteration i depends on (seed, i) alone: replaying one iteration
         * needs no replay of the ones before it. */
        prop_seed(&r, seed ^ ((uint64_t)i * PROP_MIX));
        gen(&r, &c);
        if (pred(&c))
            continue;
        shrink_bytes(pred, &c);
        shrink_knobs(pred, &c);
        report(name, seed, i, &c); /* does not return */
    }
}

uint32_t prop_iters(const char *var, uint32_t dflt)
{
    const char *v = getenv(var);
    if (v == NULL || *v == '\0')
        return dflt;
    long n = strtol(v, NULL, 10);
    if (n < 1 || n > 100000000L)
        return dflt;
    return (uint32_t)n;
}
