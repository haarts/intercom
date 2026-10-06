"""Streaming PCM / media files into a talk (used by `lanicom send` and Home Assistant)."""

from __future__ import annotations

import asyncio
import time
from typing import AsyncIterator, Iterable, Sequence

from . import opus
from .control import Target
from .node import Node

FRAME_BYTES = opus.FRAME_10MS * 2


async def pcm_from_ffmpeg(source: str, ffmpeg: str = "ffmpeg") -> AsyncIterator[bytes]:
    """Decode any file or URL ffmpeg understands to 16 kHz mono s16le, in 10 ms chunks."""
    proc = await asyncio.create_subprocess_exec(
        ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-i", source,
        "-f", "s16le", "-ac", "1", "-ar", str(opus.SAMPLE_RATE), "-",
        stdout=asyncio.subprocess.PIPE,
    )
    try:
        while True:
            try:
                chunk = await proc.stdout.readexactly(FRAME_BYTES)
            except asyncio.IncompleteReadError as err:
                if err.partial:
                    yield err.partial + bytes(FRAME_BYTES - len(err.partial))
                break
            yield chunk
    finally:
        if proc.returncode is None:
            proc.kill()
        await proc.wait()


async def _aiter(chunks):
    if hasattr(chunks, "__aiter__"):
        async for c in chunks:
            yield c
    else:
        for c in chunks:
            yield c


async def send_pcm(
    node: Node,
    target: Target | Sequence[Target],
    chunks: AsyncIterator[bytes] | Iterable[bytes],
    bitrate: int = 24_000,
) -> int:
    """Encode 10 ms PCM chunks and send them paced in real time. Returns frames sent.

    With several targets (for example a list of devices), each gets its own stream; every
    frame is encoded once."""
    encoder = opus.Encoder(bitrate=bitrate)
    talks = [node.start_talk(t) for t in ([target] if isinstance(target, Target) else target)]
    frames = 0
    start = time.monotonic()
    try:
        async for chunk in _aiter(chunks):
            delay = start + frames * 0.01 - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            packet = encoder.encode(chunk)
            for talk in talks:
                talk.send_frame(packet, opus.FRAME_10MS)
            frames += 1
    finally:
        for talk in talks:
            talk.stop()
    return frames
