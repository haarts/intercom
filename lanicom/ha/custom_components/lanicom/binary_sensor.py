"""'Talking' per intercom device."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import LanicomConfigEntry
from .const import SIGNAL_PEER_ADDED
from .entity import LanicomEntity
from .hub import PeerState


async def async_setup_entry(
    hass: HomeAssistant, entry: LanicomConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    hub = entry.runtime_data

    @callback
    def add(state: PeerState) -> None:
        async_add_entities([TalkingSensor(hub, state)])

    for state in hub.peers.values():
        add(state)
    entry.async_on_unload(async_dispatcher_connect(hass, SIGNAL_PEER_ADDED.format(entry.entry_id), add))


class TalkingSensor(LanicomEntity, BinarySensorEntity):
    def __init__(self, hub, state):
        super().__init__(hub, state, "talking")

    @property
    def is_on(self) -> bool:
        return self.peer.talking

    @property
    def extra_state_attributes(self) -> dict:
        return {"target": self.peer.target} if self.peer.talking else {}
