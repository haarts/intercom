"""Base entity: one HA device per intercom device."""

from __future__ import annotations

from homeassistant.core import callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity import Entity

from .const import DOMAIN, SIGNAL_UPDATE
from .hub import LanicomHub, PeerState


class LanicomEntity(Entity):
    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, hub: LanicomHub, state: PeerState, key: str):
        self.hub = hub
        self.peer = state
        self._attr_unique_id = f"{state.id_hex}_{key}"
        self._attr_translation_key = key
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, state.id_hex)},
            name=state.name or state.id_hex,
            manufacturer="lanicom",
            model="Intercom peer",
        )

    @property
    def available(self) -> bool:
        return self.peer.online

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(
            async_dispatcher_connect(self.hass, SIGNAL_UPDATE.format(self.hub.entry_id), self._updated)
        )

    @callback
    def _updated(self) -> None:
        self.async_write_ha_state()
