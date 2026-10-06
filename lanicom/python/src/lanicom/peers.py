"""Peer table with epoch verification (PROTOCOL.md section 4)."""

from __future__ import annotations

from dataclasses import dataclass, field

from .control import CAP_MONITOR, CAP_PLAYBACK, Target
from .replay import ReplayWindow

PEER_TIMEOUT = 30.0
CHALLENGE_INTERVAL = 0.25
MAX_PEERS = 64


@dataclass
class Peer:
    sender_id: int
    addr: tuple[str, int]
    first_seen: float
    last_seen: float = 0.0
    name: str = ""
    zones: list[str] = field(default_factory=list)
    caps: int = 0
    verified_epoch: int | None = None
    window: ReplayWindow | None = None
    challenge: int = 0
    challenge_sent: float = -1e9
    rtt_ms: float | None = None
    announced: bool = False  # on_peer_added fired

    @property
    def verified(self) -> bool:
        return self.verified_epoch is not None

    @property
    def id_hex(self) -> str:
        return f"{self.sender_id:08x}"

    def matches(self, target: Target) -> bool:
        if target.device is not None:
            return self.sender_id == target.device
        if target.zone is not None:
            return target.zone in self.zones
        return target.all


class PeerTable:
    def __init__(self):
        self.peers: dict[int, Peer] = {}

    def get(self, sender_id: int) -> Peer | None:
        return self.peers.get(sender_id)

    def get_or_create(self, sender_id: int, addr: tuple[str, int], now: float) -> Peer | None:
        peer = self.peers.get(sender_id)
        if peer is None:
            if len(self.peers) >= MAX_PEERS:
                return None
            peer = self.peers[sender_id] = Peer(sender_id, addr, first_seen=now, last_seen=now)
        return peer

    def remove(self, sender_id: int) -> Peer | None:
        return self.peers.pop(sender_id, None)

    def expire(self, now: float) -> list[Peer]:
        gone = [p for p in self.peers.values() if now - p.last_seen > PEER_TIMEOUT]
        for p in gone:
            del self.peers[p.sender_id]
        return gone

    def recipients(self, target: Target) -> list[Peer]:
        """Who gets the audio of a stream to `target`."""
        return [p for p in self.peers.values() if p.verified and p.caps & CAP_PLAYBACK and p.matches(target)]

    def metadata_recipients(self, target: Target) -> list[Peer]:
        """Who gets TalkStart/TalkStop: the audio recipients plus every monitor."""
        return [
            p for p in self.peers.values()
            if p.verified and (p.caps & CAP_MONITOR or (p.caps & CAP_PLAYBACK and p.matches(target)))
        ]

    def __iter__(self):
        return iter(list(self.peers.values()))

    def __len__(self):
        return len(self.peers)
