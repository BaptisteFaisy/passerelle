/* SPDX-License-Identifier: Apache-2.0 */
/* Stand-in for the QNX 8.0 <sys/mman.h> declarations used by the transport. */
#ifndef FS9P_STUB_SYS_MMAN_H
#define FS9P_STUB_SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>

typedef int64_t off64_t;

#define PROT_READ 0x01
#define PROT_WRITE 0x02
#define PROT_NOCACHE 0x10
#define MAP_SHARED 0x01
#define MAP_PHYS 0x02
#define MAP_ANON 0x04
#define MAP_FAILED ((void *)-1)
#define NOFD ((int)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off64_t off);
int munmap(void *addr, size_t len);
void *mmap_device_memory(void *addr, size_t len, int prot, int flags, uint64_t phys);
int munmap_device_memory(void *addr, size_t len);
int mem_offset64(const void *addr, int fd, size_t len, off64_t *off, size_t *contig);

#endif /* FS9P_STUB_SYS_MMAN_H */
