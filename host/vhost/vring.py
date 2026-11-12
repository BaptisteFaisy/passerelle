# SPDX-License-Identifier: Apache-2.0
"""A split virtqueue laid out in shared memory, plus the driver side of it.

Implements requirements:
  - VFS-02: file I/O travels on a standard request queue (the hiprio queue 0 is
    left to FORGET/INTERRUPT), and this module is what posts on it.
  - VFS-03: the queue carries FUSE requests, one read-only descriptor for the
    request and one write-only descriptor for the reply.
  - ROB-06: every index is masked to the ring size and every range is checked
    against the region, so a bogus used entry raises instead of being copied.

The layout is the one the virtio specification calls a split virtqueue: a
descriptor table (16 bytes per entry, 16-byte aligned), an available ring
(2-byte aligned) and a used ring (4-byte aligned). The three addresses are
reported to the backend with SET_VRING_ADDR as front-end virtual addresses;
the buffer addresses inside the descriptors are guest physical addresses.
"""

from __future__ import annotations

import os
import select
import struct

from vhost.memory import SharedMemory
from vhost.protocol import ProtocolError

DESC_SIZE = 16
DESC_ALIGN = 16
AVAIL_ALIGN = 2
USED_ALIGN = 4

DESC_F_NEXT = 0x1
DESC_F_WRITE = 0x2
DESC_F_INDIRECT = 0x4

_DESC = struct.Struct("<QIHH")
_RING_IDX = struct.Struct("<H")
_USED_ELEM = struct.Struct("<II")


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


