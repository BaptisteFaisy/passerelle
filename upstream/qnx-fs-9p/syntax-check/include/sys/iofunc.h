/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Stand-in for the QNX 8.0 <sys/iofunc.h> types that src/resmgr.h names in
 * its structures. Complete types with placeholder members: the point is to let
 * main.c compile on the PC, not to model iofunc. On QNX this header is what
 * pulls in <sys/resmgr.h> and <sys/dispatch.h>, so the stand-in does too.
 */
#ifndef FS9P_STUB_SYS_IOFUNC_H
#define FS9P_STUB_SYS_IOFUNC_H

#include <sys/dispatch.h>
#include <sys/resmgr.h>
#include <sys/types.h>

typedef struct {
    int stub;
} iofunc_attr_t;

typedef struct {
    int stub;
} iofunc_mount_t;

typedef struct {
    int stub;
} iofunc_funcs_t;

typedef struct {
    iofunc_attr_t *attr; /* first member on QNX, and what fs9p_node() reads */
} iofunc_ocb_t;

#endif /* FS9P_STUB_SYS_IOFUNC_H */
