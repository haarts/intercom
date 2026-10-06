"""The Home Assistant peer: one lanicom node per config entry."""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.event import async_track_time_interval
from homeassistant.util import dt as dt_util

from .const import EVENT_TALK_START, EVENT_TALK_STOP, SIGNAL_PEER_ADDED, SIGNAL_UPDATE, TALK_TIMEOUT_S
from .lanicom_lib import CAP_CAPTURE, CAP_MONITOR, NetworkKey, Node, NodeConfig, Target
from .lanicom_lib.control import TalkStart
from .lanicom_lib.peers import Peer

_LOGGER = logging.getLogger(__name__)


@dataclass
class PeerState:
    """What the entities show for one intercom device."""

    sender_id: int
    name: str
    zones: list[str]
    address: str
    online: bool = True
    talking: bool = False
    talking_since: float = 0.0
    target: str | None = None
    rtt_ms: float | None = None
    last_seen: datetime | None = None
    extra: dict = field(default_factory=dict)

    @property
    def id_hex(self) -> str:
        return f"{self.sender_id:08x}"


class LanicomHub:
    def __init__(self, hass: HomeAssistant, entry_id: str, key: NetworkKey, name: str, zones: list[str], sender_id: int):
        self.hass = hass
        self.entry_id = entry_id
        # HA talks (announcements) and wants talk events, but plays no audio: devices don't stream to it.
        self.node = Node(key, NodeConfig(name=name, zones=zones, caps=CAP_CAPTURE | CAP_MONITOR, sender_id=sender_id, rtt_probe=True))
        self.peers: dict[int, PeerState] = {}
        self._unsub_interval = None
        self.node.on_peer_added = self._peer_added
        self.node.on_peer_updated = self._peer_updated
        self.node.on_peer_removed = self._peer_removed
        self.node.on_talk_start = self._talk_start
        self.node.on_talk_stop = self._talk_stop

    async def async_start(self) -> None:
        await self.node.start()
        self._unsub_interval = async_track_time_interval(self.hass, self._refresh, timedelta(seconds=10))

    async def async_stop(self) -> None:
        if self._unsub_interval:
            self._unsub_interval()
        await self.node.stop()

    # --- node callbacks (event loop) ------------------------------------------

    def _state_from(self, peer: Peer) -> PeerState:
        state = self.peers.get(peer.sender_id)
        if state is None:
            state = self.peers[peer.sender_id] = PeerState(peer.sender_id, peer.name, list(peer.zones), peer.addr[0])
        state.name, state.zones, state.address = peer.name, list(peer.zones), peer.addr[0]
        state.online = True
        state.rtt_ms = peer.rtt_ms
        state.last_seen = dt_util.utcnow()
        return state

    @callback
    def _peer_added(self, peer: Peer) -> None:
        new = peer.sender_id not in self.peers
        state = self._state_from(peer)
        if new:
            async_dispatcher_send(self.hass, SIGNAL_PEER_ADDED.format(self.entry_id), state)
        self._notify()

    @callback
    def _peer_updated(self, peer: Peer) -> None:
        self._state_from(peer)
        self._notify()

    @callback
    def _peer_removed(self, peer: Peer) -> None:
        state = self.peers.get(peer.sender_id)
        if state:
            state.online = False
            state.talking = False
        self._notify()

    @callback
    def _talk_start(self, peer: Peer, msg: TalkStart) -> None:
        state = self._state_from(peer)
        state.talking, state.talking_since, state.target = True, time.monotonic(), str(msg.target)
        self.hass.bus.async_fire(
            EVENT_TALK_START,
            {"device_id": state.id_hex, "name": state.name, "target": str(msg.target), "stream_id": msg.stream_id},
        )
        self._notify()

    @callback
    def _talk_stop(self, peer: Peer, stream_id: int) -> None:
        state = self._state_from(peer)
        state.talking = False
        self.hass.bus.async_fire(EVENT_TALK_STOP, {"device_id": state.id_hex, "name": state.name, "stream_id": stream_id})
        self._notify()

    @callback
    def _refresh(self, _now=None) -> None:
        now = time.monotonic()
        for peer in self.node.peers:
            state = self.peers.get(peer.sender_id)
            if state and peer.verified:
                state.rtt_ms = peer.rtt_ms
                state.last_seen = dt_util.utcnow() - timedelta(seconds=now - peer.last_seen)
        for state in self.peers.values():
            if state.talking and now - state.talking_since > TALK_TIMEOUT_S:
                state.talking = False
        self._notify()

    @callback
    def _notify(self) -> None:
        async_dispatcher_send(self.hass, SIGNAL_UPDATE.format(self.entry_id))

    # --- announcements ------------------------------------------------------------

    def resolve_target(self, text: str) -> Target:
        """'all', 'zone:<z>', 'device:<id>', or a device's name."""
        text = text.strip()
        if text in ("", "all"):
            return Target.everyone()
        kind, _, value = text.partition(":")
        if kind == "zone" and value:
            return Target(zone=value.lower())
        if kind == "device" and value:
            try:
                return Target(device=int(value, 16))
            except ValueError:
                pass
        for state in self.peers.values():
            if state.online and state.name.casefold() == text.casefold():
                return Target(device=state.sender_id)
        raise HomeAssistantError(f"Unknown intercom target '{text}': use all, zone:<name>, device:<id> or a device name")
