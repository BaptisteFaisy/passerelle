# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the split virtqueue: layout, shared memory, kick and call.

Covers VFS-02 (a request queue armed with its own kick/call eventfds), VFS-06
(the memory table a virtiofs backend is configured with) and ROB-06 (an
oversized queue, a bad size or an out-of-range descriptor is refused).

The peer is the Python fake backend, which maps the memfd wherever it likes
and translates addresses through the memory table - the same thing the ``vhost``
crate does - so these tests also prove the address convention is the portable
one and not an artefact of our own two modules agreeing by accident.
"""

from __future__ import annotations

import struct

import pytest

from vhost.memory import SharedMemory
from vhost.protocol import ProtocolError
from vhost.testing import FakeSession, temp_socket_path
from vhost.vring import DESC_F_NEXT, DESC_F_WRITE, DESC_SIZE, SplitVirtqueue

TIMEOUT = 5.0


@pytest.fixture()
def fake():
    """A negotiated session with one request queue armed."""
    with FakeSession(temp_socket_path()) as session:
        yield session


def test_the_rings_are_aligned_and_in_order():
    with SharedMemory(64 * 1024) as mem:
        vq = SplitVirtqueue(mem, index=1, num=8, tx_size=512, rx_size=512)
        try:
            assert vq.desc_off % 16 == 0
            assert vq.avail_off % 2 == 0
            assert vq.used_off % 4 == 0
            assert vq.desc_off < vq.avail_off < vq.used_off < vq.buffers_off
            assert vq.buffers_off % 4096 == 0
        finally:
            vq.close()


def test_the_ring_addresses_are_inside_the_shared_region():
    with SharedMemory(64 * 1024) as mem:
        vq = SplitVirtqueue(mem, index=1, num=8, tx_size=512, rx_size=512)
        try:
            for addr in (vq.desc_addr, vq.avail_addr, vq.used_addr):
                assert 0 <= mem.offset_of(addr) < mem.size
        finally:
            vq.close()


def test_a_queue_size_must_be_a_power_of_two():
    with SharedMemory(64 * 1024) as mem:
        with pytest.raises(ProtocolError):
            SplitVirtqueue(mem, num=6)


def test_a_queue_that_does_not_fit_is_refused():
    with SharedMemory(8 * 1024) as mem:
        with pytest.raises(ProtocolError):
            SplitVirtqueue(mem, num=16, tx_size=4096, rx_size=65536)


def test_event_idx_reserves_the_two_event_fields():
    with SharedMemory(64 * 1024) as mem:
        plain = SplitVirtqueue(mem, num=8, tx_size=256, rx_size=256)
        indexed = SplitVirtqueue(mem, num=8, tx_size=256, rx_size=256, event_idx=True)
        try:
            assert indexed.used_off > plain.used_off
            assert indexed.avail_event_off > indexed.used_off
            indexed.set_used_event(3)
            assert indexed.mem.buffer[indexed.used_event_off] == 3
        finally:
            plain.close()
            indexed.close()


def test_post_publishes_one_chain_on_the_available_ring():
    with SharedMemory(64 * 1024) as mem:
        vq = SplitVirtqueue(mem, index=1, num=8, tx_size=512, rx_size=512)
        try:
            head = vq.post(0, 40, 64)
            assert head == 0
            addr, length, flags, nxt = struct.unpack_from("<QIHH", vq._buf, vq.desc_off)
            assert (length, flags, nxt) == (40, DESC_F_NEXT, 1)
            assert addr == mem.gpa_of(vq.tx_offset(0)), "descriptors carry guest addresses"
            addr2, length2, flags2, _ = struct.unpack_from("<QIHH", vq._buf, vq.desc_off + DESC_SIZE)
            assert (length2, flags2) == (64, DESC_F_WRITE)
            assert addr2 == mem.gpa_of(vq.rx_offset(0))
            assert vq.avail_idx == 1
            assert vq.used_idx() == 0
        finally:
            vq.close()


def test_the_memfd_really_is_shared_with_the_backend(fake):
    """Write here, read there: the SCM_RIGHTS descriptor is the same memory."""
    marker = b"passerelle"
    fake.mem.buffer[8192:8192 + len(marker)] = marker
    _gpa, _user_addr, mapping = fake.backend.regions[0]
    assert bytes(memoryview(mapping)[8192:8192 + len(marker)]) == marker
    # ... and the other way round, which is how the used ring reaches us.
    memoryview(mapping)[9000:9004] = b"back"
    assert bytes(fake.mem.buffer[9000:9004]) == b"back"


def test_the_memory_table_reports_our_mapping_address(fake):
    gpa, user_addr, _mapping = fake.backend.regions[0]
    assert gpa == 0
    assert user_addr == fake.mem.address
    assert len(fake.backend.regions) == 1


def test_kick_and_call_move_one_buffer(fake):
    """A posted request comes back through the used ring, signalled by call."""
    vq = fake.vq
    assert vq.used_idx() == 0
    out = fake.fuse.init()
    assert out["major"] == 7
    assert vq.used_idx() == 1, "the backend never published a used entry"
    assert fake.fuse.unexpected == []
    assert fake.backend.errors == []


def test_the_used_ring_is_drained_by_harvest(fake):
    """One request in, one used entry out - and nothing left behind."""
    fake.fuse.lookup(1, "anything")
    assert fake.vq.used_idx() == 1
    assert fake.vq.harvest() == []
    assert fake.fuse.unexpected == []


def test_a_slot_out_of_range_is_refused(fake):
    with pytest.raises(ProtocolError):
        fake.vq.post(fake.vq.slots, 10, 10)
    with pytest.raises(ProtocolError):
        fake.vq.post(0, fake.vq.tx_size + 1, 10)
