# SPDX-License-Identifier: MIT
"""Tests for Home Assistant/protocol translation."""

import pytest

from custom_components.fritz_virtual_bridge.const import DeviceProfile
from custom_components.fritz_virtual_bridge.protocol import (
    ProtocolError,
    feedback_for_state,
    parse_command,
    service_call_for_command,
)

UID = "FVB0123456789ABCDEF"


def command(body: str):
    """Parse a test command."""
    return parse_command(body, UID)


def test_light_command_maps_level_to_brightness_percentage() -> None:
    parsed = command(
        '{"event":"command","endpoint":"FVB0123456789ABCDEF",'
        '"state":1,"level":42,"sequence":7}'
    )
    assert service_call_for_command(
        DeviceProfile.DIMMABLE_LIGHT, "light.desk", parsed
    ) == ("light", "turn_on", {"entity_id": "light.desk", "brightness_pct": 42})


def test_color_temperature_uses_current_kelvin_action() -> None:
    parsed = command(
        '{"event":"command","endpoint":"FVB0123456789ABCDEF",'
        '"state":1,"color_temperature":4200,"sequence":7}'
    )
    assert service_call_for_command(
        DeviceProfile.COLOR_TEMPERATURE_LIGHT, "light.desk", parsed
    ) == (
        "light",
        "turn_on",
        {"entity_id": "light.desk", "color_temp_kelvin": 4200},
    )


def test_color_temperature_feedback_uses_confirmed_value() -> None:
    assert feedback_for_state(
        DeviceProfile.COLOR_TEMPERATURE_LIGHT,
        "on",
        {"brightness": 128, "color_temp_kelvin": 3003},
    ) == {"state": "ON", "level": "50", "color_temperature": "3003"}


@pytest.mark.parametrize(
    ("action", "service"),
    [("open", "open_cover"), ("close", "close_cover"), ("stop", "stop_cover")],
)
def test_cover_actions(action: str, service: str) -> None:
    parsed = command(
        f'{{"event":"command","endpoint":"{UID}","action":"{action}","sequence":8}}'
    )
    assert (
        service_call_for_command(DeviceProfile.COVER, "cover.blind", parsed)[1]
        == service
    )


def test_state_feedback_rounds_climate_to_half_degrees() -> None:
    assert feedback_for_state(
        DeviceProfile.THERMOSTAT, "heat", {"temperature": 20.26}
    ) == {"hvac_mode": "heat", "target_temperature": "20.5"}


def test_parser_rejects_topic_payload_endpoint_mismatch() -> None:
    with pytest.raises(ProtocolError):
        parse_command(
            '{"event":"command","endpoint":"FVBFFFFFFFFFFFFFFFF","state":1,"sequence":1}',
            UID,
        )