class SplitVirtqueue:
    """One virtqueue: its rings, its buffers and its kick/call eventfds."""

    def __init__(
        self,
        mem: SharedMemory,
        index: int = 1,
        num: int = 16,
        base_offset: int = 0,
        tx_size: int = 4096,
        rx_size: int = 65536,
        event_idx: bool = False,
        guest_phys_addr: int = 0,
    ) -> None:
        if num < 2 or num & (num - 1):
            raise ProtocolError("a virtqueue size must be a power of two >= 2, got %d" % num)
        self.mem = mem
        self.index = index
        self.num = num
        self.event_idx = event_idx
        self.guest_phys_addr = guest_phys_addr
        self.tx_size = tx_size
        self.rx_size = rx_size
        self._buf = mem.buffer
        self._layout(base_offset)
        self._clear_rings()
        self.kick_fd = os.eventfd(0)
        self.call_fd = os.eventfd(0)
        self._avail_idx = 0
        self._last_used = 0

    # --- layout ------------------------------------------------------------
    def _layout(self, base_offset: int) -> None:
        """Compute the offsets of the three rings and of the buffer slots."""
        self.desc_off = _align(base_offset, DESC_ALIGN)
        self.avail_off = _align(self.desc_off + self.num * DESC_SIZE, AVAIL_ALIGN)
        avail_size = 4 + 2 * self.num + (2 if self.event_idx else 0)
        self.used_off = _align(self.avail_off + avail_size, USED_ALIGN)
        used_size = 4 + 8 * self.num + (2 if self.event_idx else 0)
        self.used_event_off = self.avail_off + 4 + 2 * self.num
        self.avail_event_off = self.used_off + 4 + 8 * self.num
        self.buffers_off = _align(self.used_off + used_size, 4096)
        self.slots = self.num // 2
        needed = self.buffers_off + self.slots * (self.tx_size + self.rx_size)
        if needed > self.mem.size:
            raise ProtocolError("virtqueue needs %d bytes, region has %d" % (needed, self.mem.size))

    def _clear_rings(self) -> None:
        end = self.buffers_off
        self._buf[self.desc_off:end] = b"\x00" * (end - self.desc_off)

    @property
    def desc_addr(self) -> int:
        """Front-end virtual address of the descriptor table."""
        return self.mem.va_of(self.desc_off)

    @property
    def avail_addr(self) -> int:
        """Front-end virtual address of the available ring."""
        return self.mem.va_of(self.avail_off)

    @property
    def used_addr(self) -> int:
        """Front-end virtual address of the used ring."""
        return self.mem.va_of(self.used_off)

    # --- buffers -----------------------------------------------------------
    def slot_stride(self) -> int:
        return self.tx_size + self.rx_size

    def tx_offset(self, slot: int) -> int:
        self._check_slot(slot)
        return self.buffers_off + slot * self.slot_stride()

    def rx_offset(self, slot: int) -> int:
        return self.tx_offset(slot) + self.tx_size

    def tx_buffer(self, slot: int) -> memoryview:
        """The read-only request buffer of *slot*."""
        off = self.tx_offset(slot)
        return memoryview(self._buf)[off:off + self.tx_size]

    def rx_buffer(self, slot: int) -> memoryview:
        """The write-only reply buffer of *slot*."""
        off = self.rx_offset(slot)
        return memoryview(self._buf)[off:off + self.rx_size]

    def _check_slot(self, slot: int) -> None:
        if not 0 <= slot < self.slots:
            raise ProtocolError("slot %d outside 0..%d" % (slot, self.slots - 1))

    # --- ring access -------------------------------------------------------
    def _write_desc(self, index: int, addr: int, length: int, flags: int, nxt: int) -> None:
        off = self.desc_off + index * DESC_SIZE
        _DESC.pack_into(self._buf, off, addr, length, flags, nxt)

    @property
    def avail_idx(self) -> int:
        """The available-ring index this driver has published so far."""
        return self._avail_idx

    def used_idx(self) -> int:
        """The used-ring index as written by the backend."""
        return _RING_IDX.unpack_from(self._buf, self.used_off + 2)[0]

    def set_used_event(self, value: int) -> None:
        """Tell the backend which used index we want to be woken up for."""
        if self.event_idx:
            _RING_IDX.pack_into(self._buf, self.used_event_off, value & 0xFFFF)

    def avail_event(self) -> int:
        """The available index the backend asked to be kicked at."""
        if not self.event_idx:
            return 0
        return _RING_IDX.unpack_from(self._buf, self.avail_event_off)[0]

    # --- posting and harvesting -------------------------------------------
    def post(self, slot: int, tx_len: int, rx_len: int) -> int:
        """Publish *slot* with one request descriptor and one reply descriptor."""
        self._check_slot(slot)
        if not 0 < tx_len <= self.tx_size or not 0 < rx_len <= self.rx_size:
            raise ProtocolError("buffer lengths %d/%d out of range" % (tx_len, rx_len))
        head = 2 * slot
        tx_addr = self.mem.gpa_of(self.tx_offset(slot), self.guest_phys_addr)
        rx_addr = self.mem.gpa_of(self.rx_offset(slot), self.guest_phys_addr)
        self._write_desc(head, tx_addr, tx_len, DESC_F_NEXT, head + 1)
        self._write_desc(head + 1, rx_addr, rx_len, DESC_F_WRITE, 0)
        entry = self.avail_off + 4 + 2 * (self._avail_idx % self.num)
        _RING_IDX.pack_into(self._buf, entry, head)
        self._avail_idx = (self._avail_idx + 1) & 0xFFFF
        _RING_IDX.pack_into(self._buf, self.avail_off + 2, self._avail_idx)
        return head

    def kick(self) -> None:
        """Signal the backend that new buffers are available."""
        os.eventfd_write(self.kick_fd, 1)

    def wait_call(self, timeout: float = 5.0) -> bool:
        """Wait for the backend's call eventfd; True when it fired."""
        if not select.select([self.call_fd], [], [], timeout)[0]:
            return False
        os.eventfd_read(self.call_fd)
        return True

    def harvest(self) -> list[tuple[int, int]]:
        """Every used entry published since the last call, as (head, length)."""
        target = self.used_idx()
        out: list[tuple[int, int]] = []
        while self._last_used != target:
            off = self.used_off + 4 + 8 * (self._last_used % self.num)
            head, length = _USED_ELEM.unpack_from(self._buf, off)
            if head >= self.num:
                raise ProtocolError("backend returned an out-of-range descriptor %d" % head)
            out.append((head, length))
            self._last_used = (self._last_used + 1) & 0xFFFF
        return out

    # --- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Close the two eventfds (idempotent)."""
        for name in ("kick_fd", "call_fd"):
            fd = getattr(self, name, -1)
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
                setattr(self, name, -1)
