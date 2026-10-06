import math
import struct

from lanicom import opus
from lanicom.jitter import JitterBuffer, Mixer, ts_diff


def frames(n, freq=440.0, start_ts=1000):
    enc = opus.Encoder()
    out = []
    for i in range(n):
        pcm = struct.pack("<160h", *(int(6000 * math.sin(2 * math.pi * freq * (i * 160 + k) / 16000)) for k in range(160)))
        out.append((start_ts + i * 480, enc.encode(pcm)))
    return out


def drain(jb, n):
    return [jb.pop() for _ in range(n)]


def test_ts_diff_wraps():
    assert ts_diff(5, 0xFFFFFFFB) == 10
    assert ts_diff(0xFFFFFFFB, 5) == -10


def test_buffers_until_target_then_plays_in_order():
    jb = JitterBuffer(target_ms=20)
    f = frames(5)
    jb.push(*f[1])
    assert jb.pop() is None
    jb.push(*f[0])  # reordered
    out = drain(jb, 2)
    assert [o[0] for o in out] == ["frame", "frame"]
    assert out[0][1] == f[0][1] and out[1][1] == f[1][1]


def test_loss_uses_fec_when_next_frame_is_buffered():
    jb = JitterBuffer(target_ms=20)
    f = frames(4)
    for i in (0, 2, 3):
        jb.push(*f[i])
    out = drain(jb, 4)
    assert [o[0] for o in out] == ["frame", "fec", "frame", "frame"]
    assert out[1][1] == f[2][1]
    assert jb.stats.lost == 1 and jb.stats.fec == 1


def test_late_packet_dropped_and_target_grows():
    jb = JitterBuffer(target_ms=20, max_ms=60)
    f = frames(6)
    for i in (0, 1, 3):
        jb.push(*f[i])
    drain(jb, 3)  # 0, 1, then fec for 2
    jb.push(*f[2])  # arrives too late
    assert jb.stats.late == 1
    assert jb.target == 30 * 48


def test_never_holds_more_than_max():
    jb = JitterBuffer(target_ms=20, max_ms=60)
    f = frames(30)
    jb.push(*f[0])
    jb.push(*f[1])
    jb.pop()
    for item in f[2:]:
        jb.push(*item)
    assert jb.depth() <= 60 * 48
    assert jb.stats.dropped_for_latency > 0


def test_mixer_sums_two_streams_and_ends_them():
    mixer = Mixer(target_ms=20)
    a, b = frames(10, 440), frames(10, 660)
    now = 0.0
    for i in range(10):
        mixer.push(1, 100, *a[i], now)
        mixer.push(2, 200, *b[i], now)
    pcm, _ = mixer.read(160 * 10, now)
    samples = struct.unpack(f"<{160 * 10}h", pcm)
    assert max(abs(s) for s in samples[480:]) > 6000  # louder than either alone
    pcm, ended = mixer.read(160, now + 1.0)
    assert {s.stream_id for s in ended} == {100, 200}
    assert not mixer.streams
