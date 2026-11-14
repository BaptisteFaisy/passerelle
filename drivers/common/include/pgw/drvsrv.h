/* SPDX-License-Identifier: Apache-2.0 */
/*
 * pgw/drvsrv.h - the QNX resource-manager server shared by both drivers.
 *
 * Both drivers are filesystem resource managers over the same protocol-neutral
 * layer (`pgw_rmfs`), so everything that turns a QNX message into an rmfs call
 * lives here once: the node/OCB pair, the io/connect handler tables, the
 * attribute conversion, the caller-priority boost, the interrupt service
 * thread, the kernel-trace events, the system-log backend and the privilege
 * drop. A driver only supplies its transport (9P or virtiofs) and its
 * protocol-specific policies.
 *
 * This header pulls QNX headers, so only `*_qnx.c` files include it: the PC
 * build compiles those against tests/qnx-stub/include (pgw_qnx_syntax).
 *
 * Covered requirements: QNX-01, QNX-03, QNX-05, QNX-06, QNX-08, QNX-09, FS-09,
 * FS-11, SEC-01, SEC-02, VIO-04.
 */
#ifndef PGW_DRVSRV_H
#define PGW_DRVSRV_H

#include "pgw/drvopts.h"
#include "pgw/err.h"
#include "pgw/limits.h"
#include "pgw/rmfs.h"
#include "pgw/traceop.h" /* the operation numbering, testable without QNX */

#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <sys/iofunc.h>  /* first: it sets RESMGR_OCB_T before dispatch/resmgr */
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>

/* --- node and open control block ----------------------------------------- */

/* One node per open. It owns the backend handle, so a file removed on the PC
 * stays readable through it (FS-11), and its attributes are the ones the
 * iofunc layer reports (FS-02). Nodes are never shared: the PC is the truth and
 * can change a file behind our back (FS-12). */
typedef struct pgw_node {
    iofunc_attr_t attr; /* must be first: an OCB's attr points here */
    pgw_rmfs_fd *fd;    /* NULL until the open reached the backend */
    bool writable;
    uint32_t ocbs; /* OCBs bound to this node; a mapping adds one */
    char path[PGW_PATH_MAX];
} pgw_node;

typedef struct pgw_ocb {
    iofunc_ocb_t ocb; /* must be first: the iofunc defaults cast back */
    pgw_node *node;
} pgw_ocb;

/* --- server --------------------------------------------------------------- */

typedef struct pgw_srv {
    pgw_rmfs *fs;
    const pgw_mount_opts *opts;
    const char *name; /* "fs9p" or "fsvirtiofs", for logs and counters */
    dispatch_t *dpp;
    resmgr_attr_t rattr;
    resmgr_connect_funcs_t connect;
    resmgr_io_funcs_t io;
    iofunc_mount_t mount;
    iofunc_funcs_t mount_funcs;
    iofunc_attr_t root_attr;
    int mntid;
    thread_pool_t *pool;
} pgw_srv;

/* Create the dispatch channel and the POSIX-layer tables. */
pgw_err pgw_srv_init(pgw_srv *srv, pgw_rmfs *fs, const pgw_mount_opts *opts,
                     const char *name);

/* Register the mount point for the whole subtree below it (QNX-01) and record
 * the mount flags the process manager will see (SEC-01). */
pgw_err pgw_srv_attach(pgw_srv *srv, const char *mount_point);

/* Start the thread pool and serve until a termination signal. `threads` is the
 * maximum pool size: more threads than requests in flight, so a client blocked
 * on the transport never pins the only thread (QNX-05). Returns only on error. */
pgw_err pgw_srv_serve(pgw_srv *srv, uint32_t threads);

/* Detach the path and destroy the channel (QNX-07, VIO-09). */
pgw_err pgw_srv_detach(pgw_srv *srv);

/* The server of this process: one mount per process (VIO-06 is met by running
 * one instance per share). */
pgw_srv *pgw_srv_current(void);

/* Allocate a node for an open and bind an OCB to it. On success the node is
 * owned by its OCBs and freed when the last one goes. */
pgw_err pgw_node_open(resmgr_context_t *ctp, io_open_t *msg, const char *path,
                      pgw_rmfs_fd *fd, const pgw_fs_attr *a, bool writable);

/* The node whose last OCB just went, handed to the close handler; NULL when
 * another OCB (a mapping) still holds it. */
pgw_node *pgw_node_released(void);
void pgw_node_dispose(pgw_node *node);

/* The node of a message: our structure always starts with the iofunc one. */
static inline pgw_node *pgw_node_of(RESMGR_OCB_T *ocb)
{
    return (pgw_node *)(void *)ocb->attr;
}

/* --- attribute conversion (FS-02) ---------------------------------------- */

void pgw_attr_from_fs(iofunc_attr_t *attr, const pgw_fs_attr *a,
                      iofunc_mount_t *mount);

/* --- caller identity and priority (FS-09, SEC-01, QNX-03) ---------------- */

/* Fill `cred` with the caller's identity and priority. Fails (and returns an
 * error) when the kernel cannot identify the client, so every caller refuses
 * the operation instead of guessing (FS-09, SEC-01). */
pgw_err pgw_cred_from_ctp(resmgr_context_t *ctp, pgw_fs_cred *cred);

/* pgw_err -> this build's errno. An unmapped code stays an error (ROB-01). */
int pgw_qnx_status(pgw_err e);

