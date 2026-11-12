# SPDX-License-Identifier: Apache-2.0
"""The guest memory a vhost-user backend is given: a memfd plus its mapping.

Implements requirements:
  - VFS-06: configuring shared memory for the virtiofs device, here without
    QEMU - the front end *is* the hypervisor side of the socket.
  - ROB-06: sizes are checked against the page size and the region bounds.

One memfd is created, truncated to the requested size and mapped once. The
mapping address is what the front end reports as ``userspace_addr`` in the
SET_MEM_TABLE region: the backend translates the vring addresses it receives
through that value (``vmm_va_to_gpa`` in the ``vhost`` crate), so the two
processes agree on a single address space for the rings while the buffers
themselves are addressed by guest physical address.
"""

from __future__ import annotations

import ctypes
import gc
import mmap
import os

from vhost.protocol import MemoryRegion, ProtocolError, encode_memory_table

PAGE_SIZE = 4096


def round_page(size: int) -> int:
    """Round *size* up to a whole number of pages."""
    return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)


class SharedMemory:
    """A memfd-backed shared region, mapped read/write in this process."""

    def __init__(self, size: int, name: str = "pgw-vhost-user") -> None:
        size = round_page(size)
        if size <= 0:
            raise ProtocolError("a shared region must have a positive size")
        self.size = size
        self.fd = os.memfd_create(name)
        try:
            os.ftruncate(self.fd, size)
            self._map = mmap.mmap(self.fd, size)
        except OSError:
            os.close(self.fd)
            raise
        # ctypes.from_buffer exports the mapping, which would block close();
        # the reference is dropped first so the region can be released.
        self._ref = ctypes.c_char.from_buffer(self._map)
        self.address = ctypes.addressof(self._ref)
        self.closed = False

    # --- views -------------------------------------------------------------
    @property
    def buffer(self) -> mmap.mmap:
        """The mapping itself: a read/write buffer that exports nothing.

        Callers must not keep a ``memoryview`` of it alive across ``close()``;
        the virtqueue stores this object and slices it on demand instead.
        """
        return self._map

    def va_of(self, offset: int) -> int:
        """Front-end virtual address of *offset* (what SET_VRING_ADDR carries)."""
        self._check(offset, 0)
        return self.address + offset

    def offset_of(self, address: int) -> int:
        """Offset of a front-end virtual address inside the region."""
        offset = address - self.address
        if offset < 0 or offset > self.size:
            raise ProtocolError("address 0x%x is outside the shared region" % address)
        return offset

    def gpa_of(self, offset: int, guest_phys_addr: int = 0) -> int:
        """Guest physical address of *offset* (what descriptors carry)."""
        self._check(offset, 0)
        return guest_phys_addr + offset

    def _check(self, offset: int, length: int) -> None:
        if offset < 0 or length < 0 or offset + length > self.size:
            raise ProtocolError(
                "range [%d, %d) is outside the %d-byte region"
                % (offset, offset + length, self.size)
            )

    # --- protocol ----------------------------------------------------------
    def region(self, guest_phys_addr: int = 0, mmap_offset: int = 0) -> MemoryRegion:
        """The SET_MEM_TABLE entry describing this whole region."""
        return MemoryRegion(
            guest_phys_addr=guest_phys_addr,
            memory_size=self.size,
            user_addr=self.address,
            mmap_offset=mmap_offset,
        )

    def memory_table(self, guest_phys_addr: int = 0) -> bytes:
        """The SET_MEM_TABLE payload for this single region."""
        return encode_memory_table([self.region(guest_phys_addr)])

    # --- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Unmap and close the memfd (idempotent)."""
        if self.closed:
            return
        self.closed = True
        self._ref = None  # release the ctypes export of the mapping
        try:
            self._map.close()
        except BufferError:
            gc.collect()  # a temporary memoryview is still alive somewhere
            self._map.close()
        os.close(self.fd)

    def __enter__(self) -> "SharedMemory":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:  # pragma: no cover - interpreter shutdown path
        try:
            self.close()
        except Exception:
            pass
