"""Click test: how long from a sound at the board's mic until its packet reaches this laptop.

    clicktest.py [BOARD=lanicom-p4-e80332.local] [CLICKS=20]

The laptop plays short clicks on its speakers (put it next to the board) and receives the
board's lanicom stream. The reference is when PortAudio says each click left the DAC; the
laptop's own mic cross-checks that where it hears a click. Every time comes from one
PortAudio duplex stream clock.

Measured: board capture (I2S DMA, 10 ms frame, Opus) + network, up to the packet's
arrival at the laptop. A receiving board adds its jitter buffer (20 ms target), decode
and its own I2S output on top of that.
"""

import asyncio
import math
import statistics
import sys
import time
from pathlib import Path

import numpy as np
import sounddevice as sd
import yaml
from aioesphomeapi import APIClient, SwitchInfo

from lanicom import CAP_PLAYBACK, NetworkKey, Node, NodeConfig, opus

RATE = 48000
BLOCK = 240  # 5 ms
PERIOD = 0.6
SECRETS = Path(__file__).resolve().parents[2] / "lanicom" / "esphome" / "secrets.yaml"


def click_track(n):
    t = np.arange(int(0.003 * RATE)) / RATE
    burst = (0.7 * np.sin(2 * np.pi * 2000 * t) * np.hanning(len(t))).astype(np.float32)
    lead = int(0.5 * RATE)
    track = np.zeros(lead + int(n * PERIOD * RATE) + RATE, np.float32)
    starts = [lead + int(i * PERIOD * RATE) for i in range(n)]
    for s in starts:
        track[s : s + len(burst)] = burst
    return track, starts


def onsets(signal, rate, min_gap, floor_factor=8.0, minimum=0.0):
    """Sample indices where |signal| first crosses a threshold, at least min_gap s apart."""
    env = np.abs(signal)
    thr = max(minimum, floor_factor * float(np.median(env)) + 1e-9)
    hits = []
    for i in np.flatnonzero(env > thr):
        if not hits or i - hits[-1] > min_gap * rate:
            hits.append(int(i))
    return hits, thr


async def switch_talk(host, on):
    key = yaml.safe_load(SECRETS.read_text())["api_encryption_key"]
    c = APIClient(host, 6053, None, noise_psk=key)
    await c.connect(login=True)
    ents, _ = await c.list_entities_services()
    talk = next(e for e in ents if isinstance(e, SwitchInfo) and e.object_id == "talk")
    c.switch_command(talk.key, on)
    await asyncio.sleep(0.3)
    await c.disconnect()


async def main(host="lanicom-p4-e80332.local", clicks="20"):
    n = int(clicks)
    track, starts = click_track(n)
    pos, playing = 0, False
    out_log, in_blocks = [], []  # (dac_time, first sample index) / (adc_time, samples)

    def callback(indata, outdata, frames, t, status):
        nonlocal pos
        if not playing:
            outdata.fill(0)
            return
        chunk = track[pos : pos + frames]
        outdata[: len(chunk), 0] = chunk
        outdata[len(chunk) :, 0] = 0
        out_log.append((t.outputBufferDacTime, pos))
        in_blocks.append((t.inputBufferAdcTime, indata[:, 0].copy()))
        pos += frames

    stream = sd.Stream(samplerate=RATE, blocksize=BLOCK, channels=1, dtype="float32", latency="low", callback=callback)
    arrivals = []  # (stream time, ts, opus packet)
    key = NetworkKey((Path.home() / ".config" / "lanicom" / "key").read_text())
    node = Node(key, NodeConfig(name="clicktest", caps=CAP_PLAYBACK))
    node.on_audio = lambda peer, sid, ts, pkt: arrivals.append((stream.time, ts, pkt))
    stream.start()
    await node.start()
    end = time.monotonic() + 10
    while not any(p.verified and p.announced for p in node.peers):
        if time.monotonic() > end:
            sys.exit("board not found (key, firewall?)")
        await asyncio.sleep(0.1)
    await switch_talk(host, True)
    await asyncio.sleep(0.5)  # bus switch + mic warm-up
    playing = True
    await asyncio.sleep(len(track) / RATE + 0.3)
    await switch_talk(host, False)
    stream.stop()
    await node.stop()

    # When each click was in the air: the laptop mic's view.
    mic = np.concatenate([b for _, b in in_blocks])
    mic_t0 = in_blocks[0][0]
    mic_hits, mic_thr = onsets(mic, RATE, PERIOD / 2)
    played = [out_log[0][0] + (s - out_log[0][1]) / RATE for s in starts]
    heard = [mic_t0 + i / RATE for i in mic_hits]

    # When each click's packet arrived from the board: decode in ts order, keep arrival times.
    arrivals.sort(key=lambda a: a[1])
    dec = opus.Decoder()
    pcm, owner = [], []
    for k, (_, _, pkt) in enumerate(arrivals):
        frame = np.frombuffer(dec.decode(pkt), dtype=np.int16).astype(np.float32) / 32768
        pcm.append(frame)
        owner += [k] * len(frame)
    board = np.concatenate(pcm) if pcm else np.zeros(1, np.float32)
    board_hits, board_thr = onsets(board, opus.SAMPLE_RATE, PERIOD / 2, minimum=0.02)
    received = [arrivals[owner[i]][0] for i in board_hits]

    print(f"clicks played {n}, heard by laptop mic {len(heard)}, received from board {len(received)}")
    offsets = []
    for p in played:
        near = [h - p for h in heard if -0.05 < h - p < 0.15]
        if near:
            offsets.append(near[0])
    if offsets:
        print(f"laptop: speaker->own mic (as PortAudio reports it) {statistics.median(offsets) * 1000:.1f} ms median")
    # Reference: the speaker's DAC time. The laptop mic, where it hears a click, confirms it
    # (it adds only its own small input latency).
    lat = []
    for h in played:
        near = [r - h for r in received if 0 < r - h < PERIOD / 2]
        if near:
            lat.append(near[0] * 1000)
    if not lat:
        sys.exit("no click matched; is the laptop next to the board?")
    lat.sort()
    p95 = lat[min(len(lat) - 1, math.ceil(0.95 * len(lat)) - 1)]
    print(f"click at the laptop speaker -> its packet back at the laptop: n={len(lat)} min {lat[0]:.1f}  "
          f"p50 {statistics.median(lat):.1f}  p95 {p95:.1f}  max {lat[-1]:.1f} ms")
    print("  " + " ".join(f"{x:.0f}" for x in lat))


if __name__ == "__main__":
    asyncio.run(main(*sys.argv[1:3]))
