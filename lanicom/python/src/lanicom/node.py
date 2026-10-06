"""An asyncio lanicom peer: discovery, epoch verification, talk and receive."""

from __future__ import annotations

import asyncio
import logging
import random
import secrets
import socket
import struct
import time
from collections import Counter
from dataclasses import dataclass, field
from typing import Callable

from . import control
from .audio import decode_audio, encode_audio
from .control import CAP_CAPTURE, CAP_PLAYBACK, Hello, TalkStart, TalkStop, Target
from .keys import NetworkKey
from .packet import TYPE_AUDIO, TYPE_CONTROL, Header, PacketError, open_packet, seal
from .peers import CHALLENGE_INTERVAL, Peer, PeerTable
from .replay import ReplayWindow

_LOGGER = logging.getLogger(__name__)

PORT = 47100
MULTICAST_GROUP = "239.255.76.73"
HELLO_INTERVAL = 5.0
HELLO_JITTER = 1.0
STARTUP_HELLOS = (0.0, 0.5, 1.5)
ECHO_LIMIT_PER_S = 10


def _nonzero_random32() -> int:
    return secrets.randbelow(0xFFFFFFFF) + 1


@dataclass
class NodeConfig:
    name: str
    zones: list[str] = field(default_factory=list)
    caps: int = CAP_PLAYBACK | CAP_CAPTURE
    port: int = PORT
    bind: str = "0.0.0.0"
    broadcast: str | None = "255.255.255.255"  # None disables broadcast discovery
    static_peers: list[str] = field(default_factory=list)  # "host" or "host:port"
    multicast: bool = False
    sender_id: int | None = None  # persisted by the caller if it wants a stable id
    rtt_probe: bool = False  # re-challenge verified peers each Hello round to measure RTT


