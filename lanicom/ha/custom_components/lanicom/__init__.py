"""lanicom: Home Assistant as a peer on a serverless LAN intercom."""

from __future__ import annotations

import logging

import voluptuous as vol

from homeassistant.components import media_source
from homeassistant.components.ffmpeg import get_ffmpeg_manager
from homeassistant.components.media_player import async_process_play_media_url
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_NAME, Platform
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import ConfigEntryNotReady, HomeAssistantError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.typing import ConfigType

from .const import ATTR_BITRATE, ATTR_MEDIA, ATTR_TARGET, CONF_KEY, CONF_SENDER_ID, DOMAIN, SERVICE_ANNOUNCE
from .hub import LanicomHub
from .lanicom_lib import NetworkKey
from .lanicom_lib import opus

_LOGGER = logging.getLogger(__name__)
PLATFORMS = [Platform.BINARY_SENSOR, Platform.SENSOR]
CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

type LanicomConfigEntry = ConfigEntry[LanicomHub]

ANNOUNCE_SCHEMA = vol.Schema(
    {
        vol.Optional(ATTR_TARGET, default=["all"]): cv.ensure_list_csv,
        vol.Required(ATTR_MEDIA): cv.string,
        vol.Optional(ATTR_BITRATE, default=24000): vol.All(vol.Coerce(int), vol.Range(min=6000, max=64000)),
    }
)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    async def announce(call: ServiceCall) -> None:
        from .lanicom_lib.media import pcm_from_ffmpeg, send_pcm

        entries = [e for e in hass.config_entries.async_loaded_entries(DOMAIN)]
        if not entries:
            raise HomeAssistantError("lanicom is not set up")
        hub: LanicomHub = entries[0].runtime_data
        if not await hass.async_add_executor_job(opus.available):
            raise HomeAssistantError("libopus is not available on this system")
        targets = hub.resolve_targets(call.data[ATTR_TARGET])
        if not any(hub.node.peers.recipients(t) for t in targets):
            raise HomeAssistantError(f"No intercom devices match {', '.join(call.data[ATTR_TARGET])}")
        media_id = call.data[ATTR_MEDIA]
        if media_source.is_media_source_id(media_id):
            item = await media_source.async_resolve_media(hass, media_id, None)
            media_id = item.url
        url = async_process_play_media_url(hass, media_id)
        ffmpeg = get_ffmpeg_manager(hass).binary
        frames = await send_pcm(hub.node, targets, pcm_from_ffmpeg(url, ffmpeg), bitrate=call.data[ATTR_BITRATE])
        _LOGGER.debug("Announced %.1f s to %s", frames / 100, ", ".join(map(str, targets)))

    hass.services.async_register(DOMAIN, SERVICE_ANNOUNCE, announce, schema=ANNOUNCE_SCHEMA)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: LanicomConfigEntry) -> bool:
    key = await hass.async_add_executor_job(NetworkKey, entry.data[CONF_KEY])  # PBKDF2: keep it off the loop
    hub = LanicomHub(hass, entry.entry_id, key, entry.data[CONF_NAME], entry.data[CONF_SENDER_ID])
    try:
        await hub.async_start()
    except OSError as err:
        raise ConfigEntryNotReady(f"Cannot open UDP port {hub.node.config.port}: {err}") from err
    entry.runtime_data = hub
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: LanicomConfigEntry) -> bool:
    unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unloaded:
        await entry.runtime_data.async_stop()
    return unloaded
