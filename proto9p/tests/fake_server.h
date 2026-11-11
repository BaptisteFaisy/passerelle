/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/fake_server.h - an in-memory 9P2000.L server for the tests.
 *
 * The protocol layer must be testable without QEMU, without QNX and without a
 * disk (P9-10). This fake server answers every message of the contract from a
 * small simulated tree that lives in RAM, so a test can drive a real session
 * end to end and check both the bytes on the wire and the state of the tree.
 *
 * It is deliberately strict in the same way a real server is: a request whose
 * size[4] does not match the bytes received is rejected, and every reply is
 * built with the same pgw_out cursors as the library.
 */
#ifndef PGW_FAKE_SERVER_H
#define PGW_FAKE_SERVER_H

#include "pgw/buf.h"
#include "pgw/p9.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAKE_NODES 512
#define FAKE_NAME  64
#define FAKE_FIDS  256
#define FAKE_LINK  128
#define FAKE_BIG_SIZE (300u * 1024u) /* size of big.bin */
#define FAKE_MANY_COUNT 300u        /* entries in the "many" directory */

typedef struct fake_node {
    bool used;
    bool is_dir;
    uint8_t qt;      /* qid type bits (PGW_P9_QTDIR, ...) */
    uint32_t parent;
    char name[FAKE_NAME];
    char link[FAKE_LINK];
    uint8_t *data;
    size_t size;
    size_t cap;
    uint32_t mode; /* Linux mode, type bits included */
    uint64_t path; /* qid.path */
} fake_node;

typedef struct fake_fid {
    bool used;
    uint32_t node;
    uint32_t flags; /* Linux open flags seen in Tlopen/Tlcreate */
} fake_fid;

typedef struct fake_srv {
    fake_node nodes[FAKE_NODES];
    fake_fid fids[FAKE_FIDS];
    uint32_t msize_cap; /* highest msize the server accepts */
    char version[16];
    uint64_t next_path;
    size_t calls;
    size_t per_type[256];
    uint32_t max_tag;
    uint8_t fail_type; /* answer `fail_type` with Rlerror(fail_errno) once */
    uint32_t fail_errno;
    uint8_t drop_type; /* pretend `drop_type` never got an answer, once */
    uint8_t wrong_type; /* answer `wrong_type` with `wrong_reply`, once */
    uint8_t wrong_reply; /* the bogus reply type used above */
    bool trailing; /* append one byte past size[4], once */
    uint32_t last_timeout_ms; /* timeout handed to the transport */
    uint16_t last_tag;        /* tag of the last request seen */
    void (*hook)(struct fake_srv *fs, uint8_t type, void *user);
    void *hook_user;
} fake_srv;

fake_srv *fake_new(uint32_t msize_cap);
void fake_free(fake_srv *fs);

/* The pgw_p9_rpc_fn handed to pgw_p9_session_open(). */
pgw_err fake_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
                 size_t *rx_len, uint32_t prio, uint32_t timeout_ms,
                 void *user);

/* --- inspection ---------------------------------------------------------- */

size_t fake_calls(const fake_srv *fs);
size_t fake_calls_of(const fake_srv *fs, uint8_t type);
void fake_reset_counters(fake_srv *fs);
uint32_t fake_max_tag(const fake_srv *fs);

/* Number of fids the server still holds: catches a client that leaks. */
size_t fake_open_fids(const fake_srv *fs);

/* Copy the content of `path` (relative to the root) into `buf`.
 * Returns the length, or (size_t)-1 when the path does not exist. */
size_t fake_read_file(const fake_srv *fs, const char *path, void *buf,
                      size_t cap);

/* --- tree helpers shared with fake_serve.c ------------------------------- */

uint32_t fake_node_add(fake_srv *fs, uint32_t parent, const char *name,
                       bool is_dir, uint32_t mode);
int32_t fake_node_find(const fake_srv *fs, uint32_t dir, const char *name,
                       size_t len);
bool fake_node_store(fake_srv *fs, uint32_t node, size_t off,
                     const uint8_t *data, size_t len);
pgw_p9_qid fake_node_qid(const fake_srv *fs, uint32_t node);
void fake_node_attr(const fake_srv *fs, uint32_t node, pgw_p9_attr *out);

/* --- reply helpers ------------------------------------------------------- */

bool fake_begin(pgw_out *w, void *rx, size_t cap, uint8_t type, uint16_t tag);
void fake_end(pgw_out *w, size_t *rx_len);
pgw_err fake_reply_error(void *rx, size_t cap, size_t *rx_len, uint16_t tag,
                         uint32_t linux_errno);

/* Dispatcher over the per-message handlers (fake_serve.c). */
pgw_err fake_dispatch(fake_srv *fs, const pgw_p9_hdr *h, pgw_buf *req,
                      void *rx, size_t cap, size_t *rx_len);

/* --- fid table ----------------------------------------------------------- */

bool fake_fid_ok(const fake_srv *fs, uint32_t fid, uint32_t *node);
bool fake_fid_set(fake_srv *fs, uint32_t fid, uint32_t node, uint32_t flags);
void fake_fid_clear(fake_srv *fs, uint32_t fid);

#endif /* PGW_FAKE_SERVER_H */
