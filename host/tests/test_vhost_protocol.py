# SPDX-License-Identifier: Apache-2.0
"""Unit tests for the vhost-user framing, codecs and feature negotiation.

Covers VFS-01 (only the features we implement are ever acked), VFS-06 (the
socket handshake a virtiofs device is configured through) and ROB-06 (a
malformed payload raises instead of being trusted). The peer is the Python
fake backend of ``host/vhost/fake_backend.py``, so ``virtiofsd`` is not needed.
"""

from __future__ import annotations

import socket

import pytest

from vhost import protocol as vp
from vhost.device import SUPPORTED_FEATURES, SUPPORTED_PROTOCOL_FEATURES, VhostUserDevice
from vhost.fake_backend import FakeVhostBackend
from vhost.memory import SharedMemory
from vhost.testing import temp_socket
from vhost.transport import Transport

TIMEOUT = 5.0


@pytest.fixture()
def peer():
    """A fake backend and a connected front end, before any negotiation."""
    with temp_socket() as path:
        backend = FakeVhostBackend(str(path)).start()
        try:
            device = VhostUserDevice(Transport.connect(path, timeout=TIMEOUT), timeout=TIMEOUT)
            yield backend, device
        finally:
            backend.stop()


def test_header_round_trip():
    raw = vp.encode_header(vp.Request.SET_MEM_TABLE, vp.FLAG_VERSION | vp.FLAG_NEED_REPLY, 40)
    assert len(raw) == vp.HEADER_SIZE == 12
    assert vp.decode_header(raw) == (vp.Request.SET_MEM_TABLE, 0x9, 40)


def test_header_rejects_an_implausible_size():
    with pytest.raises(vp.ProtocolError):
        vp.encode_header(vp.Request.SET_CONFIG, 1, vp.MAX_MSG_SIZE + 1)
    with pytest.raises(vp.ProtocolError):
        vp.decode_header(b"\x01\x00\x00\x00")


def test_u64_payload_is_eight_bytes():
    assert vp.decode_u64(vp.encode_u64(0x1234)) == 0x1234
    with pytest.raises(vp.ProtocolError):
        vp.decode_u64(b"\x01\x02\x03")


def test_memory_table_layout_is_count_padding_then_regions():
    region = vp.MemoryRegion(guest_phys_addr=0, memory_size=4096, user_addr=0x7000, mmap_offset=0)
    payload = vp.encode_memory_table([region])
    assert len(payload) == 8 + 32
    assert payload[:8] == b"\x01\x00\x00\x00" + b"\x00" * 4
    assert vp.decode_memory_table(payload) == [region]


def test_memory_table_rejects_zero_padding_or_a_short_payload():
    region = vp.MemoryRegion(0, 4096, 0x7000, 0)
    good = vp.encode_memory_table([region])
    with pytest.raises(vp.ProtocolError):
        vp.decode_memory_table(good[:20])
    with pytest.raises(vp.ProtocolError):
        vp.decode_memory_table(b"\x01\x00\x00\x00\x09\x00\x00\x00" + good[8:])
    with pytest.raises(vp.ProtocolError):
        vp.encode_memory_table([])


def test_vring_payloads_carry_index_first():
    assert vp.decode_vring_state(vp.encode_vring_state(1, 16)) == (1, 16)
    addr = vp.decode_vring_addr(vp.encode_vring_addr(1, 0, 0x1000, 0x2000, 0x3000))
    assert addr["index"] == 1 and addr["desc"] == 0x1000
    assert addr["used"] == 0x2000 and addr["avail"] == 0x3000 and addr["log"] == 0


def test_negotiation_acks_only_the_bits_both_sides_implement(peer):
    backend, device = peer
    summary = device.negotiate()
    assert device.acked_features == device.device_features & SUPPORTED_FEATURES
    assert device.acked_features & vp.VIRTIO_F_VERSION_1
    assert device.acked_protocol_features == SUPPORTED_PROTOCOL_FEATURES & backend.protocol_features
    assert device.acked_protocol_features & vp.PROTO_F_REPLY_ACK
    assert summary["acked_protocol_features"] == "REPLY_ACK"
    assert backend.owner_set is True


def test_negotiation_follows_the_protocol_order(peer):
    backend, device = peer
    device.negotiate()
    names = [name for name, _payload in backend.log]
    assert names == [
        "GET_FEATURES",
        "SET_FEATURES",
        "GET_PROTOCOL_FEATURES",
        "SET_PROTOCOL_FEATURES",
        "SET_OWNER",
    ]


def test_reply_ack_arrives_for_every_request_after_negotiation(peer):
    backend, device = peer
    device.negotiate()
    with SharedMemory(64 * 1024) as mem:
        device.set_mem_table(mem)
    names = [name for name, _payload in backend.log]
    assert names[-1] == "SET_MEM_TABLE"
    assert backend.errors == []


def test_asking_for_an_unsupported_feature_is_refused_locally(peer):
    _backend, device = peer
    device.get_features()
    with pytest.raises(vp.ProtocolError):
        device.set_features(1 << 63)


def test_the_backend_refuses_a_feature_it_does_not_offer(peer):
    backend, device = peer
    device.negotiate()
    backend.features = 0  # the device now offers nothing at all
    device.transport.send(
        vp.Request.SET_FEATURES, vp.encode_u64(vp.VIRTIO_F_VERSION_1), need_reply=True
    )
    msg = device.transport.recv(TIMEOUT)
    assert msg.is_reply_for(vp.Request.SET_FEATURES)
    assert msg.u64() != 0, "a refusal must be acknowledged with a non-zero value"
    assert backend.errors


def test_a_reply_for_another_request_is_detected():
    """Reply correlation is by request code, not by arrival order."""
    ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        device = VhostUserDevice(Transport(ours), timeout=TIMEOUT)
        Transport(theirs).send(vp.Request.GET_FEATURES, vp.encode_u64(0), reply=True)
        with pytest.raises(vp.ProtocolError):
            device._recv_reply(vp.Request.GET_PROTOCOL_FEATURES)
    finally:
        ours.close()
        theirs.close()


def test_a_request_without_a_reply_times_out():
    """A silent backend is a timeout, never a hang (CI budget)."""
    ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        device = VhostUserDevice(Transport(ours), timeout=0.2)
        with pytest.raises(TimeoutError):
            device.request(vp.Request.SET_OWNER)
    finally:
        ours.close()
        theirs.close()
