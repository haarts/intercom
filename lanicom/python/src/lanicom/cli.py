"""`lanicom` command line peer: keygen, list, listen, talk, record, send."""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import socket
import sys
import threading
import time
from pathlib import Path

from . import opus
from .control import CAP_CAPTURE, CAP_PLAYBACK, Target
from .jitter import Mixer
from .keys import NetworkKey, generate_key_string
from .node import PORT, Node, NodeConfig, parse_sender_id

CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "lanicom"


def parse_target(text: str) -> Target:
    if text == "all":
        return Target.everyone()
    kind, _, value = text.partition(":")
    if kind == "zone" and value:
        return Target(zone=value)
    if kind == "device" and value:
        return Target(device=parse_sender_id(value))
    raise argparse.ArgumentTypeError("target is 'all', 'zone:<name>' or 'device:<id>'")


def load_key(args) -> NetworkKey:
    text = args.key or os.environ.get("LANICOM_KEY")
    if not text:
        path = Path(args.key_file) if args.key_file else CONFIG_DIR / "key"
        if not path.exists():
            sys.exit(f"no network key: pass --key, set LANICOM_KEY or write it to {path} (see `lanicom keygen`)")
        text = path.read_text()
    return NetworkKey(text)


def persistent_sender_id() -> int | None:
    """A stable id per machine, so peers don't see a new device on every run."""
    path = CONFIG_DIR / "sender_id"
    try:
        return parse_sender_id(path.read_text().strip())
    except (OSError, ValueError):
        pass
    import secrets

    sid = secrets.randbelow(0xFFFFFFFF) + 1
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        path.write_text(f"{sid:08x}\n")
    except OSError:
        return None
    return sid


def make_node(args, caps: int) -> Node:
    config = NodeConfig(
        name=args.name,
        zones=[z for z in args.zones.split(",") if z] if args.zones else [],
        caps=caps,
        port=args.port,
        bind=args.bind,
        static_peers=args.peer or [],
        multicast=args.multicast,
        sender_id=persistent_sender_id(),
        broadcast=None if args.no_broadcast else "255.255.255.255",
    )
    return Node(load_key(args), config)


# --- audio devices (optional dependency: sounddevice) ----------------------


class Player:
    """Plays the node's mixed incoming streams on the default output device."""

    def __init__(self, node: Node, target_ms: int):
        import sounddevice as sd

        self.mixer = Mixer(target_ms=target_ms)
        self.lock = threading.Lock()
        self.muted = False
        self.stream = sd.RawOutputStream(
            samplerate=opus.SAMPLE_RATE, channels=1, dtype="int16", blocksize=opus.FRAME_10MS,
            latency="low", callback=self._callback,
        )
        node.on_audio = self._on_audio
        node.on_talk_stop = lambda peer, sid: self._locked(self.mixer.stop, peer.sender_id, sid)

    def _locked(self, fn, *a):
        with self.lock:
            return fn(*a)

    def _on_audio(self, peer, stream_id, ts, packet):
        with self.lock:
            self.mixer.push(peer.sender_id, stream_id, ts, packet, time.monotonic())

    def _callback(self, outdata, frames, _time, _status):
        with self.lock:
            pcm, _ = self.mixer.read(frames, time.monotonic())
        outdata[:] = bytes(len(pcm)) if self.muted else pcm

    def start(self):
        self.stream.start()


class Recorder:
    """Captures 10 ms frames from the default input and sends them while a talk is open."""

    def __init__(self, node: Node, loop: asyncio.AbstractEventLoop, bitrate: int):
        import sounddevice as sd

        self.node, self.loop = node, loop
        self.encoder = opus.Encoder(bitrate=bitrate)
        self.talk = None
        self.stream = sd.RawInputStream(
            samplerate=opus.SAMPLE_RATE, channels=1, dtype="int16", blocksize=opus.FRAME_10MS,
            latency="low", callback=self._callback,
        )

    def _callback(self, indata, frames, _time, _status):
        if self.talk is not None and frames == opus.FRAME_10MS:
            packet = self.encoder.encode(bytes(indata))
            self.loop.call_soon_threadsafe(self._send, packet)

    def _send(self, packet):
        if self.talk is not None and not self.talk.closed:
            self.talk.send_frame(packet, opus.FRAME_10MS)

    def start(self):
        self.stream.start()


