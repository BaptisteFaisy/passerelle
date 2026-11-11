/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal stand-in for the QNX 8.0 <sys/neutrino.h> declarations used by the
 * virtio-pci transport. Declarations only; see include/pci/pci.h for why.
 */
#ifndef FS9P_STUB_SYS_NEUTRINO_H
#define FS9P_STUB_SYS_NEUTRINO_H

#include <stdint.h>

#define EOK 0
#define _NTO_INTR_WAIT_FLAGS_UNMASK 0x00000001u
#define _NTO_INTR_FLAGS_TRK_MSK 0x00000001u

struct sigevent;

typedef int32_t rcvid_t;
typedef int32_t pid_t_stub;

int InterruptAttachThread(int intr, unsigned flags);
int InterruptDetach(int id);
int InterruptWait(int flags, const struct sigevent *event);
int InterruptUnmask(int intr, int id);
int ChannelCreate(unsigned flags);
int ConnectAttach(uint32_t node, int32_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);

#endif /* FS9P_STUB_SYS_NEUTRINO_H */
