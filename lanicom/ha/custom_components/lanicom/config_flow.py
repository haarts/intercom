"""Config flow: the network key, plus how HA shows up to the other devices."""

from __future__ import annotations

import secrets
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_NAME
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import CONF_KEY, CONF_SENDER_ID, CONF_ZONES, DEFAULT_NAME, DOMAIN
from .lanicom_lib import NetworkKey


class LanicomConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        # A device advertised _lanicom._udp. We can't tell which network it's on without
        # the key, so only offer setup when no network is configured yet.
        if self._async_current_entries():
            return self.async_abort(reason="already_configured")
        await self.async_set_unique_id(DOMAIN + "_discovery")
        self._abort_if_unique_id_configured()
        return await self.async_step_user()

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            key_string = user_input[CONF_KEY].strip()
            zones = [z.strip().lower() for z in user_input.get(CONF_ZONES, "").split(",") if z.strip()]
            if len(key_string) < 8:
                errors[CONF_KEY] = "key_too_short"
            elif len(zones) > 8 or any(len(z) > 16 for z in zones):
                errors[CONF_ZONES] = "bad_zones"
            else:
                key = await self.hass.async_add_executor_job(NetworkKey, key_string)
                await self.async_set_unique_id(f"{key.key_id:04x}", raise_on_progress=False)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title=f"{user_input[CONF_NAME]} ({key.key_id:04x})",
                    data={
                        CONF_KEY: key_string,
                        CONF_NAME: user_input[CONF_NAME],
                        CONF_ZONES: ",".join(zones),
                        CONF_SENDER_ID: secrets.randbelow(0xFFFFFFFF) + 1,
                    },
                )
        schema = vol.Schema(
            {
                vol.Required(CONF_KEY): str,
                vol.Required(CONF_NAME, default=DEFAULT_NAME): str,
                vol.Optional(CONF_ZONES, default=""): str,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)
