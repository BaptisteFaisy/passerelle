/* SPDX-License-Identifier: Apache-2.0 */
/* Stand-in for the QNX 8.0 <sys/cpuinline.h> declaration used by the transport. */
#ifndef FS9P_STUB_SYS_CPUINLINE_H
#define FS9P_STUB_SYS_CPUINLINE_H

/* Full system memory barrier (dmb sy on aarch64, mfence on x86_64). It is
 * what orders the available-ring index store against the notification write,
 * which goes to device memory and is not ordered by a C11 fence. */
void __cpu_membarrier(void);

#endif /* FS9P_STUB_SYS_CPUINLINE_H */
