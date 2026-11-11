/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * protofuse/tests/fake_virtiofsd.h - In-memory virtiofs server for the tests.
 *
 * Implements requirements:
 *   - VFS-03: a server that speaks the FUSE wire protocol, so that the client
 *     is exercised end to end without QEMU or virtiofsd
 *   - VFS-02: it records, for every request, whether it arrived on the priority
 *     lane and in which order, which is how the queue discipline is asserted
 *   - VFS-04: it counts the references the client gives back
 */
#ifndef PGW_FAKE_VIRTIOFSD_H
#define PGW_FAKE_VIRTIOFSD_H

#include "pgw/err.h"
#include "pgw/fusep.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FV_LOG_MAX 256u
#define FV_NAME_MAX 255u

typedef struct fv_node {
    uint64_t nodeid;
    bool is_dir;
    bool is_link;
    uint32_t mode; /* permission bits only */
    uint64_t size;
    char name[FV_NAME_MAX + 1u];
    char target[FV_NAME_MAX + 1u];
    uint8_t *data;
    size_t cap;
    struct fv_node *parent;
    struct fv_node *child;
    struct fv_node *sibling;
} fv_node;

typedef struct fake_virtiofsd {
    fv_node *root;
    uint64_t next_nodeid;
    uint64_t next_fh;

    /* What INIT will announce. */
    uint32_t major;
    uint32_t minor;
    uint32_t max_write;
    uint32_t max_pages;
    uint64_t flags;

    /* Instrumentation. */
    unsigned calls;
    unsigned hiprio_calls;
    unsigned normal_calls;
    unsigned forget_msgs;    /* FORGET and BATCH_FORGET messages received */
    unsigned forget_batches; /* of which BATCH_FORGET */
    unsigned forgotten_nodes;
    unsigned init_calls;
    uint64_t forgotten_refs; /* sum of the nlookup counts given back */
    uint32_t op_log[FV_LOG_MAX];
    uint64_t unique_log[FV_LOG_MAX];
    bool hiprio_log[FV_LOG_MAX];
    size_t op_log_len;

    /* Fault injection for the next reply only. */
    int32_t force_error;  /* negative Linux errno, or 0 */
    bool drop_reply;      /* answer with zero bytes */
    bool corrupt_unique;  /* echo a different unique */
    bool truncate_reply;  /* declare a length larger than sent */
} fake_virtiofsd;

fake_virtiofsd *fv_new(uint32_t minor, uint32_t max_write);
void fv_free(fake_virtiofsd *f);

/* The pgw_fuse_rpc_fn the session is opened with. */
pgw_err fv_rpc(const void *tx, size_t tx_len, void *rx, size_t rx_cap,
               size_t *rx_len, bool hiprio, uint32_t prio, uint32_t timeout_ms,
               void *user);

/* --- tree helpers used by the tests --------------------------------------- */

fv_node *fv_node_by_id(fake_virtiofsd *f, uint64_t nodeid);
fv_node *fv_child(fake_virtiofsd *f, uint64_t parent, const char *name);
fv_node *fv_add(fake_virtiofsd *f, fv_node *parent, const char *name, bool is_dir);
fv_node *fv_detach(fv_node *parent, const char *name);
bool fv_remove(fake_virtiofsd *f, fv_node *parent, const char *name);
bool fv_set_size(fv_node *n, uint64_t size);
size_t fv_node_count(const fake_virtiofsd *f);
uint64_t fv_path_id(fake_virtiofsd *f, const char *path); /* "/a/b" -> nodeid */

#endif /* PGW_FAKE_VIRTIOFSD_H */
