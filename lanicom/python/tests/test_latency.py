"""Loopback latency: capture-complete -> UDP -> verify/replay -> jitter buffer -> decoded PCM out.

Simulates a 10 ms playout clock on the receiver and reports how long each frame
takes from being sent to leaving the mixer. This is the software part of the
budget; the device adds I2S DMA, Opus lookahead and the acoustic path.
"""

import asyncio
import math
import statistics
import struct
import time

from lanicom import opus
from lanicom.jitter import Mixer

from test_node import make, started, verified, wait_for


async def test_loopback_latency(capsys):
    a, b = await started(make(0), make(1))
    mixer = Mixer(target_ms=20)
    arrivals = {}
    b.on_audio = lambda peer, sid, ts, pkt: (arrivals.setdefault(ts, time.monotonic()), mixer.push(peer.sender_id, sid, ts, pkt, time.monotonic()))
    try:
        await wait_for(lambda: verified(a, b))
        enc = opus.Encoder()
        talk = a.start_talk()
        sent = {}
        n = 200
        start = time.monotonic()
        played_at = []
        loud_frames = set(range(20, n, 20))  # a click every 200 ms
        for i in range(n):
            await asyncio.sleep(max(0, start + i * 0.01 - time.monotonic()))
            amp = 20000 if i in loud_frames else 0
            pcm = struct.pack("<160h", *(int(amp * math.sin(k / 3)) for k in range(160)))
            sent[i] = time.monotonic()
            talk.send_frame(enc.encode(pcm), 160)
            pcm_out, _ = mixer.read(160, time.monotonic())
            samples = struct.unpack("<160h", pcm_out)
            if max(map(abs, samples)) > 5000:
                played_at.append(time.monotonic())
        # Match each click to the first loud output after it.
        delays = []
        for i in sorted(loud_frames):
            after = [t for t in played_at if t >= sent[i]]
            if after:
                delays.append((after[0] - sent[i]) * 1000)
        net = [(arrivals[ts] - s) * 1000 for ts, s in zip(sorted(arrivals), sent.values())]
        with capsys.disabled():
            print(f"\nloopback: network+crypto p50 {statistics.median(net):.2f} ms, "
                  f"send->playout p50 {statistics.median(delays):.1f} ms, max {max(delays):.1f} ms "
                  f"(jitter target 20 ms, Opus lookahead excluded)")
        assert len(delays) >= len(loud_frames) - 1
        assert statistics.median(delays) < 45
    finally:
        await a.stop()
        await b.stop()
