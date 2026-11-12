/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ref9p/ref9p_fields.h - the independent reader used by the wire-format
 * benches.
 *
 * A conformance test is only worth something if its oracle does not share code
 * with the thing it checks. These accessors read and write the 9P2000.L fields
 * straight from the bytes, at the offsets of the normative message table, and
 * deliberately do not use pgw_buf: if pgw_buf and the reference server ever
 * disagree about where a field lives, the benches that use this header say so.
 *
 * The helpers also own the raw conversation with the reference server (one
 * frame per message, no session), so both wire benches drive it identically.
 */
#ifndef REF9P_FIELDS_H
#define REF9P_FIELDS_H

#include "ref9p_harness.h"

#include "pgw/p9.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REF9P_FIELD_TAG 7u
#define REF9P_NOTAG 0xFFFFu
#define REF9P_RX_CAP 16384u
#define REF9P_TX_CAP 4096u

/* Shared scratch buffers: the benches are single-threaded. */
extern uint8_t ref9p_rx[REF9P_RX_CAP];
extern uint8_t ref9p_tx[REF9P_TX_CAP];

/* --- the oracle: little-endian field access at absolute offsets ------------ */
uint8_t ref9p_o_u8(const uint8_t *p, size_t off);
uint16_t ref9p_o_u16(const uint8_t *p, size_t off);
uint32_t ref9p_o_u32(const uint8_t *p, size_t off);
uint64_t ref9p_o_u64(const uint8_t *p, size_t off);
void ref9p_o_put_u8(uint8_t *p, size_t off, uint8_t v);
void ref9p_o_put_u16(uint8_t *p, size_t off, uint16_t v);
void ref9p_o_put_u32(uint8_t *p, size_t off, uint32_t v);
void ref9p_o_put_u64(uint8_t *p, size_t off, uint64_t v);

/* A string at `off`; returns the offset just past it. */
size_t ref9p_o_put_str(uint8_t *p, size_t off, const char *s);

/* Patch size[4] once the message is complete; returns its length. */
size_t ref9p_o_seal(uint8_t *p, size_t len);

/* size[4] must equal the bytes received, and type/tag must be the expected
 * ones: the three fields every 9P message starts with. */
void ref9p_expect_header(const uint8_t *msg, size_t len, uint8_t type, uint16_t tag);

/* Send one request built in ref9p_tx and receive the reply into ref9p_rx.
 * Returns the number of bytes received. */
size_t ref9p_call(struct ref9p_server *srv, const void *tx, size_t tx_len);

/* Start a server over a fresh directory holding hello.txt, sub/nested.txt and
 * the symlink alias -> hello.txt, then run Tversion and Tattach by hand.
 * Fid 0 is the attached root. */
struct ref9p_server *ref9p_fields_start(void);

/* Walk fid 0 to `name` into `newfid`, and open a fid with Linux flags. */
size_t ref9p_walk_to(struct ref9p_server *srv, uint32_t newfid, const char *name);
void ref9p_lopen(struct ref9p_server *srv, uint32_t fid, uint32_t linux_flags);

#ifdef __cplusplus
}
#endif

#endif /* REF9P_FIELDS_H */
