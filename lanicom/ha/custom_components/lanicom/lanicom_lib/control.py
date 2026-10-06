"""Control messages (spec/lanicom.proto), hand-coded protobuf wire format.

Only what lanicom.proto needs: varint, fixed32, fixed64 and length-delimited
fields. Encoding follows proto3 rules (defaults are omitted, fields are in
field-number order), so the bytes match the official protobuf library's.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Union

CAP_PLAYBACK = 1
CAP_CAPTURE = 2
CAP_MONITOR = 4

_VARINT, _I64, _LEN, _I32 = 0, 1, 2, 5


class ControlError(ValueError):
    pass


@dataclass(frozen=True)
class Target:
    device: int | None = None
    all: bool = False

    @classmethod
    def everyone(cls) -> "Target":
        return cls(all=True)

    def __str__(self) -> str:
        if self.device is not None:
            return f"device:{self.device:08x}"
        return "all"


@dataclass
class Hello:
    name: str = ""
    caps: int = 0
    challenge: int = 0
    echo: int = 0
    bye: bool = False


@dataclass
class TalkStart:
    target: Target = field(default_factory=Target.everyone)
    stream_id: int = 0


@dataclass
class TalkStop:
    stream_id: int = 0


Control = Union[Hello, TalkStart, TalkStop]


# --- writer ---------------------------------------------------------------


def _varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _key(num: int, wire: int) -> bytes:
    return _varint(num << 3 | wire)


def _len_field(num: int, payload: bytes) -> bytes:
    return _key(num, _LEN) + _varint(len(payload)) + payload


def _encode_target(t: Target) -> bytes:
    if t.device is not None:
        return _key(1, _I32) + struct.pack("<I", t.device)
    return _key(3, _VARINT) + _varint(1 if t.all else 0)


def _encode_hello(h: Hello) -> bytes:
    out = b""
    if h.name:
        out += _len_field(1, h.name.encode())
    if h.caps:
        out += _key(3, _VARINT) + _varint(h.caps)
    if h.challenge:
        out += _key(4, _I64) + struct.pack("<Q", h.challenge)
    if h.echo:
        out += _key(5, _I64) + struct.pack("<Q", h.echo)
    if h.bye:
        out += _key(6, _VARINT) + b"\x01"
    return out


def encode(msg: Control) -> bytes:
    if isinstance(msg, Hello):
        return _len_field(1, _encode_hello(msg))
    if isinstance(msg, TalkStart):
        body = _len_field(1, _encode_target(msg.target))
        if msg.stream_id:
            body += _key(2, _VARINT) + _varint(msg.stream_id)
        return _len_field(2, body)
    if isinstance(msg, TalkStop):
        body = _key(1, _VARINT) + _varint(msg.stream_id) if msg.stream_id else b""
        return _len_field(3, body)
    raise TypeError(type(msg))


# --- reader ---------------------------------------------------------------


def _fields(data: bytes):
    """Yield (field number, wire type, value); value is int or bytes."""
    pos, end = 0, len(data)

    def varint() -> int:
        nonlocal pos
        result = shift = 0
        while True:
            if pos >= end or shift > 63:
                raise ControlError("bad varint")
            b = data[pos]
            pos += 1
            result |= (b & 0x7F) << shift
            if not b & 0x80:
                return result & 0xFFFFFFFFFFFFFFFF
            shift += 7

    while pos < end:
        key = varint()
        num, wire = key >> 3, key & 7
        if num == 0:
            raise ControlError("field 0")
        if wire == _VARINT:
            yield num, wire, varint()
        elif wire == _I64:
            if pos + 8 > end:
                raise ControlError("truncated")
            yield num, wire, struct.unpack_from("<Q", data, pos)[0]
            pos += 8
        elif wire == _I32:
            if pos + 4 > end:
                raise ControlError("truncated")
            yield num, wire, struct.unpack_from("<I", data, pos)[0]
            pos += 4
        elif wire == _LEN:
            n = varint()
            if pos + n > end:
                raise ControlError("truncated")
            yield num, wire, data[pos : pos + n]
            pos += n
        else:
            raise ControlError(f"wire type {wire}")


def _text(value) -> str:
    if not isinstance(value, bytes):
        raise ControlError("expected string")
    try:
        return value.decode()
    except UnicodeDecodeError:
        raise ControlError("bad utf-8") from None


def _expect(wire: int, want: int) -> None:
    if wire != want:
        raise ControlError("wrong wire type")


def _decode_target(data: bytes) -> Target:
    t = Target(all=False)
    for num, wire, value in _fields(data):
        if num == 1:
            _expect(wire, _I32)
            t = Target(device=value)
        elif num == 3:
            _expect(wire, _VARINT)
            t = Target(all=bool(value))
    return t


def _decode_hello(data: bytes) -> Hello:
    h = Hello()
    for num, wire, value in _fields(data):
        if num == 1:
            _expect(wire, _LEN)
            h.name = _text(value)
        elif num == 3:
            _expect(wire, _VARINT)
            h.caps = value & 0xFFFFFFFF
        elif num == 4:
            _expect(wire, _I64)
            h.challenge = value
        elif num == 5:
            _expect(wire, _I64)
            h.echo = value
        elif num == 6:
            _expect(wire, _VARINT)
            h.bye = bool(value)
    return h


def decode(data: bytes) -> Control | None:
    """Decode a Control message; None if it holds no message we know."""
    msg: Control | None = None
    for num, wire, value in _fields(data):
        if num in (1, 2, 3):
            _expect(wire, _LEN)
        if num == 1:
            msg = _decode_hello(value)
        elif num == 2:
            ts = TalkStart(target=Target(all=False))
            for n2, w2, v2 in _fields(value):
                if n2 == 1:
                    _expect(w2, _LEN)
                    ts.target = _decode_target(v2)
                elif n2 == 2:
                    _expect(w2, _VARINT)
                    ts.stream_id = v2 & 0xFFFFFFFF
            msg = ts
        elif num == 3:
            stop = TalkStop()
            for n2, w2, v2 in _fields(value):
                if n2 == 1:
                    _expect(w2, _VARINT)
                    stop.stream_id = v2 & 0xFFFFFFFF
            msg = stop
    return msg
