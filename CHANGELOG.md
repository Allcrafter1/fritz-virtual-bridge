# Changelog

## 0.1.3 - 2026-10-04

- Restore each device's persisted remote ID explicitly after provider restarts,
  including allocation gaps left by deleted devices. Previously an ID mismatch
  stopped provisioning before the lamp unit was created.
- Correct the HAN-FUN interface-list byte order for the local 7530 / 8.25
  receiver. A device could previously appear by name but have no assignable
  lamp or blind unit for a directly paired controller.
- Add regression coverage for durable IDs, collisions and local interface
  encoding. Verified native dimmable unit creation on the laboratory 7530.
- This fix requires rebuilding/updating the FRITZ!Box package; updating only
  the Home Assistant integration does not replace the provider.

## 0.1.2 - 2026-10-04

- Detect an unresponsive internal provider control channel and disconnect the
  stale watcher instead of silently accepting unusable MQTT state.
- Add a local watchdog that restarts only the isolated FRITZ!OS Smart Home
  process after three failed health checks; internet and network services are
  not restarted.
- Bound local control-socket connection attempts so a full or stale queue
  cannot block recovery indefinitely.

## 0.1.1 - 2026-10-04

- Reworked the README into a problem-first onboarding path for FRITZ!Smart
  Control 440 users and added a complete German entry page.
- Documented the practically tested Windows/WSL2 setup, including a copyable
  Codex-assisted installation prompt.
- Clarified the recommended dedicated Mosquitto-app login and the separate MQTT
  credentials used by the bridge.
- Fixed the Freetz service status check to use the MQTT bridge PID file.
- Fixed missing labels in the Freetz package UI by declaring its translated
  CGI file to the Freetz language build step.
- Made the Freetz installer derive the source archive version from the package
  definition so release bumps cannot leave a stale archive name behind.
- Documented update isolation, two-part backups and direct 440 pairing with the
  bridge box.

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
