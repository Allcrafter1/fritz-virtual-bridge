# FRITZ! Virtual Bridge

FRITZ! Virtual Bridge makes selected Home Assistant entities available to a
FRITZ!Box as native-looking Smart Home devices. They can then be assigned to a
FRITZ!Smart Control 440 in the normal FRITZ!OS user interface.

The project is an experimental source-only bridge built from a working
laboratory proof of concept. Version 0.1.0 is the first public test release;
it deliberately supports one exact hardware and firmware combination:

- FRITZ!Box 7530 (classic/HW236) as a dedicated bridge
- FRITZ!OS 8.25 with Freetz-NG
- an unmodified FRITZ!Box 6690 Cable running FRITZ!OS 8.25 as Mesh master
- FRITZ!Smart Control 440 firmware 05.45
- Home Assistant with the MQTT integration

The proof of concept already supports bidirectional switching, dimming, color
temperature, cover control, thermostat setpoints, Boost/cold timers and a
Home-Assistant-owned thermostat schedule shadow with the native 440
`next change` display.

## Installation model

1. Connect a compatible dedicated 7530 directly to a Linux computer and run
   the guided local build/installation script.
2. Configure IP-client/Mesh operation in FRITZ!OS and the MQTT connection in
   the Freetz web interface. Normal use requires no SSH or file editing.
3. Install the Home Assistant custom integration through HACS.
4. Add the bridge once, then add one mapping per Home Assistant entity.
5. Follow the final wizard page or its persistent notification into FRITZ!OS
   and assign or configure the generated virtual devices there.

No AVM firmware, AVM program or modified firmware image is distributed by
this repository. Users build and install their own image. See
[Licensing and boundaries](docs/licensing.md) before installing anything.

The guided local build, HACS setup and rollback procedure is documented in
[Installation](docs/installation.md).

## Status

The source is experimental software. The dynamic provider, persistent registry,
MQTT discovery and Home Assistant mapping flow have completed an end-to-end
laboratory migration test, including a clean source build. Direct pairing of
the 440 with the bridge box still needs a dedicated latency qualification. The
compatibility guard refuses to inject the provider into an unknown `aha`
binary.

The agreed product architecture and user workflow are documented in
[Architecture](docs/architecture.md). The publishable MQTT contract is in
[MQTT protocol version 1](docs/mqtt-protocol.md); raw laboratory captures and
AVM binaries are deliberately excluded from the repository.

The bridge currently supports at most 32 persistent virtual devices. Removing
a Home Assistant mapping also removes its native FRITZ!OS device and releases
the corresponding bridge registry slot. Allocated provider IDs are not reused.

The current Mesh-master test topology adds substantial command latency. Tests
through a 6690 Mesh master to the 7530 bridge consistently delivered absolute
brightness values, but took roughly 6–7 seconds per command. MQTT and Home
Assistant state feedback accounted for less than one second of that path. A
440 paired directly with the bridge box is the intended low-latency topology
and still needs a dedicated qualification run.

## License

Original project code is dual-licensed under your choice of the permissive
[Apache License 2.0](LICENSE) or [MIT License](LICENSE-MIT). The MIT option keeps
the Freetz-NG package compatible with GPL-2.0-only code. Third-party components
retain their own licenses. See [NOTICE](NOTICE).
