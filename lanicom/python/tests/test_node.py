"""End-to-end tests with real UDP sockets on 127.0.0.x (one address per node, same port)."""

import asyncio
import socket
import time

import pytest

from lanicom import CAP_CAPTURE, CAP_PLAYBACK, NetworkKey, Node, NodeConfig, Target
from lanicom.control import Hello
from lanicom.packet import TYPE_CONTROL
from lanicom import control

KEY = NetworkKey("test-key-one", iterations=1000)
OTHER = NetworkKey("test-key-two", iterations=1000)
PORT = 47199
HOSTS = ["127.0.0.1", "127.0.0.2", "127.0.0.3", "127.0.0.4"]


def can_bind_loopback_aliases():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.2", 0))
        s.close()
        return True
    except OSError:
        return False


pytestmark = pytest.mark.skipif(not can_bind_loopback_aliases(), reason="needs 127.0.0.0/8 loopback aliases (Linux)")


def make(i, key=KEY, caps=CAP_PLAYBACK | CAP_CAPTURE, peers=None):
    others = [h for h in HOSTS if h != HOSTS[i]] if peers is None else peers
    cfg = NodeConfig(name=f"n{i}", caps=caps, port=PORT, bind=HOSTS[i], broadcast=None, static_peers=others)
    return Node(key, cfg)


async def wait_for(cond, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return
        await asyncio.sleep(0.01)
    raise AssertionError("timed out")


async def started(*nodes):
    for n in nodes:
        await n.start()
    return nodes


def verified(a, b):
    p = a.peers.get(b.sender_id)
    return p is not None and p.verified and p.announced


async def test_discovery_and_verification():
    a, b, c = await started(make(0), make(1), make(2))
    try:
        await wait_for(lambda: all(verified(x, y) for x in (a, b, c) for y in (a, b, c) if x is not y))
        assert a.peers.get(b.sender_id).name == "n1"
        assert a.peers.get(b.sender_id).rtt_ms is not None
    finally:
        for n in (a, b, c):
            await n.stop()


async def test_audio_targets_and_events():
    a, b, c = await started(make(0), make(1), make(2))
    got = {b.sender_id: [], c.sender_id: []}
    events = []
    for n in (b, c):
        n.on_audio = lambda peer, sid, ts, pkt, n=n: got[n.sender_id].append((sid, ts, pkt))
        n.on_talk_start = lambda peer, m, n=n: events.append((n.config.name, "start", str(m.target)))
        n.on_talk_stop = lambda peer, sid, n=n: events.append((n.config.name, "stop"))
    try:
        await wait_for(lambda: verified(a, b) and verified(a, c))
        talk = a.start_talk(Target(device=b.sender_id))
        for i in range(5):
            assert talk.send_frame(b"\x78\x01\x02", 160) == 1
        talk.stop()
        await wait_for(lambda: len(got[b.sender_id]) == 5 and ("n1", "stop") in events)
        assert got[c.sender_id] == []
        ts = [t for _, t, _ in got[b.sender_id]]
        assert [x - ts[0] for x in ts] == [0, 480, 960, 1440, 1920]
        assert events.count(("n1", "start", f"device:{b.sender_id:08x}")) == 1  # 3 copies, deduped
        assert events.count(("n1", "stop")) == 1

        talk = a.start_talk(Target.everyone())
        assert talk.send_frame(b"\x78\x01", 160) == 2
        talk = a.start_talk(Target(device=c.sender_id))
        assert talk.send_frame(b"\x78\x01", 160) == 1
        await wait_for(lambda: len(got[c.sender_id]) == 2)
    finally:
        for n in (a, b, c):
            await n.stop()


async def test_no_audio_to_peers_without_playback():
    a, b = await started(make(0), make(1, caps=CAP_CAPTURE))
    try:
        await wait_for(lambda: verified(a, b))
        assert a.start_talk().send_frame(b"\x78\x01", 160) == 0
    finally:
        await a.stop()
        await b.stop()


async def test_wrong_key_is_invisible():
    a, b, x = await started(make(0), make(1), make(2, key=OTHER))
    try:
        await wait_for(lambda: verified(a, b) and verified(b, a))
        await asyncio.sleep(0.3)
        assert x.sender_id not in a.peers.peers and not x.peers.peers
        assert a.stats["foreign"] > 0 and x.stats["foreign"] > 0
    finally:
        for n in (a, b, x):
            await n.stop()


async def test_bye_removes_peer_immediately():
    a, b = await started(make(0), make(1))
    removed = []
    a.on_peer_removed = removed.append
    await wait_for(lambda: verified(a, b))
    await b.stop()
    await wait_for(lambda: removed)
    assert removed[0].sender_id == b.sender_id
    await a.stop()


async def test_replayed_session_rejected_after_receiver_restart():
    """Record a's packets to b, restart b (losing all state), replay: nothing gets through."""
    a, b = await started(make(0), make(1))
    recorded = []
    real = a._sendto
    a._sendto = lambda data, addr: (recorded.append(data), real(data, addr))
    await wait_for(lambda: verified(a, b))
    talk = a.start_talk()
    for _ in range(5):
        talk.send_frame(b"\x78\x01", 160)
    talk.stop()
    await a.stop()
    await b.stop()

    b2 = make(1, peers=[])
    await b2.start()
    audio = []
    b2.on_audio = lambda *args: audio.append(args)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((HOSTS[0], PORT))  # the attacker replays from a's address
    try:
        for data in recorded:
            sock.sendto(data, (HOSTS[1], PORT))
        await asyncio.sleep(0.3)
        assert audio == []
        assert b2.stats["unverified"] > 0
        assert not any(p.verified for p in b2.peers)
        # The challenge went to the attacker, who can't answer it without the key.
        sock.settimeout(0.5)
        challenge, _ = sock.recvfrom(1500)
        assert challenge[1] == TYPE_CONTROL
    finally:
        sock.close()
        await b2.stop()


async def test_peer_reboot_is_reverified():
    a, b = await started(make(0), make(1))
    await wait_for(lambda: verified(a, b))
    old_epoch = a.peers.get(b.sender_id).verified_epoch
    sid = b.sender_id
    for task in b._tasks:  # crash: no bye
        task.cancel()
    b.transport.close()
    b2 = make(1)
    b2.sender_id = sid
    await b2.start()
    try:
        await wait_for(lambda: a.peers.get(sid) is not None and a.peers.get(sid).verified_epoch == b2.epoch)
        assert b2.epoch != old_epoch
    finally:
        await a.stop()
        await b2.stop()


async def test_monitor_gets_talk_metadata_but_no_audio():
    from lanicom import CAP_MONITOR

    a, b, m = await started(make(0), make(1), make(2, caps=CAP_CAPTURE | CAP_MONITOR))
    events, audio = [], []
    m.on_talk_start = lambda peer, msg: events.append(("start", str(msg.target)))
    m.on_talk_stop = lambda peer, sid: events.append(("stop",))
    m.on_audio = lambda *args: audio.append(args)
    try:
        await wait_for(lambda: verified(a, b) and verified(a, m))
        talk = a.start_talk(Target(device=b.sender_id))
        assert talk.send_frame(b"\x78\x01", 160) == 1  # audio only to b
        talk.stop()
        await wait_for(lambda: ("stop",) in events)
        assert events == [("start", f"device:{b.sender_id:08x}"), ("stop",)]
        assert audio == []
    finally:
        for n in (a, b, m):
            await n.stop()
