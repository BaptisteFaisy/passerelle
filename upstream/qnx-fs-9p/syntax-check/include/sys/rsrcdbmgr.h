/* SPDX-License-Identifier: Apache-2.0 */
/* Stand-in for the QNX 8.0 <sys/rsrcdbmgr.h> declarations used by the transport. */
#ifndef FS9P_STUB_SYS_RSRCDBMGR_H
#define FS9P_STUB_SYS_RSRCDBMGR_H

#include <stdint.h>

#define RSRCDBMGR_IRQ 0x00000001u
#define RSRCDBMGR_FLAG_RANGE 0x00000001u

typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t flags;
    uint32_t length;
} rsrc_request_t;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t flags;
    uint32_t length;
} rsrc_alloc_t;

int rsrcdbmgr_attach(rsrc_request_t *req, int count);
int rsrcdbmgr_detach(rsrc_request_t *req, int count);
int rsrcdbmgr_create(rsrc_alloc_t *alloc, int count);

#endif /* FS9P_STUB_SYS_RSRCDBMGR_H */
