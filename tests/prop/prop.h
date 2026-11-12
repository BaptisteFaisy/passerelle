/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/prop/prop.h - the deterministic property harness (gap 61).
 *
 * The codecs of proto9p/ and protofuse/ are trust boundaries: every field they
 * read comes from outside. A property harness states what must hold for *all*
 * inputs of a shape - "encode then decode gives back the same value", "a
 * declared length never exceeds the buffer", "a lying count is refused" - and
 * checks it over a seeded pseudo-random sequence, so a failure is reproducible
 * from two numbers and no external dependency is needed (hypothesis is not
 * installed on the runners, and the deterministic harness is the guarantee).
 *
 * What it provides over a plain loop:
 *   - reproducibility: iteration i is seeded from (seed, i) alone, so a failing
 *     case can be replayed on its own;
 *   - shrinking: on failure the case is reduced - shortest byte prefix that
 *     still fails, then each integer knob with its high bits cleared - and the
 *     *minimal* failing case is reported, not the one that happened to be
 *     generated;
 *   - a bound: every campaign has a fixed iteration count, so a property that
 *     loops forever inside the code under test is caught by CTest's timeout.
 *
 * Covers P9-09 and VFS-03 (nothing the peer sends is trusted) and ROB-06 (no
 * out-of-bounds read, which the sanitizer build turns into a hard failure).
 */
#ifndef PGW_TESTS_PROP_H
#define PGW_TESTS_PROP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROP_MAX_BYTES 1024u
/* Bytes written after `cap` by an encoder are a violation; the guard makes one
 * visible without a sanitizer build. */
#define PROP_GUARD 32u
#define PROP_GUARD_BYTE 0xAAu

/* --- the generator -------------------------------------------------------- */

typedef struct prop_rng {
    uint64_t s;
} prop_rng;

void prop_seed(prop_rng *r, uint64_t seed);
uint64_t prop_u64(prop_rng *r);
uint32_t prop_u32(prop_rng *r);
/* A uniform value in [0, bound); 0 when bound is 0. */
uint32_t prop_below(prop_rng *r, uint32_t bound);

/* One generated case. `bytes`/`len` carry a message or a payload, the three
 * integers are free knobs, `tag` labels the case in a failure report. */
typedef struct prop_case {
    uint8_t bytes[PROP_MAX_BYTES];
    size_t len;
    uint64_t value;
    uint32_t a;
    uint32_t b;
    char tag[48];
} prop_case;

/* Fill a case from the generator. Must be deterministic in `r` alone. */
typedef void (*prop_gen)(prop_rng *r, prop_case *c);

/* The property: true when the case satisfies it. Must depend on the case
 * only, so that shrinking a field cannot make it hold by accident. */
typedef bool (*prop_pred)(const prop_case *c);

/* --- the runner ----------------------------------------------------------- */

/* Run `iters` generated cases. On the first failure, shrink the case and
 * abandon the test case with the minimal reproducer (seed, iteration, length,
 * knobs and a hex dump). */
void prop_forall(const char *name, prop_gen gen, prop_pred pred, uint32_t iters,
                 uint64_t seed);

/* Iteration count from the environment (long campaigns), or `dflt`. */
uint32_t prop_iters(const char *var, uint32_t dflt);

/* Fill `n` bytes of `c->bytes` with generator noise, starting at `off`. */
void prop_fill(prop_rng *r, prop_case *c, size_t off, size_t n);

/* A NUL-terminated name of `len` characters in `out` (truncated to `cap`).
 * Generators pass a length above the protocol limit on purpose: the refusal is
 * the property under test, never a crash and never a silent truncation. */
void prop_name(char *out, size_t cap, size_t len);

/* Buffer guard: PROP_GUARD bytes of PROP_GUARD_BYTE are written just past the
 * capacity an encoder is given, so a write outside the buffer is visible
 * without a sanitizer build. `arm` before the call, `ok` after. */
void prop_guard_arm(uint8_t *buf, size_t cap);
bool prop_guard_ok(const uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PGW_TESTS_PROP_H */
