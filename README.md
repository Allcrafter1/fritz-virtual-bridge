# FRITZ! Virtual Bridge

FRITZ! Virtual Bridge makes selected Home Assistant entities available to a
FRITZ!Box as native-looking Smart Home devices. They can then be assigned to a
FRITZ!Smart Control 440 in the normal FRITZ!OS user interface.

The project is an experimental source-only bridge built from a working
laboratory proof of concept. The validated hardware and firmware combination
is:

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

1. Build a Freetz-NG image locally from an original AVM firmware image and the
   package supplied by this repository.
2. Configure the bridge's MQTT connection in the Freetz web interface.
3. Install the Home Assistant custom integration.
4. Add the bridge once, then add one mapping per Home Assistant entity.
5. Assign the generated virtual devices to one or more 440 controllers in
   FRITZ!OS.

No AVM firmware, AVM program or modified firmware image is distributed by
this repository. Users build and install their own image. See
[Licensing and boundaries](docs/licensing.md) before installing anything.

The guided local build, HACS setup and rollback procedure is documented in
[Installation](docs/installation.md).

## Status

The source is currently pre-release software. The dynamic provider, persistent
registry, MQTT discovery and Home Assistant mapping flow have completed an
end-to-end laboratory migration test. A clean source build and direct-pairing
latency qualification remain before the first release. The compatibility guard
refuses to inject the provider into an unknown `aha` binary.

The agreed product architecture and user workflow are documented in
[Architecture](docs/architecture.md). Laboratory protocol evidence remains in
the separate development workspace and will be reduced to publishable
protocol documentation before the first release.

The bridge currently supports at most 32 persistent virtual devices. Removing
a Home Assistant mapping disables its FRITZ device and preserves its identity;
the project does not yet expose permanent deletion.

The current Mesh-master test topology adds substantial command latency. Tests
through a 6690 Mesh master to the 7530 bridge consistently delivered absolute
brightness values, but took roughly 6–7 seconds per command. MQTT and Home
Assistant state feedback accounted for less than one second of that path. A
440 paired directly with the bridge box is the intended low-latency topology
and still needs a dedicated qualification run.

## License

Original project code is licensed under the permissive MIT License. Third-party
components retain their own licenses. See [LICENSE](LICENSE) and
[NOTICE](NOTICE).
