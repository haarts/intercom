"""Python <-> C interop through lanicom-core/build/lanicom_tool (built by `cmake -S lanicom-core -B lanicom-core/build`)."""

import os
import random
import subprocess
from pathlib import Path

import pytest

from lanicom import control
from lanicom.keys import NetworkKey
from lanicom.packet import Header, open_packet, seal

from test_control import random_msg

TOOL = Path(os.environ.get("LANICOM_TOOL", Path(__file__).resolve().parents[2] / "lanicom-core" / "build" / "lanicom_tool"))
pytestmark = pytest.mark.skipif(not TOOL.exists(), reason=f"{TOOL} not built")

KEY_STRING = "interop-key-%d" % 7
KEY = NetworkKey(KEY_STRING)


def tool(*args):
    return subprocess.run([str(TOOL), *map(str, args)], check=True, capture_output=True, text=True).stdout.strip()


def test_python_seals_c_opens():
    rng = random.Random(3)
    for _ in range(20):
        h = Header(rng.choice([1, 2]), KEY.key_id, rng.getrandbits(32) or 1, rng.getrandbits(64), rng.getrandbits(32))
        pt = rng.randbytes(rng.randrange(0, 300))
        out = tool("open", KEY_STRING, seal(KEY, h, pt).hex()).split()
        assert out == [str(h.type), f"{h.sender_id:08x}", f"{h.epoch:016x}", str(h.seq)] + ([pt.hex()] if pt else [])


def test_c_seals_python_opens():
    rng = random.Random(4)
    for _ in range(20):
        type_, sid, epoch, seq = rng.choice([1, 2]), rng.getrandbits(32) or 1, rng.getrandbits(64), rng.getrandbits(32)
        pt = rng.randbytes(rng.randrange(0, 300))
        packet = bytes.fromhex(tool("seal", KEY_STRING, type_, f"{sid:x}", f"{epoch:x}", seq, pt.hex()))
        header, plain = open_packet(KEY, packet)
        assert (header.type, header.sender_id, header.epoch, header.seq, plain) == (type_, sid, epoch, seq, pt)


def test_c_rejects_tampering():
    data = bytearray(seal(KEY, Header(2, KEY.key_id, 5, 6, 7), b"hello"))
    data[25] ^= 0x10
    assert tool("open", KEY_STRING, data.hex()) == "error auth"
    assert tool("open", "another key", bytes(data).hex()) == "error foreign"


def test_control_round_trip_through_c():
    rng = random.Random(5)
    checked = 0
    for _ in range(200):
        msg = random_msg(rng)
        # The C codec truncates to protocol limits; only compare messages within them.
        if isinstance(msg, control.Hello) and len(msg.name.encode()) > 32:
            continue
        data = control.encode(msg)
        assert tool("control", data.hex()) == data.hex()
        checked += 1
    assert checked > 100
