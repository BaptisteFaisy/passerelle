/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Stand-in for the QNX 8.0 <sys/resmgr.h> types that src/resmgr.h names.
 * Complete types with one placeholder member each, for syntax only.
 */
#ifndef FS9P_STUB_SYS_RESMGR_H
#define FS9P_STUB_SYS_RESMGR_H

#include <sys/types.h>

typedef struct {
    int stub;
} resmgr_connect_funcs_t;

typedef struct {
    int stub;
} resmgr_io_funcs_t;

typedef struct {
    int stub;
} resmgr_context_t;

#endif /* FS9P_STUB_SYS_RESMGR_H */