def _need_sounddevice():
    try:
        import sounddevice  # noqa: F401
    except (ImportError, OSError) as err:
        sys.exit(f"audio needs the 'sounddevice' package and PortAudio: pip install 'lanicom[audio]' ({err})")


# --- commands --------------------------------------------------------------


def print_events(node: Node) -> None:
    node.on_peer_added = lambda p: print(f"+ {p.id_hex} {p.name!r} {p.addr[0]} zones={','.join(p.zones) or '-'}")
    node.on_peer_removed = lambda p: print(f"- {p.id_hex} {p.name!r}")
    node.on_talk_start = lambda p, m: print(f"> {p.name or p.id_hex} talking to {m.target}")
    prev = node.on_talk_stop

    def stop(p, sid):
        print(f"< {p.name or p.id_hex} done")
        if prev:
            prev(p, sid)

    node.on_talk_stop = stop


async def cmd_list(args) -> None:
    node = make_node(args, caps=0)
    await node.start()
    await asyncio.sleep(args.wait)
    print(f"{'id':8}  {'name':20} {'address':15} {'zones':20} {'rtt':>7}")
    for p in sorted(node.peers, key=lambda p: p.name):
        if p.verified:
            rtt = f"{p.rtt_ms:.1f}ms" if p.rtt_ms is not None else "-"
            print(f"{p.id_hex}  {p.name[:20]:20} {p.addr[0]:15} {','.join(p.zones)[:20]:20} {rtt:>7}")
    dropped = {k: v for k, v in node.stats.items() if k in ("foreign", "auth", "malformed", "replay")}
    if dropped:
        print(f"dropped: {dropped}")
    await node.stop()


async def cmd_listen(args) -> None:
    _need_sounddevice()
    node = make_node(args, caps=CAP_PLAYBACK)
    player = Player(node, args.jitter_ms)
    print_events(node)
    await node.start()
    player.start()
    print(f"listening as {node.config.name!r} ({node.sender_id:08x}); Ctrl-C to quit")
    try:
        await asyncio.Event().wait()
    finally:
        await node.stop()


async def cmd_talk(args) -> None:
    _need_sounddevice()
    node = make_node(args, caps=CAP_PLAYBACK | CAP_CAPTURE)
    loop = asyncio.get_running_loop()
    player = Player(node, args.jitter_ms)
    recorder = Recorder(node, loop, args.bitrate)
    print_events(node)
    await node.start()
    player.start()
    recorder.start()
    print(f"talk to {args.to}: press Enter to start talking, Enter again to stop (Ctrl-C quits)")
    reader = asyncio.StreamReader()
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    try:
        while await reader.readline():
            if recorder.talk is None:
                recorder.talk = node.start_talk(args.to)
                player.muted = True  # half duplex
                print("● talking … (Enter to stop)")
            else:
                recorder.talk.stop()
                recorder.talk = None
                player.muted = False
                print("○ stopped")
    finally:
        await node.stop()


