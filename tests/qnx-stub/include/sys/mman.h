/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/qnx-stub/include/sys/mman.h - stub of <sys/mman.h> for the PC syntax
 * check of the QNX sources.
 *
 * The PC build compiles the `*_qnx.c` files against these stubs so a broken
 * QNX file is caught without the SDP. Only the symbols the drivers use are
 * declared here.
 */
#ifndef PGW_STUB_SYS_MMAN_H
#define PGW_STUB_SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NOFD (-1)

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_NOCACHE 0x1000

#define MAP_ANON 0x1000
#define MAP_PHYS 0x2000
#define MAP_SHARED 0x10000

#define MAP_FAILED ((void *)-1)

extern void *mmap(void *addr, size_t len, int prot, int flags, int fd,
                   off64_t offset);
extern int munmap(void *addr, size_t len);
extern int mem_offset64(const void *addr, int fd, size_t len, off64_t *off,
                        size_t *contig);

#endif /* PGW_STUB_SYS_MMAN_H */
