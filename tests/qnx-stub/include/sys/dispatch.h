/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/dispatch.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirements: QNX-01 (the dispatch layer receives the messages),
 * QNX-03 (a thread serves the request at the caller's priority), QNX-05 (a
 * thread pool: a blocked client never pins the only thread).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, dispatch_create() and
 * thread_pool_create(); QNX 8.0 introduced dispatch_create_channel() which the
 * drivers use when they own the channel.
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create.html
 */
#ifndef PGW_STUB_SYS_DISPATCH_H
#define PGW_STUB_SYS_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>

/* The per-thread receive buffer, filled by dispatch_block() and handed to every
 * handler by dispatch_handler(). */
typedef struct _dispatch_context {
    resmgr_context_t ctp;
    void *msg;
    uint32_t msg_size;
    void *reserved[4];
} dispatch_context_t;

typedef struct _dispatch dispatch_t;

typedef struct _thread_pool thread_pool_t;

/* The pool passes its work item to every callback under THREAD_POOL_PARAM_T.
 * QNX 8's <sys/resmgr.h> defaults it to resmgr_context_t; the drivers pin it to
 * dispatch_context_t (before including the headers) so the dispatch layer's own
 * dispatch_* functions type-check, exactly as the QNX 8 resource-manager
 * examples do. */
#ifndef THREAD_POOL_PARAM_T
#define THREAD_POOL_PARAM_T dispatch_context_t
#endif

#ifndef THREAD_POOL_HANDLE_T
#define THREAD_POOL_HANDLE_T dispatch_t
#endif

typedef struct _thread_pool_attr {
    THREAD_POOL_HANDLE_T *handle;
    THREAD_POOL_PARAM_T *(*block_func)(THREAD_POOL_PARAM_T *ctp);
    void (*unblock_func)(THREAD_POOL_PARAM_T *ctp);
    int (*handler_func)(THREAD_POOL_PARAM_T *ctp);
    THREAD_POOL_PARAM_T *(*context_alloc)(THREAD_POOL_HANDLE_T *handle);
    void (*context_free)(THREAD_POOL_PARAM_T *ctp);
    void *attr; /* pthread_attr_t * on target */
    unsigned short lo_water;
    unsigned short increment;
    unsigned short hi_water;
    unsigned short maximum;
    uint32_t reserved[2];
} thread_pool_attr_t;

#define POOL_FLAG_EXIT_SELF   0x00000001u
#define POOL_FLAG_USE_SELF    0x00000002u
#define POOL_FLAG_NO_ATTR_SYNC 0x00000004u

dispatch_t *dispatch_create(void);
dispatch_t *dispatch_create_channel(int chid, unsigned flags);
int dispatch_destroy(dispatch_t *dpp);

dispatch_context_t *dispatch_context_alloc(dispatch_t *dpp);
void dispatch_context_free(dispatch_context_t *ctp);
dispatch_context_t *dispatch_block(dispatch_context_t *ctp);
int dispatch_handler(dispatch_context_t *ctp);
void dispatch_unblock(dispatch_context_t *ctp);

/* Let the kernel deliver a pulse to this process: used to wake the shutdown
 * thread and, for virtiofs, to route the FORGET/INTERRUPT queue (VFS-02). */
int dispatch_get_pulse(dispatch_t *dpp, int code, int value);

thread_pool_t *thread_pool_create(thread_pool_attr_t *attr, unsigned flags);
int thread_pool_destroy(thread_pool_t *pool);
int thread_pool_start(thread_pool_t *pool);
int thread_pool_control(thread_pool_t *pool, thread_pool_attr_t *attr,
                        unsigned lower, unsigned upper, unsigned always);

#endif /* PGW_STUB_SYS_DISPATCH_H */
