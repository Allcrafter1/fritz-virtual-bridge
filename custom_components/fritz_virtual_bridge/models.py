# SPDX-License-Identifier: MIT
"""Transport-independent bridge models."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from urllib.parse import urlsplit, urlunsplit

from .const import PROTOCOL_VERSION, DeviceProfile

_BRIDGE_ID = re.compile(r"^[a-z0-9][a-z0-9_-]{2,31}$")
_ENDPOINT_UID = re.compile(r"^FVB[0-9A-F]{16}$")


@dataclass(frozen=True, slots=True)
class VirtualDevice:
    """Persistent mapping sent to the bridge."""

    endpoint_uid: str
    profile: DeviceProfile
    name: str
    source_entity_id: str
    enabled: bool = True

    def __post_init__(self) -> None:
        if not _ENDPOINT_UID.fullmatch(self.endpoint_uid):
            raise ValueError("invalid endpoint UID")
        if not self.name.strip() or len(self.name.encode("utf-8")) > 79:
            raise ValueError("name must contain 1..79 UTF-8 bytes")
        if "." not in self.source_entity_id:
            raise ValueError("invalid Home Assistant entity ID")

    def management_payload(self, request_id: str) -> dict[str, object]:
        """Return a versioned idempotent upsert request."""
        return {
            "schema_version": PROTOCOL_VERSION,
            "request_id": request_id,
            "operation": "upsert_device",
            "device": {
                "uid": self.endpoint_uid,
                "profile": self.profile.value,
                "name": self.name,
                "enabled": self.enabled,
            },
        }


def validate_bridge_id(value: str) -> str:
    """Validate a bridge ID used as one MQTT topic component."""
    value = value.strip().lower()
    if not _BRIDGE_ID.fullmatch(value):
        raise ValueError("invalid bridge ID")
    return value


def validate_fritz_url(value: str) -> str:
    """Validate and normalize the local FRITZ!Box configuration URL."""
    value = value.strip()
    parsed = urlsplit(value)
    if (
        parsed.scheme not in {"http", "https"}
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError("invalid FRITZ!Box URL")
    return urlunsplit((parsed.scheme, parsed.netloc, parsed.path.rstrip("/"), "", ""))


def endpoint_uid_from_token(token: str) -> str:
    """Create the stable 19-character FRITZ UID from a random token."""
    digest = hashlib.sha256(token.encode("utf-8")).hexdigest()[:16].upper()
    return f"FVB{digest}"


def bridge_details_from_discovery(topic: str, payload: str | bytes) -> tuple[str, str]:
    """Validate a retained bridge announcement and return ID and topic root."""
    parts = topic.strip("/").split("/")
    if len(parts) < 4 or parts[-2:] != ["bridge", "info"]:
        raise ValueError("invalid discovery topic")
    bridge_id = validate_bridge_id(parts[-3])
    topic_root = "/".join(parts[:-3])
    if not topic_root:
        raise ValueError("empty discovery topic root")
    try:
        document = json.loads(payload)
    except (TypeError, UnicodeDecodeError, json.JSONDecodeError) as err:
        raise ValueError("invalid discovery payload") from err
    if not isinstance(document, dict) or document.get("bridge_id") != bridge_id:
        raise ValueError("discovery identity mismatch")
    if document.get("schema_version") != PROTOCOL_VERSION:
        raise ValueError("unsupported discovery schema")
    return bridge_id, topic_root


def profile_for_domain(
    domain: str, *, color_temperature: bool = False
) -> DeviceProfile:
    """Return the default virtual profile for a Home Assistant domain."""
    if domain in {"switch", "input_boolean"}:
        return DeviceProfile.SWITCH
    if domain == "light":
        return (
            DeviceProfile.COLOR_TEMPERATURE_LIGHT
            if color_temperature
            else DeviceProfile.DIMMABLE_LIGHT
        )
    if domain == "cover":
        return DeviceProfile.COVER
    if domain == "climate":
        return DeviceProfile.THERMOSTAT
    raise ValueError(f"unsupported source domain: {domain}")
