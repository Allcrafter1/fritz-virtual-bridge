# Changelog

## 0.1.0 - 2026-10-04

First public test release.

- Expose Home Assistant switches, lights, covers and climate entities as
  persistent virtual FRITZ! Smart Home devices.
- Carry commands from FRITZ!OS and FRITZ!Smart Control 440 to Home Assistant
  and confirmed state back over local MQTT.
- Support dimming, color temperature, cover position, thermostat setpoints,
  timed Boost/cold modes and the tested Zigbee2MQTT schedule shadow.
- Add MQTT discovery and Home Assistant UI flows for creating, rebinding and
  deleting virtual devices.
- Add a Freetz web page for broker credentials and bridge configuration.
- Restrict provider injection to the validated FRITZ!Box 7530 / FRITZ!OS 8.25
  `aha` fingerprint.

Known limitation: commands routed through a separate 6690 Mesh master took
roughly 6–7 seconds in the laboratory. Pair the controller directly with the
dedicated bridge box for the intended topology; its final latency measurement
is still pending.