/* True when the client asked to be unblocked (signal or timeout) or went away:
 * a long transfer checks it between chunks (QNX-05). */
bool pgw_client_unblocking(resmgr_context_t *ctp);

typedef struct pgw_prio_state {
    int policy;
    struct sched_param param;
    bool changed;
} pgw_prio_state;

/* Run the request at the caller's priority (QNX-03). */
void pgw_prio_enter(resmgr_context_t *ctp, pgw_prio_state *st);
void pgw_prio_leave(pgw_prio_state *st);

/* --- handler tables ------------------------------------------------------- */

void pgw_resmgr_io_init(resmgr_io_funcs_t *io);
void pgw_resmgr_connect_init(resmgr_connect_funcs_t *connect);
/* Attribute-changing handlers (chmod, chown, utime, truncate). */
void pgw_resmgr_meta_init(resmgr_io_funcs_t *io);
/* The mount() callout (QNX-07). */
void pgw_resmgr_mount_init(resmgr_connect_funcs_t *connect);

int pgw_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb);
int pgw_io_read_file(resmgr_context_t *ctp, io_read_t *msg, pgw_ocb *ocb,
                     uint32_t xtype);
int pgw_io_readdir(resmgr_context_t *ctp, io_read_t *msg, pgw_ocb *ocb);

/* --- kernel trace (QNX-09) ------------------------------------------------ */

/* The operation numbering itself lives in pgw/traceop.h: a table and a packing
 * rule, no OS header, so the PC bench checks them (test_traceop.c). */

void pgw_trace_enable(bool on);
bool pgw_trace_enabled(void);
/* One user trace event (low level). */
void pgw_trace_emit(int event, int data0, int data1);
/* The pair every request produces: one at submission, one at reception.
 * `tag` is the 9P tag or the FUSE unique id, so a reader can pair them. The
 * submission event also carries the priority the serving thread runs at
 * (QNX-03); see pgw_trace_pack(). */
void pgw_trace_req(enum pgw_op op, uint32_t tag);
void pgw_trace_rep(enum pgw_op op, uint32_t tag, int32_t status);

/* Largest single read/write handed to the backend in one call: bounded so a
 * huge client request is split into chunks and the thread stays responsive to
 * an unblock request between them (QNX-05). */
#define PGW_IO_CHUNK (64u * 1024u)

/* --- system log and counters (QNX-08) ------------------------------------ */

pgw_err pgw_slog2_open(const char *set_name);
void pgw_slog2_close(void);
/* devctl(DCMD_PGW_STATS) answer: the counter table as `name=value` lines. */
int pgw_stats_devctl(resmgr_context_t *ctp, io_devctl_t *msg);
#define PGW_DCMD_STATS 0x0000d7a1

/* --- privilege drop (QNX-06, SEC-01) ------------------------------------- */

typedef struct pgw_ability {
    unsigned aid;    /* PROCMGR_AID_* */
    const char *why; /* documented reason, logged once at startup */
} pgw_ability;

/* Allow exactly the listed abilities, lock them, then leave root (or become
 * `user` when given). Refuses to serve if root privileges remain (QNX-06). */
pgw_err pgw_privdrop_apply(const pgw_ability *list, size_t n, const char *user);

/* --- replies popped by another waiter (VIO-02) ---------------------------- */

/* One virtqueue serves every client: a reply can be popped by the thread that
 * waits for another one. The table keeps the early replies until their owner
 * comes for them; without it, two clients lose each other's replies. */
void pgw_reqtab_keep(void *cookie, uint32_t len);
bool pgw_reqtab_take(void *cookie, uint32_t *out_len);
void pgw_reqtab_clear(void);

/* --- DMA addresses (VIO-07) ---------------------------------------------- */

/* Physical address of `len` bytes at `addr`, for a device that reads or
 * writes them. PGW_EIO when the range is not physically contiguous. */
pgw_err pgw_qnx_phys(void *addr, size_t len, uint64_t *out_pa);

/* Memory a device can read and write: physically contiguous and non-cached.
 * The pair is the allocator the 9P and virtiofs sessions use for their
 * request and reply buffers (VIO-07). */
void *pgw_qnx_alloc_dma(size_t bytes, void *user);
void pgw_qnx_free_dma(void *ptr, size_t bytes, void *user);

/* --- interrupt service thread (VIO-04) ---------------------------------- */

/* Sleep `ms` milliseconds. delay() is a _QNX_SOURCE-gated extension; nanosleep()
 * is POSIX, which both the target and the host build expose. The busy-wait loops
 * below use it to yield to the CPU instead of spinning (VIO-04, ROB-05). */
static inline void pgw_relax_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u),
                           (long)(ms % 1000u) * 1000000L };
    (void)nanosleep(&ts, NULL);
}

/* QNX 8.0 removed InterruptAttach(): a thread is attached with
 * InterruptAttachThread() and becomes the Interrupt Service Thread. */
typedef struct pgw_ist {
    int irq;
    int id;
    volatile bool running;
    void (*on_irq)(void *user);
    void *user;
} pgw_ist;

pgw_err pgw_ist_start(pgw_ist *ist, int irq, void (*on_irq)(void *user),
                      void *user);
void pgw_ist_stop(pgw_ist *ist);

#endif /* PGW_DRVSRV_H */