async def cmd_record(args) -> None:
    """Write what peers say to a WAV file (16 kHz mono), e.g. for tools/voicetest."""
    import wave

    node = make_node(args, caps=CAP_PLAYBACK)
    mixer = Mixer(target_ms=args.jitter_ms)
    node.on_audio = lambda peer, sid, ts, pkt: mixer.push(peer.sender_id, sid, ts, pkt, time.monotonic())
    node.on_talk_stop = lambda peer, sid: mixer.stop(peer.sender_id, sid)
    print_events(node)
    await node.start()
    print(f"recording to {args.file} for {args.duration:g} s")
    with wave.open(args.file, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(opus.SAMPLE_RATE)
        start = time.monotonic()
        blocks = int(args.duration * 100)
        for i in range(blocks):
            await asyncio.sleep(max(0.0, start + (i + 1) * 0.01 - time.monotonic()))
            pcm, _ = mixer.read(opus.FRAME_10MS, time.monotonic())
            out.writeframes(pcm)
    await node.stop()


async def cmd_send(args) -> None:
    from .media import pcm_from_ffmpeg, send_pcm

    node = make_node(args, caps=CAP_CAPTURE)
    await node.start()
    await asyncio.sleep(args.wait)  # let discovery and verification finish
    targets = node.peers.recipients(args.to)
    if not targets:
        await node.stop()
        sys.exit(f"no peers match {args.to}")
    print(f"sending {args.file} to {', '.join(p.name or p.id_hex for p in targets)}")
    frames = await send_pcm(node, args.to, pcm_from_ffmpeg(args.file), bitrate=args.bitrate)
    print(f"sent {frames / 100:.1f} s")
    await node.stop()


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(prog="lanicom", description="Serverless LAN intercom peer")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="cmd", required=True)

    keygen = sub.add_parser("keygen", help="generate a network key")
    keygen.add_argument("--save", action="store_true", help=f"also write it to {CONFIG_DIR / 'key'}")
    keygen.add_argument("--qr", action="store_true", help="print it as a QR code (needs the 'qrcode' package)")

    def common(p):
        p.add_argument("--key", help="network key (default: $LANICOM_KEY or ~/.config/lanicom/key)")
        p.add_argument("--key-file")
        p.add_argument("--name", default=socket.gethostname())
        p.add_argument("--zones", default="", help="comma-separated zones this peer belongs to")
        p.add_argument("--port", type=int, default=PORT)
        p.add_argument("--bind", default="0.0.0.0", help="local address to listen on")
        p.add_argument("--peer", action="append", help="static peer host[:port] (repeatable)")
        p.add_argument("--no-broadcast", action="store_true", help="only use --peer addresses")
        p.add_argument("--multicast", action="store_true")
        return p

    p = common(sub.add_parser("list", help="show the peers on this network"))
    p.add_argument("--wait", type=float, default=2.0)
    p = common(sub.add_parser("listen", help="play what others say"))
    p.add_argument("--jitter-ms", type=int, default=30)
    p = common(sub.add_parser("talk", help="talk (Enter toggles) and listen"))
    p.add_argument("--to", type=parse_target, default=Target.everyone(), help="all | zone:<name> | device:<id>")
    p.add_argument("--jitter-ms", type=int, default=30)
    p.add_argument("--bitrate", type=int, default=24000)
    p = common(sub.add_parser("record", help="record what peers say to a WAV file"))
    p.add_argument("file")
    p.add_argument("--duration", type=float, default=10.0, help="seconds")
    p.add_argument("--jitter-ms", type=int, default=30)
    p = common(sub.add_parser("send", help="play an audio file or URL on peers (needs ffmpeg)"))
    p.add_argument("file")
    p.add_argument("--to", type=parse_target, default=Target.everyone())
    p.add_argument("--wait", type=float, default=1.0)
    p.add_argument("--bitrate", type=int, default=24000)

    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.WARNING, format="%(levelname)s %(message)s")

    if args.cmd == "keygen":
        key = generate_key_string()
        print(key)
        if args.save:
            CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            path = CONFIG_DIR / "key"
            path.write_text(key + "\n")
            path.chmod(0o600)
            print(f"saved to {path}", file=sys.stderr)
        if args.qr:
            try:
                import qrcode
            except ImportError:
                sys.exit("pip install qrcode")
            qr = qrcode.QRCode(border=1)
            qr.add_data(key)
            qr.print_ascii(invert=True)
        return
    commands = {"list": cmd_list, "listen": cmd_listen, "talk": cmd_talk, "record": cmd_record, "send": cmd_send}
    try:
        asyncio.run(commands[args.cmd](args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
