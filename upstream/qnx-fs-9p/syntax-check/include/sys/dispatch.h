/* SPDX-License-Identifier: Apache-2.0 */
/* Stand-in for the QNX 8.0 <sys/dispatch.h> declarations used by the transport. */
#ifndef FS9P_STUB_SYS_DISPATCH_H
#define FS9P_STUB_SYS_DISPATCH_H

typedef struct name_attach name_attach_t;
typedef struct dispatch dispatch_t;

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags);
int name_detach(name_attach_t *attach, unsigned flags);

#endif /* FS9P_STUB_SYS_DISPATCH_H */
