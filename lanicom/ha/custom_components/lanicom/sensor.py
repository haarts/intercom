"""Round-trip time and last-seen per intercom device."""

from __future__ import annotations

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import EntityCategory, UnitOfTime
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
        async_add_entities([RttSensor(hub, state), LastSeenSensor(hub, state)])

    for state in hub.peers.values():
        add(state)
    entry.async_on_unload(async_dispatcher_connect(hass, SIGNAL_PEER_ADDED.format(entry.entry_id), add))


class RttSensor(LanicomEntity, SensorEntity):
    _attr_device_class = SensorDeviceClass.DURATION
    _attr_native_unit_of_measurement = UnitOfTime.MILLISECONDS
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_suggested_display_precision = 1
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, hub, state):
        super().__init__(hub, state, "rtt")

    @property
    def native_value(self):
        return self.peer.rtt_ms

    @property
    def extra_state_attributes(self) -> dict:
        return {"address": self.peer.address, "device_id": self.peer.id_hex}


class LastSeenSensor(LanicomEntity, SensorEntity):
    _attr_device_class = SensorDeviceClass.TIMESTAMP
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, hub, state):
        super().__init__(hub, state, "last_seen")

    @property
    def available(self) -> bool:
        return True  # most useful exactly when the device is gone

    @property
    def native_value(self):
        return self.peer.last_seen
