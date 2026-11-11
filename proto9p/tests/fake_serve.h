/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proto9p/tests/fake_serve.h - internal contract between the fake server files.
 *
 * The handlers of the fake server are split over three translation units so
 * that no file grows past the project limit:
 *   - fake_serve.c       version, attach, walk, clunk, flush and the data path;
 *   - fake_serve_meta.c  the metadata and namespace messages;
 *   - fake_dispatch.c    the type -> handler table.
 *
 * Everything they share is here: the Linux errno numbers that a real
 * 9P2000.L server puts in Rlerror, the handler signature, and the two reply
 * helpers (a lone qid, and the dirent type byte).
 */
#ifndef PGW_FAKE_SERVE_H
#define PGW_FAKE_SERVE_H

#include "fake_server.h"

#include "pgw/buf.h"

#include <stddef.h>
#include <stdint.h>

/* Linux errno values, as they appear on the wire. */
#define L_EPERM     1u
#define L_ENOENT    2u
#define L_EIO       5u
#define L_EBADF     9u
#define L_EACCES    13u
#define L_EEXIST    17u
#define L_ENOTDIR   20u
#define L_EISDIR    21u
#define L_EINVAL    22u
#define L_EMFILE    24u
#define L_EFBIG     27u
#define L_ENOSPC    28u
#define L_ENOSYS    38u
#define L_ENOTEMPTY 39u

typedef pgw_err (*fake_fn)(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                           size_t cap, size_t *rx_len);

/* One line of the dispatch table: a T-message type and its handler. */
typedef struct fake_entry {
    uint8_t type;
    fake_fn fn;
} fake_entry;

/* The `d_type` byte of a readdir record (Linux DT_* value). */
uint8_t fake_dirent_type(const fake_node *n);

/* Reply that is a lone qid[13] (Rattach, Rmkdir, Rsymlink). */
pgw_err fake_reply_qid(fake_srv *fs, uint32_t node, uint8_t type, uint16_t tag,
                       void *rx, size_t cap, size_t *rx_len);

/* --- handlers ------------------------------------------------------------ */

pgw_err fake_h_version(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len);
pgw_err fake_h_attach(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                      size_t cap, size_t *rx_len);
pgw_err fake_h_walk(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                    size_t cap, size_t *rx_len);
pgw_err fake_h_clunk(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                     size_t cap, size_t *rx_len);
pgw_err fake_h_flush(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                     size_t cap, size_t *rx_len);
pgw_err fake_h_lopen(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                     size_t cap, size_t *rx_len);
pgw_err fake_h_lcreate(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len);
pgw_err fake_h_read(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                    size_t cap, size_t *rx_len);
pgw_err fake_h_write(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                     size_t cap, size_t *rx_len);
pgw_err fake_h_readdir(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len);
pgw_err fake_h_fsync(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                     size_t cap, size_t *rx_len);
pgw_err fake_h_getattr(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len);
pgw_err fake_h_setattr(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len);
pgw_err fake_h_mkdir(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                     size_t cap, size_t *rx_len);
pgw_err fake_h_unlinkat(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                        size_t cap, size_t *rx_len);
pgw_err fake_h_renameat(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                        size_t cap, size_t *rx_len);
pgw_err fake_h_symlink(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                       size_t cap, size_t *rx_len);
pgw_err fake_h_readlink(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                        size_t cap, size_t *rx_len);
pgw_err fake_h_link(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                    size_t cap, size_t *rx_len);
pgw_err fake_h_statfs(fake_srv *fs, uint16_t tag, pgw_buf *req, void *rx,
                      size_t cap, size_t *rx_len);

#endif /* PGW_FAKE_SERVE_H */
