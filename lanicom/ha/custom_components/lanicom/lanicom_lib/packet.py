"""Packet header and AEAD sealing (PROTOCOL.md section 3)."""

from __future__ import annotations

import struct
from dataclasses import dataclass

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

from .keys import NetworkKey

VERSION = 1
TYPE_CONTROL = 1
TYPE_AUDIO = 2
HEADER_LEN = 20
TAG_LEN = 16
MIN_PACKET = HEADER_LEN + TAG_LEN
MAX_PACKET = 1200

_HEADER = struct.Struct(">BBHIQI")


class PacketError(Exception):
    """Raised for packets that must be dropped; `reason` is the counter name."""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


@dataclass(frozen=True)
class Header:
    type: int
    key_id: int
    sender_id: int
    epoch: int
    seq: int
    version: int = VERSION

    def pack(self) -> bytes:
        return _HEADER.pack(self.version, self.type, self.key_id, self.sender_id, self.epoch, self.seq)

    @classmethod
    def parse(cls, data: bytes) -> "Header":
        if not MIN_PACKET <= len(data) <= MAX_PACKET:
            raise PacketError("malformed")
        version, type_, key_id, sender_id, epoch, seq = _HEADER.unpack_from(data)
        if version != VERSION or type_ not in (TYPE_CONTROL, TYPE_AUDIO):
            raise PacketError("malformed")
        return cls(type_, key_id, sender_id, epoch, seq, version)


def seal(key: NetworkKey, header: Header, plaintext: bytes) -> bytes:
    raw = header.pack()
    out = raw + ChaCha20Poly1305(key.send_key(header.sender_id)).encrypt(raw[8:20], plaintext, raw)
    if len(out) > MAX_PACKET:
        raise ValueError("packet too large")
    return out


def open_packet(key: NetworkKey, data: bytes) -> tuple[Header, bytes]:
    """Parse, check key_id and authenticate. Raises PacketError."""
    header = Header.parse(data)
    if header.key_id != key.key_id:
        raise PacketError("foreign")
    try:
        plain = ChaCha20Poly1305(key.send_key(header.sender_id)).decrypt(data[8:20], data[HEADER_LEN:], data[:HEADER_LEN])
    except InvalidTag:
        raise PacketError("auth") from None
    return header, plain