class Talk:
    """An outgoing talk spurt. Feed it Opus frames in real time."""

    def __init__(self, node: "Node", target: Target):
        self.node = node
        self.target = target
        self.stream_id = _nonzero_random32()
        self.ts = secrets.randbits(32)
        self.frames = 0
        self.closed = False

    def send_frame(self, opus_packet: bytes, samples: int) -> int:
        """Send one Opus frame (`samples` at 16 kHz). Returns the number of recipients."""
        if self.closed:
            raise RuntimeError("talk closed")
        if self.frames < 3:
            self.node._send_talk_metadata(TalkStart(self.target, self.stream_id), self.target)
        n = self.node._send_payload(TYPE_AUDIO, encode_audio(self.stream_id, self.ts, opus_packet), self.target)
        self.ts = (self.ts + samples * 3) & 0xFFFFFFFF
        self.frames += 1
        return n

    def stop(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.frames:
            for _ in range(3):
                self.node._send_talk_metadata(TalkStop(self.stream_id), self.target)


class Node(asyncio.DatagramProtocol):
    def __init__(self, key: NetworkKey, config: NodeConfig):
        self.key = key
        self.config = config
        self.sender_id = config.sender_id or _nonzero_random32()
        self.epoch = secrets.randbits(64) or 1
        self.seq = 0
        self.peers = PeerTable()
        self.stats: Counter[str] = Counter()
        self.transport: asyncio.DatagramTransport | None = None
        self._tasks: list[asyncio.Task] = []
        self._echo_budget: dict[str, tuple[float, int]] = {}
        self._talks_seen: dict[tuple[int, int], float] = {}
        self._static: list[tuple[str, int]] = []
        # Callbacks (all optional; called from the event loop).
        self.on_peer_added: Callable[[Peer], None] | None = None
        self.on_peer_removed: Callable[[Peer], None] | None = None
        self.on_peer_updated: Callable[[Peer], None] | None = None
        self.on_talk_start: Callable[[Peer, TalkStart], None] | None = None
        self.on_talk_stop: Callable[[Peer, int], None] | None = None
        self.on_audio: Callable[[Peer, int, int, bytes], None] | None = None

    # --- lifecycle -------------------------------------------------------

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0xB8)  # DSCP EF: voice
        sock.bind((self.config.bind, self.config.port))
        if self.config.multicast:
            mreq = socket.inet_aton(MULTICAST_GROUP) + socket.inet_aton("0.0.0.0")
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
        sock.setblocking(False)
        for entry in self.config.static_peers:
            host, _, port = entry.partition(":")
            infos = await loop.getaddrinfo(host, int(port or self.config.port), family=socket.AF_INET, type=socket.SOCK_DGRAM)
            self._static.append(infos[0][4][:2])
        await loop.create_datagram_endpoint(lambda: self, sock=sock)
        self._tasks = [loop.create_task(self._hello_loop()), loop.create_task(self._expiry_loop())]
        _LOGGER.info("lanicom node %08x (%s) on port %d", self.sender_id, self.config.name, self.config.port)

    async def stop(self) -> None:
        for task in self._tasks:
            task.cancel()
        if self.transport:
            # Unicast only: a broadcast copy too would arrive after the peer removed us
            # and re-create us there as an unverified stranger.
            for peer in self.peers:
                if peer.verified:
                    self._send_hello_to(peer.addr, bye=True)
            self.transport.close()
            self.transport = None

    def connection_made(self, transport) -> None:
        self.transport = transport

    # --- sending ---------------------------------------------------------

    def _next_header(self, type_: int) -> Header:
        if self.seq > 0xFFFFFFFF:
            self.epoch = secrets.randbits(64) or 1
            self.seq = 0
        header = Header(type_, self.key.key_id, self.sender_id, self.epoch, self.seq)
        self.seq += 1
        return header

    def _sendto(self, data: bytes, addr: tuple[str, int]) -> None:
        if self.transport is None:
            return
        try:
            self.transport.sendto(data, addr)
            self.stats["tx"] += 1
        except OSError as err:
            self.stats["tx_error"] += 1
            _LOGGER.debug("send to %s failed: %s", addr, err)

    def _seal(self, type_: int, plaintext: bytes) -> bytes:
        return seal(self.key, self._next_header(type_), plaintext)

    def _send_payload(self, type_: int, plaintext: bytes, target: Target) -> int:
        """Seal once; send to the multicast group or fan out. Returns recipient count."""
        recipients = self.peers.recipients(target)
        if not recipients:
            return 0
        data = self._seal(type_, plaintext)
        if self.config.multicast and target.all:
            self._sendto(data, (MULTICAST_GROUP, self.config.port))
        else:
            for peer in recipients:
                self._sendto(data, peer.addr)
        return len(recipients)

    def _send_talk_metadata(self, msg: control.Control, target: Target) -> None:
        """TalkStart/TalkStop: unicast to the stream's recipients and all monitors."""
        recipients = self.peers.metadata_recipients(target)
        if recipients:
            data = self._seal(TYPE_CONTROL, control.encode(msg))
            for peer in recipients:
                self._sendto(data, peer.addr)

    def _hello(self, **kw) -> Hello:
        return Hello(name=self.config.name, zones=list(self.config.zones), caps=self.config.caps, **kw)

    def _send_hello_to(self, addr: tuple[str, int], **kw) -> None:
        self._sendto(self._seal(TYPE_CONTROL, control.encode(self._hello(**kw))), addr)

    def _broadcast_hello(self, **kw) -> None:
        data = self._seal(TYPE_CONTROL, control.encode(self._hello(**kw)))
        if self.config.broadcast:
            self._sendto(data, (self.config.broadcast, self.config.port))
        for addr in self._static:
            self._sendto(data, addr)

    def _challenge(self, peer: Peer, now: float) -> None:
        if now - peer.challenge_sent < CHALLENGE_INTERVAL:
            return
        peer.challenge = secrets.randbits(64) or 1
        peer.challenge_sent = now
        self._send_hello_to(peer.addr, challenge=peer.challenge)

    def start_talk(self, target: Target | None = None) -> Talk:
        return Talk(self, target or Target.everyone())

    def announce_changed(self) -> None:
        """Call after changing config.name/zones/caps."""
        self._broadcast_hello()
        for peer in self.peers:
            if peer.verified:
                self._send_hello_to(peer.addr)

    # --- periodic --------------------------------------------------------

    async def _hello_loop(self) -> None:
        last = 0.0
        for at in STARTUP_HELLOS:
            await asyncio.sleep(at - last)
            last = at
            self._broadcast_hello()
        while True:
            await asyncio.sleep(HELLO_INTERVAL + random.uniform(-HELLO_JITTER, HELLO_JITTER))
            self._broadcast_hello()
            if self.config.rtt_probe:
                now = time.monotonic()
                for peer in self.peers:
                    if peer.verified:
                        self._challenge(peer, now)

    async def _expiry_loop(self) -> None:
        while True:
            await asyncio.sleep(1.0)
            now = time.monotonic()
            for peer in self.peers.expire(now):
                self._peer_gone(peer)
            for key, seen in list(self._talks_seen.items()):
                if now - seen > 10:
                    del self._talks_seen[key]

    def _peer_gone(self, peer: Peer) -> None:
        _LOGGER.info("peer %s (%s) gone", peer.id_hex, peer.name)
        if peer.announced and self.on_peer_removed:
            self.on_peer_removed(peer)

    # --- receiving -------------------------------------------------------

    def datagram_received(self, data: bytes, addr) -> None:
        try:
            self._receive(data, (addr[0], addr[1]), time.monotonic())
        except Exception:  # never let one bad packet kill the protocol
            self.stats["error"] += 1
            _LOGGER.exception("error handling packet from %s", addr)

    def _receive(self, data: bytes, addr: tuple[str, int], now: float) -> None:
        try:
            header = Header.parse(data)
        except PacketError as err:
            self.stats[err.reason] += 1
            return
        if header.key_id != self.key.key_id:
            self.stats["foreign"] += 1
            return
        own = header.sender_id == self.sender_id
        if own and header.epoch == self.epoch:
            return  # our own broadcast
        try:
            header, plain = open_packet(self.key, data)
        except PacketError as err:
            self.stats[err.reason] += 1
            return
        if own:
            self.stats["collision"] += 1
            self.sender_id = _nonzero_random32()
            _LOGGER.warning("sender id collision; now %08x", self.sender_id)
            return

        msg = None
        if header.type == TYPE_CONTROL:
            try:
                msg = control.decode(plain)
            except control.ControlError:
                self.stats["malformed"] += 1
                return

        peer = self.peers.get_or_create(header.sender_id, addr, now)
        if peer is None:
            self.stats["peer_table_full"] += 1
            return

        if isinstance(msg, Hello) and msg.challenge and self._echo_allowed(addr[0], now):
            self._send_hello_to(addr, echo=msg.challenge)

        if header.epoch != peer.verified_epoch:
            if isinstance(msg, Hello) and peer.challenge and msg.echo == peer.challenge:
                self._verified(peer, header, now)
            else:
                self.stats["unverified"] += 1
                if not peer.verified:
                    peer.addr = addr  # only an unverified peer's address follows unverified packets
                self._challenge(peer, now)
                return
        elif not peer.window.check_and_update(header.seq):
            self.stats["replay"] += 1
            return
        elif isinstance(msg, Hello) and peer.challenge and msg.echo == peer.challenge:
            peer.rtt_ms = (now - peer.challenge_sent) * 1000
            peer.challenge = 0

        peer.addr = addr
        peer.last_seen = now
        if header.type == TYPE_AUDIO:
            self._handle_audio(peer, plain, now)
        elif msg is not None:
            self._handle_control(peer, msg, now)

    def _echo_allowed(self, host: str, now: float) -> bool:
        window, count = self._echo_budget.get(host, (now, 0))
        if now - window >= 1.0:
            window, count = now, 0
        if count >= ECHO_LIMIT_PER_S:
            return False
        self._echo_budget[host] = (window, count + 1)
        if len(self._echo_budget) > 256:
            self._echo_budget = {h: v for h, v in self._echo_budget.items() if now - v[0] < 1.0}
        return True

    def _verified(self, peer: Peer, header: Header, now: float) -> None:
        if peer.verified:
            _LOGGER.info("peer %s rebooted (new epoch)", peer.id_hex)
        peer.verified_epoch = header.epoch
        peer.window = ReplayWindow(header.seq)
        peer.rtt_ms = (now - peer.challenge_sent) * 1000
        peer.challenge = 0
        self.stats["verified"] += 1

    def _handle_control(self, peer: Peer, msg: control.Control, now: float) -> None:
        self.stats["rx_control"] += 1
        if isinstance(msg, Hello):
            if msg.bye:
                if self.peers.remove(peer.sender_id):
                    self._peer_gone(peer)
                return
            changed = (msg.name, msg.zones, msg.caps) != (peer.name, peer.zones, peer.caps)
            peer.name, peer.zones, peer.caps = msg.name, list(msg.zones), msg.caps
            if not peer.announced:
                peer.announced = True
                _LOGGER.info("peer %s (%s) at %s:%d, zones %s", peer.id_hex, peer.name, *peer.addr, peer.zones)
                if self.on_peer_added:
                    self.on_peer_added(peer)
            elif changed and self.on_peer_updated:
                self.on_peer_updated(peer)
        elif isinstance(msg, TalkStart):
            key = (peer.sender_id, msg.stream_id)
            if key not in self._talks_seen:
                self._talks_seen[key] = now
                if self.on_talk_start:
                    self.on_talk_start(peer, msg)
        elif isinstance(msg, TalkStop):
            key = (peer.sender_id, msg.stream_id)
            if self._talks_seen.get(key) != -1.0:
                self._talks_seen[key] = -1.0  # stopped; kept so repeats are ignored
                if self.on_talk_stop:
                    self.on_talk_stop(peer, msg.stream_id)

    def _handle_audio(self, peer: Peer, plain: bytes, now: float) -> None:
        try:
            stream_id, ts, packet = decode_audio(plain)
        except ValueError:
            self.stats["malformed"] += 1
            return
        self.stats["rx_audio"] += 1
        if self.on_audio:
            self.on_audio(peer, stream_id, ts, packet)


def parse_sender_id(text: str) -> int:
    return struct.unpack(">I", bytes.fromhex(text.zfill(8)))[0]
