"""Key derivation (PROTOCOL.md section 2)."""

from __future__ import annotations

import base64
import hashlib
import hmac
import secrets
import struct

SALT = b"lanicom-v1"
PBKDF2_ITERATIONS = 100_000


def generate_key_string() -> str:
    """125 random bits as five dash-separated groups of base32, e.g. 'k7m2p-...'."""
    raw = base64.b32encode(secrets.token_bytes(16)).decode().lower()[:25]
    return "-".join(raw[i : i + 5] for i in range(0, 25, 5))


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    out, block, counter = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


class NetworkKey:
    """Everything derived from one key string; per-sender keys are cached."""

    def __init__(self, key_string: str, iterations: int = PBKDF2_ITERATIONS):
        self.master = hashlib.pbkdf2_hmac("sha256", key_string.strip().encode(), SALT, iterations, 32)
        self.prk = hmac.new(SALT, self.master, hashlib.sha256).digest()
        self.key_id: int = struct.unpack(">H", hkdf_expand(self.prk, b"key-id", 2))[0]
        self._send_keys: dict[int, bytes] = {}

    def send_key(self, sender_id: int) -> bytes:
        key = self._send_keys.get(sender_id)
        if key is None:
            if len(self._send_keys) > 1024:
                self._send_keys.clear()
            key = hkdf_expand(self.prk, b"packet" + struct.pack(">I", sender_id), 32)
            self._send_keys[sender_id] = key
        return key
