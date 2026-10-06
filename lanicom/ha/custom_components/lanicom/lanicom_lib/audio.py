"""AUDIO payload (PROTOCOL.md section 6)."""

from __future__ import annotations

import struct

_AUDIO = struct.Struct(">II")
TS_RATE = 48_000  # ts ticks per second


def encode_audio(stream_id: int, ts: int, opus: bytes) -> bytes:
    return _AUDIO.pack(stream_id, ts & 0xFFFFFFFF) + opus


def decode_audio(data: bytes) -> tuple[int, int, bytes]:
    if len(data) < _AUDIO.size + 1:
        raise ValueError("short audio payload")
    stream_id, ts = _AUDIO.unpack_from(data)
    return stream_id, ts, data[_AUDIO.size :]
