# SPDX-License-Identifier: MIT
"""FRITZ! Virtual Bridge integration."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import device_registry as dr

from .bridge import FritzVirtualBridge
from .const import CONF_BRIDGE_ID, CONF_FRITZ_URL, DEFAULT_FRITZ_URL, DOMAIN

type FritzVirtualBridgeConfigEntry = ConfigEntry[FritzVirtualBridge]


async def async_setup_entry(
    hass: HomeAssistant, entry: FritzVirtualBridgeConfigEntry
) -> bool:
    """Set up a configured bridge."""
    bridge = FritzVirtualBridge(hass, entry)
    await bridge.async_start()
    entry.runtime_data = bridge
    dr.async_get(hass).async_get_or_create(
        config_entry_id=entry.entry_id,
        identifiers={(DOMAIN, entry.data[CONF_BRIDGE_ID])},
        name=f"FRITZ! Virtual Bridge {entry.data[CONF_BRIDGE_ID]}",
        manufacturer="FRITZ! Virtual Bridge project",
        model="Local MQTT bridge",
        configuration_url=entry.data.get(CONF_FRITZ_URL, DEFAULT_FRITZ_URL),
    )
    entry.async_on_unload(entry.add_update_listener(_async_reload_entry))
    return True


async def async_unload_entry(
    hass: HomeAssistant, entry: FritzVirtualBridgeConfigEntry
) -> bool:
    """Unload a configured bridge."""
    await entry.runtime_data.async_stop()
    return True


async def _async_reload_entry(
    hass: HomeAssistant, entry: FritzVirtualBridgeConfigEntry
) -> None:
    """Reload after bridge or device-subentry changes."""
    await hass.config_entries.async_reload(entry.entry_id)
