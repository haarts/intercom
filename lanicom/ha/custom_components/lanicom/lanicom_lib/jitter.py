"""Adaptive jitter buffer, per-stream decoding and the mixer.

Frames are ordered by the payload `ts` (48 kHz ticks), never by packet seq.
"""

from __future__ import annotations

import array
from dataclasses import dataclass

from . import opus

TICKS_PER_SAMPLE = 3  # 48 kHz ts ticks per 16 kHz sample
_HALF = 1 << 31


def ts_diff(a: int, b: int) -> int:
    """a - b on the 32-bit wrapping timestamp circle."""
    return ((a - b + _HALF) & 0xFFFFFFFF) - _HALF


@dataclass
class JitterStats:
    received: int = 0
    late: int = 0
    duplicate: int = 0
    lost: int = 0
    fec: int = 0
    dropped_for_latency: int = 0


class JitterBuffer:
    """Decides what to decode next. pop() returns one of:

    ("frame", packet, samples), ("fec", next_packet, samples), ("plc", None, samples),
    or None while buffering (before playout starts) or when there is nothing to play.
    """

    def __init__(self, target_ms: int = 20, max_ms: int = 60, step_ms: int = 10):
        self.target = target_ms * 48
        self.max = max_ms * 48
        self.step = step_ms * 48
        self.frames: dict[int, tuple[bytes, int]] = {}  # ts -> (packet, ticks)
        self.next_ts: int | None = None  # None until playout starts
        self.last_ticks = 480
        self.stats = JitterStats()
        self._over_target = 0

    def _oldest(self) -> int:
        ref = next(iter(self.frames))
        return min(self.frames, key=lambda t: ts_diff(t, ref))

    def depth(self) -> int:
        """Buffered ticks from the playout point (or oldest frame) to the end of the newest frame."""
        if not self.frames:
            return 0
        start = self.next_ts if self.next_ts is not None else self._oldest()
        return max(ts_diff(ts, start) + ticks for ts, (_, ticks) in self.frames.items())

    def push(self, ts: int, packet: bytes) -> None:
        self.stats.received += 1
        if self.next_ts is not None and ts_diff(ts, self.next_ts) < 0:
            self.stats.late += 1
            self.target = min(self.target + self.step, self.max)
            return
        if ts in self.frames:
            self.stats.duplicate += 1
            return
        self.frames[ts] = (packet, opus.packet_samples(packet) * TICKS_PER_SAMPLE)
        # Never hold more than `max`: skip ahead, dropping the oldest frames.
        while self.next_ts is not None and self.depth() > self.max:
            self._skip()

    def _skip(self) -> None:
        _, ticks = self.frames.pop(self.next_ts, (None, self.last_ticks))
        self.next_ts = (self.next_ts + ticks) & 0xFFFFFFFF
        self.stats.dropped_for_latency += 1

    @property
    def started(self) -> bool:
        return self.next_ts is not None

    @property
    def empty(self) -> bool:
        return not self.frames

    def pop(self):
        if self.next_ts is None:
            if not self.frames or self.depth() < self.target:
                return None
            self.next_ts = self._oldest()
        if not self.frames:
            return None
        # Clock drift / a burst after a stall: if we sit well above target for a while, drop a frame.
        if self.depth() > self.target + 2 * self.last_ticks:
            self._over_target += 1
            if self._over_target >= 50:
                self._over_target = 0
                self._skip()
                if not self.frames:
                    return None
        else:
            self._over_target = 0
        hit = self.frames.pop(self.next_ts, None)
        if hit is not None:
            packet, ticks = hit
            self.next_ts = (self.next_ts + ticks) & 0xFFFFFFFF
            self.last_ticks = ticks
            return ("frame", packet, ticks // TICKS_PER_SAMPLE)
        gap = min(ts_diff(t, self.next_ts) for t in self.frames)
        if gap >= self.max:
            # The sender jumped (e.g. restarted its ts); resync rather than conceal for ages.
            self.next_ts = self._oldest()
            return self.pop()
        ticks = self.last_ticks
        self.stats.lost += 1
        self.next_ts = (self.next_ts + ticks) & 0xFFFFFFFF
        nxt = self.frames.get(self.next_ts)
        if nxt is not None:
            self.stats.fec += 1
            return ("fec", nxt[0], ticks // TICKS_PER_SAMPLE)
        return ("plc", None, ticks // TICKS_PER_SAMPLE)


class Stream:
    """One incoming talk spurt: jitter buffer + decoder + PCM FIFO."""

    CONCEAL_MS = 60  # conceal underruns this long after the last packet; then output silence

    def __init__(self, sender_id: int, stream_id: int, now: float, **jitter_args):
        self.sender_id = sender_id
        self.stream_id = stream_id
        self.jitter = JitterBuffer(**jitter_args)
        self.decoder = opus.Decoder()
        self.pcm = array.array("h")
        self.first_packet = now
        self.last_packet = now
        self.stopped = False

    def push(self, ts: int, packet: bytes, now: float) -> None:
        self.last_packet = now
        self.jitter.push(ts, packet)

    def read(self, samples: int, now: float) -> array.array:
        """Up to `samples` of PCM (short or empty while buffering or after the end)."""
        while len(self.pcm) < samples:
            item = self.jitter.pop()
            if item is None:
                if not self.jitter.started or self.stopped or now - self.last_packet > self.CONCEAL_MS / 1000:
                    break
                item = ("plc", None, self.jitter.last_ticks // TICKS_PER_SAMPLE)  # underrun mid-stream
            kind, packet, n = item
            if kind == "frame":
                out = self.decoder.decode(packet)
            elif kind == "fec":
                out = self.decoder.decode_fec(packet, n)
            else:
                out = self.decoder.conceal(n)
            self.pcm.frombytes(out)
        out = self.pcm[:samples]
        del self.pcm[:samples]
        return out

    def finished(self, now: float, timeout: float = 0.3) -> bool:
        idle = self.stopped or now - self.last_packet > timeout
        return idle and self.jitter.empty and not self.pcm


class Mixer:
    """Sums active streams with saturation."""

    def __init__(self, **jitter_args):
        self.streams: dict[tuple[int, int], Stream] = {}
        self.jitter_args = jitter_args

    def push(self, sender_id: int, stream_id: int, ts: int, packet: bytes, now: float) -> Stream:
        key = (sender_id, stream_id)
        stream = self.streams.get(key)
        if stream is None:
            stream = self.streams[key] = Stream(sender_id, stream_id, now, **self.jitter_args)
        stream.push(ts, packet, now)
        return stream

    def stop(self, sender_id: int, stream_id: int) -> None:
        stream = self.streams.get((sender_id, stream_id))
        if stream:
            stream.stopped = True

    def read(self, samples: int, now: float) -> tuple[bytes, list[Stream]]:
        """Mixed PCM (always `samples` long) and the streams that finished during this call."""
        acc = [0] * samples
        for stream in list(self.streams.values()):
            pcm = stream.read(samples, now)
            if pcm:
                for i, v in enumerate(pcm):
                    acc[i] += v
        ended = [s for s in self.streams.values() if s.finished(now)]
        for s in ended:
            del self.streams[(s.sender_id, s.stream_id)]
        out = array.array("h", (32767 if v > 32767 else -32768 if v < -32768 else v for v in acc))
        return out.tobytes(), ended
