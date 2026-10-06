"""Generate spec/vectors/lanicom-v1.json.

Uses primitives independent of the lanicom package wherever possible
(`cryptography`'s PBKDF2/HKDF, the official protobuf runtime, a set-based
replay model), so the vectors check the implementations instead of echoing them.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF, HKDFExpand
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC

import pbref

OUT = Path(__file__).resolve().parents[2] / "spec" / "vectors" / "lanicom-v1.json"
SALT = b"lanicom-v1"


def derive(key_string: str):
    master = PBKDF2HMAC(hashes.SHA256(), 32, SALT, 100_000).derive(key_string.strip().encode())
    key_id = struct.unpack(">H", HKDF(hashes.SHA256(), 2, SALT, b"key-id").derive(master))[0]
    # HKDF-Extract by hand only to reuse the PRK for several expands.
    import hmac, hashlib

    prk = hmac.new(SALT, master, hashlib.sha256).digest()
    def send_key(sid):
        return HKDFExpand(hashes.SHA256(), 32, b"packet" + struct.pack(">I", sid)).derive(prk)
    return master, key_id, send_key


def seal(key_string, type_, sender_id, epoch, seq, plaintext, key_id=None, version=1):
    _, kid, send_key = derive(key_string)
    header = struct.pack(">BBHIQI", version, type_, kid if key_id is None else key_id, sender_id, epoch, seq)
    return header + ChaCha20Poly1305(send_key(sender_id)).encrypt(header[8:20], plaintext, header)


def control_vectors():
    pb = pbref.load()
    cases = []

    def add(name, msg, fields):
        cases.append({"name": name, "message": fields, "bytes": msg.SerializeToString().hex()})

    c = pb.Control(hello=pb.Hello(name="Keuken", caps=3))
    add("hello", c, {"hello": {"name": "Keuken", "caps": 3}})
    c = pb.Control(hello=pb.Hello(name="Zolder ☀", caps=1, challenge=0x0123456789ABCDEF))
    add("hello_challenge_utf8", c, {"hello": {"name": "Zolder ☀", "caps": 1, "challenge": 0x0123456789ABCDEF}})
    c = pb.Control(hello=pb.Hello(echo=0xFEDCBA9876543210, bye=True))
    add("hello_echo_bye", c, {"hello": {"echo": 0xFEDCBA9876543210, "bye": True}})
    c = pb.Control(hello=pb.Hello())
    add("hello_empty", c, {"hello": {}})
    c = pb.Control(talk_start=pb.TalkStart(target=pb.Target(all=True), stream_id=0xDEADBEEF))
    add("talk_start_all", c, {"talk_start": {"target": {"all": True}, "stream_id": 0xDEADBEEF}})
    c = pb.Control(talk_start=pb.TalkStart(target=pb.Target(device=0x8A3F01C2), stream_id=300))
    add("talk_start_device", c, {"talk_start": {"target": {"device": 0x8A3F01C2}, "stream_id": 300}})
    c = pb.Control(talk_stop=pb.TalkStop(stream_id=0xDEADBEEF))
    add("talk_stop", c, {"talk_stop": {"stream_id": 0xDEADBEEF}})
    # Unknown fields (a future version) must be skipped.
    unknown = (
        bytes.fromhex("0a")  # field 1 (hello), LEN
    )
    inner = (
        b"\x0a\x01A"  # name = "A"
        + b"\x38\x96\x01"  # field 7 varint 150 (unknown)
        + b"\x45\x01\x02\x03\x04"  # field 8 fixed32 (unknown)
        + b"\x49" + bytes(8)  # field 9 fixed64 (unknown)
        + b"\x52\x02hi"  # field 10 LEN (unknown)
        + b"\x18\x02"  # caps = 2
    )
    raw = unknown + bytes([len(inner)]) + inner + b"\x22\x00"  # + unknown top-level field 4 (empty LEN)
    parsed = pb.Control.FromString(raw)
    assert parsed.hello.name == "A" and parsed.hello.caps == 2
    cases.append({"name": "unknown_fields", "message": {"hello": {"name": "A", "caps": 2}}, "bytes": raw.hex(), "decode_only": True})
    return cases


def replay_vectors():
    """A set-based model of the 64-wide window."""
    def run(first, seqs):
        seen, highest, out = {first}, first, []
        for s in seqs:
            ok = s not in seen and s > highest - 64
            if ok:
                seen.add(s)
                highest = max(highest, s)
            out.append([s, ok])
        return {"first": first, "steps": out}

    return [
        run(0, [1, 2, 2, 0, 5, 3, 4, 4]),
        run(100, [99, 37, 36, 164, 100, 101, 163, 165, 101]),
        run(10, [500, 437, 436, 438, 438, 10, 1000]),
        run(0xFFFFFF00, [0xFFFFFF01, 0xFFFFFFFF, 0xFFFFFFC0, 0xFFFFFFC1, 0xFFFFFFFE]),
    ]


def main():
    k1 = "kupzf-wdgg2-jyx4q-sxng5-7whzy"
    k2 = "  correct horse battery staple \n"
    keys = []
    for ks in (k1, k2, "lanicom"):
        master, key_id, send_key = derive(ks)
        keys.append({
            "key_string": ks,
            "master": master.hex(),
            "key_id": key_id,
            "send_keys": {f"{sid:08x}": send_key(sid).hex() for sid in (1, 0x8A3F01C2, 0xFFFFFFFF)},
        })

    packets = []
    hello = bytes.fromhex(control_vectors()[0]["bytes"])
    audio = struct.pack(">II", 0xDEADBEEF, 480 * 7) + bytes.fromhex("78 01 02 03 04 05".replace(" ", ""))
    for ks, type_, sid, epoch, seq, pt in (
        (k1, 1, 0x8A3F01C2, 0x0000000500ABCDEF, 0, hello),
        (k1, 2, 0x8A3F01C2, 0x0000000500ABCDEF, 1, audio),
        (k1, 2, 0x00000001, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF, audio),
        (k2, 1, 0xFFFFFFFF, 1, 42, b""),
    ):
        packets.append({
            "key_string": ks, "type": type_, "sender_id": sid, "epoch": epoch, "seq": seq,
            "plaintext": pt.hex(), "packet": seal(ks, type_, sid, epoch, seq, pt).hex(),
        })

    good = bytes.fromhex(packets[0]["packet"])
    flip = lambda b, i: b[:i] + bytes([b[i] ^ 1]) + b[i + 1 :]
    invalid = [
        {"name": "short", "packet": good[:35].hex(), "reason": "malformed"},
        {"name": "too_long", "packet": (good + bytes(1200)).hex(), "reason": "malformed"},
        {"name": "bad_version", "packet": seal(k1, 1, 5, 1, 0, b"x", version=2).hex(), "reason": "malformed"},
        {"name": "bad_type", "packet": flip(good, 1).hex(), "reason": "malformed"},
        {"name": "foreign_key_id", "packet": seal(k2, 1, 5, 1, 0, b"x").hex(), "reason": "foreign"},
        {"name": "flipped_tag", "packet": flip(good, len(good) - 1).hex(), "reason": "auth"},
        {"name": "flipped_ciphertext", "packet": flip(good, 21).hex(), "reason": "auth"},
        {"name": "flipped_seq", "packet": flip(good, 19).hex(), "reason": "auth"},
        {"name": "flipped_sender", "packet": flip(good, 7).hex(), "reason": "auth"},
    ]
    for case in invalid:
        case["key_string"] = k1

    vectors = {
        "description": "lanicom v1 test vectors; see PROTOCOL.md. Generated by python/tools/gen_vectors.py.",
        "keys": keys,
        "packets": packets,
        "invalid_packets": invalid,
        "control": control_vectors(),
        "replay": replay_vectors(),
    }
    OUT.write_text(json.dumps(vectors, indent=1, ensure_ascii=False) + "\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
