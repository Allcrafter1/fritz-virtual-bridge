# Architecture and user workflow

## Components

The system has three deliberately separate layers.

### Freetz-NG package

The package runs on a dedicated supported FRITZ!Box and contains only project
code:

- a small provider loaded into the existing `aha` process,
- a separate MQTT and persistence process,
- a local control client,
- a Freetz init script and configuration page.

The provider reuses the original FRITZ!OS Smart Home and Mesh stack. It does
not implement DECT radio and does not replace an AVM executable. The init
script starts `aha` with the provider only after verifying the exact supported
binary fingerprint. Stopping the package restarts the original process
without the provider.

### Home Assistant custom integration

One config entry represents one FRITZ! Virtual Bridge. Every mapped Home
Assistant entity is stored as a config subentry. This gives the user normal UI
flows to add, rename, reconfigure and remove mappings without YAML or a second
web interface.

The integration does not create a second controllable copy of the selected
light, switch, cover or climate entity. It translates between the stable
virtual-device endpoint and the original entity's native Home Assistant
actions and state attributes.

### MQTT transport

MQTT carries management, FRITZ-originated commands and confirmed Home
Assistant state. Messages are local, versioned and independent of Home
Assistant's internal storage format. Retained configuration and state allow
both sides to recover after restart in either order.

## Device identity and lifecycle

Each mapping receives an immutable random 64-bit identifier. Its FRITZ UID is
`FVB` followed by sixteen hexadecimal characters, which fits the native
19-character field. The following values are allocated once and persisted on
the bridge:

- FRITZ UID,
- provider remote ID,
- device profile,
- HAN-FUN IPUI/unit discriminator where applicable,
- user-visible device name,
- enabled/disabled state.

Renaming or binding a replacement Home Assistant entity keeps the FRITZ UID
and all radio-side identities. Existing 440 assignments therefore survive.
Changing the device profile creates a new virtual device because FRITZ!OS
capabilities and widget layouts are type-specific.

Removing a mapping is authoritative: the bridge calls the local native AHA
deletion path, removes the provider endpoint and then removes the persistent
registry entry. Existing controller assignments for that virtual device are
therefore removed with it. Provider IDs are never reused.

## Management protocol

The initial public protocol uses schema version 1 beneath
`fritzvirtual/<bridge_id>`:

| Topic | Direction | Purpose |
|---|---|---|
| `bridge/availability` | bridge -> HA | retained online/offline status |
| `bridge/info` | bridge -> HA | retained version, capabilities and compatibility |
| `management/request` | HA -> bridge | versioned upsert/rename/prune request |
| `management/response/<request_id>` | bridge -> HA | correlated accepted/error response |
| `device/<uid>/command` | bridge -> HA | commands from FRITZ!OS or the 440 |
| `device/<uid>/<property>/set` | HA -> bridge | confirmed HA state and configuration |

Management JSON is validated and persisted by the separate MQTT process. The
provider inside `aha` receives only a bounded, validated local snapshot and
never parses MQTT credentials or writes persistent files.

The exact version-1 messages, validation rules and recovery semantics are in
[MQTT protocol version 1](mqtt-protocol.md).

## Supported profiles

The first usable release targets:

| HA source | Virtual FRITZ profile | Functions |
|---|---|---|
| `switch`, `input_boolean` | switch/socket | on/off |
| `light` | dimmable or color-temperature light | on/off, brightness, optional color temperature |
| `cover` | blind | open, close, stop, position |
| `climate` | radiator thermostat | mode, setpoint, Boost/cold timer, optional schedule shadow |

Free HS color remains an experimental capability until the 440 palette and
arbitrary Home Assistant color gamuts have a predictable mapping.

The thermostat schedule adapter is optional. The first adapter supports the
validated Zigbee2MQTT `5+2` workday/holiday schedule entities. The internal
schedule model is generic so later adapters can support other thermostat
integrations without changing the FRITZ protocol.

For the standard Zigbee2MQTT entity naming scheme the integration detects the
`select.*_week`, `text.*_workdays_schedule` and
`text.*_holidays_schedule` siblings automatically. It publishes a rolling
current/next transition once per minute only while the real thermostat uses
the `schedule` preset and the week selector is `5+2`. Editing the virtual
schedule in FRITZ!OS is rejected by the provider and the HA-owned shadow is
restored.

Boost and cold overrides are kept in a private atomic Home Assistant store.
The first override records the previous temperature, HVAC mode and preset.
Extending or switching the override retains that original state. Expiry,
manual cancellation and a Home Assistant restart restore it without visible
input helpers.

## User workflow

1. Install **FRITZ! Virtual Bridge**. Home Assistant creates an integration
   entry automatically from the retained MQTT bridge announcement and shows an
   onboarding notification explaining the otherwise icon-only add action.
2. Open the bridge entry and use the `+` action in the top-right corner. The
   following dialog is titled **Add virtual FRITZ! device**.
3. Select a source entity. The integration proposes the matching profile and
   capabilities; the user can reduce optional capabilities.
4. Choose a FRITZ device name. The immutable endpoint is created and confirmed
   by the bridge.
5. Follow the final wizard page's direct FRITZ!OS link and place or configure
   the device there. The persistent completion notification and the bridge
   device named **Open the FRITZ!OS interface** remain permanent alternatives.
   Screen layout and its short display label remain FRITZ!OS responsibilities.
6. Reconfigure a mapping to bind a replacement HA entity while retaining the
   FRITZ identity and 440 assignments.

When a mapped thermostat has no recognized schedule adapter, setpoint, mode,
Boost and cold continue to work; only the native next-change schedule display
stays disabled.

## Latency topology

The MQTT leg is local push. In the current validation topology, however, the
440 is paired with a 6690 Mesh master while virtual endpoints originate on a
7530 Mesh repeater. Measured absolute dim commands all reached Home Assistant
with the correct value, but the Mesh path serialized them at roughly 6–7
seconds each. The confirmed Home Assistant state reached the FRITZ entity about
0.6 seconds after the source entity changed.

Fast interactive control therefore requires the controller and virtual
provider to use the same bridge box. Pairing the 440 directly with the
dedicated 7530 removes the master-to-repeater command leg; that topology is the
next latency qualification target. Rapid repeated commands in the Mesh test
topology can appear lost while earlier commands are still in transit.

## Safety and rollback

- Only explicitly fingerprinted FRITZ!OS/`aha` builds are accepted.
- Provider input lengths, IDs and enum values are bounded before use.
- MQTT credentials remain in root-readable Freetz configuration.
- Unknown management schema versions are rejected.
- Stopping the Freetz service restarts stock `aha` without `LD_PRELOAD`.
- Removing the package requires building/flashing a normal Freetz or AVM
  image; no AVM file is patched in place at runtime.
- The productive Mesh master is never modified by the installation.
